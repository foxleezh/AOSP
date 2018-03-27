/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jni_internal.h"

#include <dlfcn.h>

#include <cstdarg>
#include <memory>
#include <utility>
#include <vector>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "atomic.h"
#include "base/allocator.h"
#include "base/enums.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "fault_handler.h"
#include "gc_root.h"
#include "gc/accounting/card_table-inl.h"
#include "indirect_reference_table-inl.h"
#include "interpreter/interpreter.h"
#include "jni_env_ext.h"
#include "java_vm_ext.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/field-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "parsed_options.h"
#include "reflection.h"
#include "runtime.h"
#include "safe_map.h"
#include "scoped_thread_state_change-inl.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "utf.h"
#include "well_known_classes.h"

namespace art {

// Consider turning this on when there is errors which could be related to JNI array copies such as
// things not rendering correctly. E.g. b/16858794
static constexpr bool kWarnJniAbort = false;

// Section 12.3.2 of the JNI spec describes JNI class descriptors. They're
// separated with slashes but aren't wrapped with "L;" like regular descriptors
// (i.e. "a/b/C" rather than "La/b/C;"). Arrays of reference types are an
// exception; there the "L;" must be present ("[La/b/C;"). Historically we've
// supported names with dots too (such as "a.b.C").
static std::string NormalizeJniClassDescriptor(const char* name) {
  std::string result;
  // Add the missing "L;" if necessary.
  if (name[0] == '[') {
    result = name;
  } else {
    result += 'L';
    result += name;
    result += ';';
  }
  // Rewrite '.' as '/' for backwards compatibility.
  if (result.find('.') != std::string::npos) {
    LOG(WARNING) << "Call to JNI FindClass with dots in name: "
                 << "\"" << name << "\"";
    std::replace(result.begin(), result.end(), '.', '/');
  }
  return result;
}

static void ThrowNoSuchMethodError(ScopedObjectAccess& soa,
                                   ObjPtr<mirror::Class> c,
                                   const char* name,
                                   const char* sig,
                                   const char* kind)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string temp;
  soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
                                 "no %s method \"%s.%s%s\"",
                                 kind,
                                 c->GetDescriptor(&temp),
                                 name,
                                 sig);
}

static void ReportInvalidJNINativeMethod(const ScopedObjectAccess& soa,
                                         ObjPtr<mirror::Class> c,
                                         const char* kind,
                                         jint idx,
                                         bool return_errors)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(return_errors ? ::android::base::ERROR : ::android::base::FATAL)
      << "Failed to register native method in " << c->PrettyDescriptor()
      << " in " << c->GetDexCache()->GetLocation()->ToModifiedUtf8()
      << ": " << kind << " is null at index " << idx;
  soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
                                 "%s is null at index %d",
                                 kind,
                                 idx);
}

static ObjPtr<mirror::Class> EnsureInitialized(Thread* self, ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (LIKELY(klass->IsInitialized())) {
    return klass;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_klass, true, true)) {
    return nullptr;
  }
  return h_klass.Get();
}

static jmethodID FindMethodID(ScopedObjectAccess& soa, jclass jni_class,
                              const char* name, const char* sig, bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(jni_class));
  if (c == nullptr) {
    return nullptr;
  }
  ArtMethod* method = nullptr;
  auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  if (is_static) {
    method = c->FindDirectMethod(name, sig, pointer_size);
  } else if (c->IsInterface()) {
    method = c->FindInterfaceMethod(name, sig, pointer_size);
  } else {
    method = c->FindVirtualMethod(name, sig, pointer_size);
    if (method == nullptr) {
      // No virtual method matching the signature.  Search declared
      // private methods and constructors.
      method = c->FindDeclaredDirectMethod(name, sig, pointer_size);
    }
  }
  if (method == nullptr || method->IsStatic() != is_static) {
    ThrowNoSuchMethodError(soa, c, name, sig, is_static ? "static" : "non-static");
    return nullptr;
  }
  return jni::EncodeArtMethod(method);
}

static ObjPtr<mirror::ClassLoader> GetClassLoader(const ScopedObjectAccess& soa)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* method = soa.Self()->GetCurrentMethod(nullptr);
  // If we are running Runtime.nativeLoad, use the overriding ClassLoader it set.
  if (method == jni::DecodeArtMethod(WellKnownClasses::java_lang_Runtime_nativeLoad)) {
    return soa.Decode<mirror::ClassLoader>(soa.Self()->GetClassLoaderOverride());
  }
  // If we have a method, use its ClassLoader for context.
  if (method != nullptr) {
    return method->GetDeclaringClass()->GetClassLoader();
  }
  // We don't have a method, so try to use the system ClassLoader.
  ObjPtr<mirror::ClassLoader> class_loader =
      soa.Decode<mirror::ClassLoader>(Runtime::Current()->GetSystemClassLoader());
  if (class_loader != nullptr) {
    return class_loader;
  }
  // See if the override ClassLoader is set for gtests.
  class_loader = soa.Decode<mirror::ClassLoader>(soa.Self()->GetClassLoaderOverride());
  if (class_loader != nullptr) {
    // If so, CommonCompilerTest should have marked the runtime as a compiler not compiling an
    // image.
    CHECK(Runtime::Current()->IsAotCompiler());
    CHECK(!Runtime::Current()->IsCompilingBootImage());
    return class_loader;
  }
  // Use the BOOTCLASSPATH.
  return nullptr;
}

static jfieldID FindFieldID(const ScopedObjectAccess& soa, jclass jni_class, const char* name,
                            const char* sig, bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(jni_class))));
  if (c == nullptr) {
    return nullptr;
  }
  ArtField* field = nullptr;
  mirror::Class* field_type;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (sig[1] != '\0') {
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(c->GetClassLoader()));
    field_type = class_linker->FindClass(soa.Self(), sig, class_loader);
  } else {
    field_type = class_linker->FindPrimitiveClass(*sig);
  }
  if (field_type == nullptr) {
    // Failed to find type from the signature of the field.
    DCHECK(soa.Self()->IsExceptionPending());
    StackHandleScope<1> hs2(soa.Self());
    Handle<mirror::Throwable> cause(hs2.NewHandle(soa.Self()->GetException()));
    soa.Self()->ClearException();
    std::string temp;
    soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                   "no type \"%s\" found and so no field \"%s\" "
                                   "could be found in class \"%s\" or its superclasses", sig, name,
                                   c->GetDescriptor(&temp));
    soa.Self()->GetException()->SetCause(cause.Get());
    return nullptr;
  }
  std::string temp;
  if (is_static) {
    field = mirror::Class::FindStaticField(
        soa.Self(), c.Get(), name, field_type->GetDescriptor(&temp));
  } else {
    field = c->FindInstanceField(name, field_type->GetDescriptor(&temp));
  }
  if (field == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                   "no \"%s\" field \"%s\" in class \"%s\" or its superclasses",
                                   sig, name, c->GetDescriptor(&temp));
    return nullptr;
  }
  return jni::EncodeArtField(field);
}

static void ThrowAIOOBE(ScopedObjectAccess& soa, mirror::Array* array, jsize start,
                        jsize length, const char* identifier)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string type(array->PrettyTypeOf());
  soa.Self()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                 "%s offset=%d length=%d %s.length=%d",
                                 type.c_str(), start, length, identifier, array->GetLength());
}

static void ThrowSIOOBE(ScopedObjectAccess& soa, jsize start, jsize length,
                        jsize array_length)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  soa.Self()->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
                                 "offset=%d length=%d string.length()=%d", start, length,
                                 array_length);
}

int ThrowNewException(JNIEnv* env, jclass exception_class, const char* msg, jobject cause)
    REQUIRES(!Locks::mutator_lock_) {
  // Turn the const char* into a java.lang.String.
  ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg));
  if (msg != nullptr && s.get() == nullptr) {
    return JNI_ERR;
  }

  // Choose an appropriate constructor and set up the arguments.
  jvalue args[2];
  const char* signature;
  if (msg == nullptr && cause == nullptr) {
    signature = "()V";
  } else if (msg != nullptr && cause == nullptr) {
    signature = "(Ljava/lang/String;)V";
    args[0].l = s.get();
  } else if (msg == nullptr && cause != nullptr) {
    signature = "(Ljava/lang/Throwable;)V";
    args[0].l = cause;
  } else {
    signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    args[0].l = s.get();
    args[1].l = cause;
  }
  jmethodID mid = env->GetMethodID(exception_class, "<init>", signature);
  if (mid == nullptr) {
    ScopedObjectAccess soa(env);
    LOG(ERROR) << "No <init>" << signature << " in "
        << mirror::Class::PrettyClass(soa.Decode<mirror::Class>(exception_class));
    return JNI_ERR;
  }

  ScopedLocalRef<jthrowable> exception(
      env, reinterpret_cast<jthrowable>(env->NewObjectA(exception_class, mid, args)));
  if (exception.get() == nullptr) {
    return JNI_ERR;
  }
  ScopedObjectAccess soa(env);
  soa.Self()->SetException(soa.Decode<mirror::Throwable>(exception.get()));
  return JNI_OK;
}

static JavaVMExt* JavaVmExtFromEnv(JNIEnv* env) {
  return reinterpret_cast<JNIEnvExt*>(env)->vm;
}

#define CHECK_NON_NULL_ARGUMENT(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, nullptr)

#define CHECK_NON_NULL_ARGUMENT_RETURN_VOID(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, )

#define CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, 0)

#define CHECK_NON_NULL_ARGUMENT_RETURN(value, return_val) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, return_val)

#define CHECK_NON_NULL_ARGUMENT_FN_NAME(name, value, return_val) \
  if (UNLIKELY((value) == nullptr)) { \
    JavaVmExtFromEnv(env)->JniAbortF(name, #value " == null"); \
    return return_val; \
  }

#define CHECK_NON_NULL_MEMCPY_ARGUMENT(length, value) \
  if (UNLIKELY((length) != 0 && (value) == nullptr)) { \
    JavaVmExtFromEnv(env)->JniAbortF(__FUNCTION__, #value " == null"); \
    return; \
  }

template <bool kNative>
static ArtMethod* FindMethod(mirror::Class* c, const StringPiece& name, const StringPiece& sig)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  for (auto& method : c->GetMethods(pointer_size)) {
    if (kNative == method.IsNative() && name == method.GetName() && method.GetSignature() == sig) {
      return &method;
    }
  }
  return nullptr;
}

class JNI {
 public:
  static jint GetVersion(JNIEnv*) {
    return JNI_VERSION_1_6;
  }

  static jclass DefineClass(JNIEnv*, const char*, jobject, const jbyte*, jsize) {
    LOG(WARNING) << "JNI DefineClass is not supported";
    return nullptr;
  }

  static jclass FindClass(JNIEnv* env, const char* name) {
    CHECK_NON_NULL_ARGUMENT(name);
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    std::string descriptor(NormalizeJniClassDescriptor(name));
    ScopedObjectAccess soa(env);
    mirror::Class* c = nullptr;
    if (runtime->IsStarted()) {
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::ClassLoader> class_loader(hs.NewHandle(GetClassLoader(soa)));
      c = class_linker->FindClass(soa.Self(), descriptor.c_str(), class_loader);
    } else {
      c = class_linker->FindSystemClass(soa.Self(), descriptor.c_str());
    }
    return soa.AddLocalReference<jclass>(c);
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject jlr_method) {
    CHECK_NON_NULL_ARGUMENT(jlr_method);
    ScopedObjectAccess soa(env);
    return jni::EncodeArtMethod(ArtMethod::FromReflectedMethod(soa, jlr_method));
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject jlr_field) {
    CHECK_NON_NULL_ARGUMENT(jlr_field);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> obj_field = soa.Decode<mirror::Object>(jlr_field);
    if (obj_field->GetClass() != mirror::Field::StaticClass()) {
      // Not even a java.lang.reflect.Field, return null. TODO, is this check necessary?
      return nullptr;
    }
    ObjPtr<mirror::Field> field = ObjPtr<mirror::Field>::DownCast(obj_field);
    return jni::EncodeArtField(field->GetArtField());
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ArtMethod* m = jni::DecodeArtMethod(mid);
    mirror::Executable* method;
    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
    DCHECK(!Runtime::Current()->IsActiveTransaction());
    if (m->IsConstructor()) {
      method = mirror::Constructor::CreateFromArtMethod<kRuntimePointerSize, false>(soa.Self(), m);
    } else {
      method = mirror::Method::CreateFromArtMethod<kRuntimePointerSize, false>(soa.Self(), m);
    }
    return soa.AddLocalReference<jobject>(method);
  }

  static jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField(fid);
    return soa.AddLocalReference<jobject>(
        mirror::Field::CreateFromArtField<kRuntimePointerSize>(soa.Self(), f, true));
  }

  static jclass GetObjectClass(JNIEnv* env, jobject java_object) {
    CHECK_NON_NULL_ARGUMENT(java_object);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    return soa.AddLocalReference<jclass>(o->GetClass());
  }

  static jclass GetSuperclass(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);
    return soa.AddLocalReference<jclass>(c->IsInterface() ? nullptr : c->GetSuperClass());
  }

  // Note: java_class1 should be safely castable to java_class2, and
  // not the other way around.
  static jboolean IsAssignableFrom(JNIEnv* env, jclass java_class1, jclass java_class2) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class1, JNI_FALSE);
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class2, JNI_FALSE);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c1 = soa.Decode<mirror::Class>(java_class1);
    ObjPtr<mirror::Class> c2 = soa.Decode<mirror::Class>(java_class2);
    return c2->IsAssignableFrom(c1) ? JNI_TRUE : JNI_FALSE;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_FALSE);
    if (jobj == nullptr) {
      // Note: JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(jobj);
      ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);
      return obj->InstanceOf(c) ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jint Throw(JNIEnv* env, jthrowable java_exception) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Throwable> exception = soa.Decode<mirror::Throwable>(java_exception);
    if (exception == nullptr) {
      return JNI_ERR;
    }
    soa.Self()->SetException(exception);
    return JNI_OK;
  }

  static jint ThrowNew(JNIEnv* env, jclass c, const char* msg) {
    CHECK_NON_NULL_ARGUMENT_RETURN(c, JNI_ERR);
    return ThrowNewException(env, c, msg, nullptr);
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    return static_cast<JNIEnvExt*>(env)->self->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
  }

  static void ExceptionClear(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    soa.Self()->ClearException();
  }

  static void ExceptionDescribe(JNIEnv* env) {
    ScopedObjectAccess soa(env);

    // If we have no exception to describe, pass through.
    if (!soa.Self()->GetException()) {
      return;
    }

    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Throwable> old_exception(
        hs.NewHandle<mirror::Throwable>(soa.Self()->GetException()));
    soa.Self()->ClearException();
    ScopedLocalRef<jthrowable> exception(env,
                                         soa.AddLocalReference<jthrowable>(old_exception.Get()));
    ScopedLocalRef<jclass> exception_class(env, env->GetObjectClass(exception.get()));
    jmethodID mid = env->GetMethodID(exception_class.get(), "printStackTrace", "()V");
    if (mid == nullptr) {
      LOG(WARNING) << "JNI WARNING: no printStackTrace()V in "
                   << mirror::Object::PrettyTypeOf(old_exception.Get());
    } else {
      env->CallVoidMethod(exception.get(), mid);
      if (soa.Self()->IsExceptionPending()) {
        LOG(WARNING) << "JNI WARNING: " << mirror::Object::PrettyTypeOf(soa.Self()->GetException())
                     << " thrown while calling printStackTrace";
        soa.Self()->ClearException();
      }
    }
    soa.Self()->SetException(old_exception.Get());
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    mirror::Object* exception = soa.Self()->GetException();
    return soa.AddLocalReference<jthrowable>(exception);
  }

  static void FatalError(JNIEnv*, const char* msg) {
    LOG(FATAL) << "JNI FatalError called: " << msg;
  }

  static jint PushLocalFrame(JNIEnv* env, jint capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    if (EnsureLocalCapacityInternal(soa, capacity, "PushLocalFrame") != JNI_OK) {
      return JNI_ERR;
    }
    down_cast<JNIEnvExt*>(env)->PushFrame(capacity);
    return JNI_OK;
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject java_survivor) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> survivor = soa.Decode<mirror::Object>(java_survivor);
    soa.Env()->PopFrame();
    return soa.AddLocalReference<jobject>(survivor);
  }

  static jint EnsureLocalCapacity(JNIEnv* env, jint desired_capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    return EnsureLocalCapacityInternal(soa, desired_capacity, "EnsureLocalCapacity");
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    return soa.Vm()->AddGlobalRef(soa.Self(), decoded_obj);
  }

  static void DeleteGlobalRef(JNIEnv* env, jobject obj) {
    JavaVMExt* vm = down_cast<JNIEnvExt*>(env)->vm;
    Thread* self = down_cast<JNIEnvExt*>(env)->self;
    vm->DeleteGlobalRef(self, obj);
  }

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    return soa.Vm()->AddWeakGlobalRef(soa.Self(), decoded_obj);
  }

  static void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
    JavaVMExt* vm = down_cast<JNIEnvExt*>(env)->vm;
    Thread* self = down_cast<JNIEnvExt*>(env)->self;
    vm->DeleteWeakGlobalRef(self, obj);
  }

  static jobject NewLocalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    // Check for null after decoding the object to handle cleared weak globals.
    if (decoded_obj == nullptr) {
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(decoded_obj);
  }

  static void DeleteLocalRef(JNIEnv* env, jobject obj) {
    if (obj == nullptr) {
      return;
    }
    // SOA is only necessary to have exclusion between GC root marking and removing.
    // We don't want to have the GC attempt to mark a null root if we just removed
    // it. b/22119403
    ScopedObjectAccess soa(env);
    auto* ext_env = down_cast<JNIEnvExt*>(env);
    if (!ext_env->locals.Remove(ext_env->local_ref_cookie, obj)) {
      // Attempting to delete a local reference that is not in the
      // topmost local reference frame is a no-op.  DeleteLocalRef returns
      // void and doesn't throw any exceptions, but we should probably
      // complain about it so the user will notice that things aren't
      // going quite the way they expect.
      LOG(WARNING) << "JNI WARNING: DeleteLocalRef(" << obj << ") "
                   << "failed to find entry";
    }
  }

  static jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
    if (obj1 == obj2) {
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      return (soa.Decode<mirror::Object>(obj1) == soa.Decode<mirror::Object>(obj2))
              ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jobject AllocObject(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
      return soa.AddLocalReference<jobject>(mirror::String::AllocEmptyString<true>(soa.Self(),
                                                                              allocator_type));
    }
    return soa.AddLocalReference<jobject>(c->AllocObject(soa.Self()));
  }

  static jobject NewObject(JNIEnv* env, jclass java_class, jmethodID mid, ...) {
    va_list args;
    va_start(args, mid);
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    jobject result = NewObjectV(env, java_class, mid, args);
    va_end(args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env, jclass java_class, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(),
                                                soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      // Replace calls to String.<init> with equivalent StringFactory call.
      jmethodID sf_mid = jni::EncodeArtMethod(
          WellKnownClasses::StringInitToStringFactory(jni::DecodeArtMethod(mid)));
      return CallStaticObjectMethodV(env, WellKnownClasses::java_lang_StringFactory, sf_mid, args);
    }
    ObjPtr<mirror::Object> result = c->AllocObject(soa.Self());
    if (result == nullptr) {
      return nullptr;
    }
    jobject local_result = soa.AddLocalReference<jobject>(result);
    CallNonvirtualVoidMethodV(env, local_result, java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return local_result;
  }

  static jobject NewObjectA(JNIEnv* env, jclass java_class, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(),
                                                soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      // Replace calls to String.<init> with equivalent StringFactory call.
      jmethodID sf_mid = jni::EncodeArtMethod(
          WellKnownClasses::StringInitToStringFactory(jni::DecodeArtMethod(mid)));
      return CallStaticObjectMethodA(env, WellKnownClasses::java_lang_StringFactory, sf_mid, args);
    }
    ObjPtr<mirror::Object> result = c->AllocObject(soa.Self());
    if (result == nullptr) {
      return nullptr;
    }
    jobject local_result = soa.AddLocalReference<jobjectArray>(result);
    CallNonvirtualVoidMethodA(env, local_result, java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return local_result;
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID(soa, java_class, name, sig, false);
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass java_class, const char* name,
                                     const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID(soa, java_class, name, sig, true);
  }

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallBooleanMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallBooleanMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetZ();
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallByteMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallByteMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetB();
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallCharMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallCharMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetC();
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallDoubleMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallDoubleMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetD();
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallFloatMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallFloatMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetF();
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallIntMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallIntMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetI();
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallLongMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallLongMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetJ();
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallShortMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallShortMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetS();
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap);
    va_end(ap);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    jobject local_result = soa.AddLocalReference<jobject>(result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                              ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetZ();
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetB();
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetC();
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetS();
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetI();
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetJ();
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetF();
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetD();
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, ap);
    va_end(ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, obj, mid, args);
  }

  static jfieldID GetFieldID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID(soa, java_class, name, sig, false);
  }

  static jfieldID GetStaticFieldID(JNIEnv* env, jclass java_class, const char* name,
                                   const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID(soa, java_class, name, sig, true);
  }

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(obj);
    ArtField* f = jni::DecodeArtField(fid);
    return soa.AddLocalReference<jobject>(f->GetObject(o));
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField(fid);
    return soa.AddLocalReference<jobject>(f->GetObject(f->GetDeclaringClass()));
  }

  static void SetObjectField(JNIEnv* env, jobject java_object, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_object);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    ObjPtr<mirror::Object> v = soa.Decode<mirror::Object>(java_value);
    ArtField* f = jni::DecodeArtField(fid);
    f->SetObject<false>(o, v);
  }

  static void SetStaticObjectField(JNIEnv* env, jclass, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> v = soa.Decode<mirror::Object>(java_value);
    ArtField* f = jni::DecodeArtField(fid);
    f->SetObject<false>(f->GetDeclaringClass(), v);
  }

#define GET_PRIMITIVE_FIELD(fn, instance) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(instance); \
  ArtField* f = jni::DecodeArtField(fid); \
  return f->Get ##fn (o)

#define GET_STATIC_PRIMITIVE_FIELD(fn) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField(fid); \
  return f->Get ##fn (f->GetDeclaringClass())

#define SET_PRIMITIVE_FIELD(fn, instance, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(instance); \
  ArtField* f = jni::DecodeArtField(fid); \
  f->Set ##fn <false>(o, value)

#define SET_STATIC_PRIMITIVE_FIELD(fn, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField(fid); \
  f->Set ##fn <false>(f->GetDeclaringClass(), value)

  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Boolean, obj);
  }

  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Byte, obj);
  }

  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Char, obj);
  }

  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Short, obj);
  }

  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Int, obj);
  }

  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Long, obj);
  }

  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Float, obj);
  }

  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Double, obj);
  }

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Boolean);
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Byte);
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Char);
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Short);
  }

  static jint GetStaticIntField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Int);
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Long);
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Float);
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Double);
  }

  static void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fid, jboolean v) {
    SET_PRIMITIVE_FIELD(Boolean, obj, v);
  }

  static void SetByteField(JNIEnv* env, jobject obj, jfieldID fid, jbyte v) {
    SET_PRIMITIVE_FIELD(Byte, obj, v);
  }

  static void SetCharField(JNIEnv* env, jobject obj, jfieldID fid, jchar v) {
    SET_PRIMITIVE_FIELD(Char, obj, v);
  }

  static void SetFloatField(JNIEnv* env, jobject obj, jfieldID fid, jfloat v) {
    SET_PRIMITIVE_FIELD(Float, obj, v);
  }

  static void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fid, jdouble v) {
    SET_PRIMITIVE_FIELD(Double, obj, v);
  }

  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fid, jint v) {
    SET_PRIMITIVE_FIELD(Int, obj, v);
  }

  static void SetLongField(JNIEnv* env, jobject obj, jfieldID fid, jlong v) {
    SET_PRIMITIVE_FIELD(Long, obj, v);
  }

  static void SetShortField(JNIEnv* env, jobject obj, jfieldID fid, jshort v) {
    SET_PRIMITIVE_FIELD(Short, obj, v);
  }

  static void SetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid, jboolean v) {
    SET_STATIC_PRIMITIVE_FIELD(Boolean, v);
  }

  static void SetStaticByteField(JNIEnv* env, jclass, jfieldID fid, jbyte v) {
    SET_STATIC_PRIMITIVE_FIELD(Byte, v);
  }

  static void SetStaticCharField(JNIEnv* env, jclass, jfieldID fid, jchar v) {
    SET_STATIC_PRIMITIVE_FIELD(Char, v);
  }

  static void SetStaticFloatField(JNIEnv* env, jclass, jfieldID fid, jfloat v) {
    SET_STATIC_PRIMITIVE_FIELD(Float, v);
  }

  static void SetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid, jdouble v) {
    SET_STATIC_PRIMITIVE_FIELD(Double, v);
  }

  static void SetStaticIntField(JNIEnv* env, jclass, jfieldID fid, jint v) {
    SET_STATIC_PRIMITIVE_FIELD(Int, v);
  }

  static void SetStaticLongField(JNIEnv* env, jclass, jfieldID fid, jlong v) {
    SET_STATIC_PRIMITIVE_FIELD(Long, v);
  }

  static void SetStaticShortField(JNIEnv* env, jclass, jfieldID fid, jshort v) {
    SET_STATIC_PRIMITIVE_FIELD(Short, v);
  }

  static jobject CallStaticObjectMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    jobject local_result = soa.AddLocalReference<jobject>(result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetZ();
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetZ();
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetB();
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetB();
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallStaticCharMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetC();
  }

  static jchar CallStaticCharMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetC();
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallStaticShortMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetS();
  }

  static jshort CallStaticShortMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetS();
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallStaticIntMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetI();
  }

  static jint CallStaticIntMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetI();
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallStaticLongMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetJ();
  }

  static jlong CallStaticLongMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetJ();
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetF();
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetF();
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetD();
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetD();
  }

  static void CallStaticVoidMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, ap);
    va_end(ap);
  }

  static void CallStaticVoidMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, nullptr, mid, args);
  }

  static jstring NewString(JNIEnv* env, const jchar* chars, jsize char_count) {
    if (UNLIKELY(char_count < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewString", "char_count < 0: %d", char_count);
      return nullptr;
    }
    if (UNLIKELY(chars == nullptr && char_count > 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewString", "chars == null && char_count > 0");
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    mirror::String* result = mirror::String::AllocFromUtf16(soa.Self(), char_count, chars);
    return soa.AddLocalReference<jstring>(result);
  }

  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    if (utf == nullptr) {
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    mirror::String* result = mirror::String::AllocFromModifiedUtf8(soa.Self(), utf);
    return soa.AddLocalReference<jstring>(result);
  }

  static jsize GetStringLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    return soa.Decode<mirror::String>(java_string)->GetLength();
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    return soa.Decode<mirror::String>(java_string)->GetUtfLength();
  }

  static void GetStringRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                              jchar* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (start < 0 || length < 0 || length > s->GetLength() - start) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      if (s->IsCompressed()) {
        for (int i = 0; i < length; ++i) {
          buf[i] = static_cast<jchar>(s->CharAt(start+i));
        }
      } else {
        const jchar* chars = static_cast<jchar*>(s->GetValue());
        memcpy(buf, chars + start, length * sizeof(jchar));
      }
    }
  }

  static void GetStringUTFRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                                 char* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (start < 0 || length < 0 || length > s->GetLength() - start) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      if (s->IsCompressed()) {
        for (int i = 0; i < length; ++i) {
          buf[i] = s->CharAt(start+i);
        }
      } else {
        const jchar* chars = s->GetValue();
        size_t bytes = CountUtf8Bytes(chars + start, length);
        ConvertUtf16ToModifiedUtf8(buf, bytes, chars + start, length);
      }
    }
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(s) || s->IsCompressed()) {
      jchar* chars = new jchar[s->GetLength()];
      if (s->IsCompressed()) {
        int32_t length = s->GetLength();
        for (int i = 0; i < length; ++i) {
          chars[i] = s->CharAt(i);
        }
      } else {
        memcpy(chars, s->GetValue(), sizeof(jchar) * s->GetLength());
      }
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      return chars;
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return static_cast<jchar*>(s->GetValue());
  }

  static void ReleaseStringChars(JNIEnv* env, jstring java_string, const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (s->IsCompressed() || (s->IsCompressed() == false && chars != s->GetValue())) {
      delete[] chars;
    }
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(s)) {
      StackHandleScope<1> hs(soa.Self());
      HandleWrapperObjPtr<mirror::String> h(hs.NewHandleWrapper(&s));
      if (!kUseReadBarrier) {
        heap->IncrementDisableMovingGC(soa.Self());
      } else {
        // For the CC collector, we only need to wait for the thread flip rather than the whole GC
        // to occur thanks to the to-space invariant.
        heap->IncrementDisableThreadFlip(soa.Self());
      }
    }
    if (s->IsCompressed()) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      int32_t length = s->GetLength();
      jchar* chars = new jchar[length];
      for (int i = 0; i < length; ++i) {
        chars[i] = s->CharAt(i);
      }
      return chars;
    } else {
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return static_cast<jchar*>(s->GetValue());
    }
  }

  static void ReleaseStringCritical(JNIEnv* env,
                                    jstring java_string,
                                    const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (heap->IsMovableObject(s)) {
      if (!kUseReadBarrier) {
        heap->DecrementDisableMovingGC(soa.Self());
      } else {
        heap->DecrementDisableThreadFlip(soa.Self());
      }
    }
    if (s->IsCompressed() || (s->IsCompressed() == false && s->GetValue() != chars)) {
      delete[] chars;
    }
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    if (java_string == nullptr) {
      return nullptr;
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_TRUE;
    }
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    size_t byte_count = s->GetUtfLength();
    char* bytes = new char[byte_count + 1];
    CHECK(bytes != nullptr);  // bionic aborts anyway.
    if (s->IsCompressed()) {
      for (size_t i = 0; i < byte_count; ++i) {
        bytes[i] = s->CharAt(i);
      }
    } else {
      const uint16_t* chars = s->GetValue();
      ConvertUtf16ToModifiedUtf8(bytes, byte_count, chars, s->GetLength());
    }
    bytes[byte_count] = '\0';
    return bytes;
  }

  static void ReleaseStringUTFChars(JNIEnv*, jstring, const char* chars) {
    delete[] chars;
  }

  static jsize GetArrayLength(JNIEnv* env, jarray java_array) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(java_array);
    if (UNLIKELY(!obj->IsArrayInstance())) {
      soa.Vm()->JniAbortF("GetArrayLength", "not an array: %s", obj->PrettyTypeOf().c_str());
      return 0;
    }
    mirror::Array* array = obj->AsArray();
    return array->GetLength();
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        soa.Decode<mirror::ObjectArray<mirror::Object>>(java_array);
    return soa.AddLocalReference<jobject>(array->Get(index));
  }

  static void SetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index,
                                    jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        soa.Decode<mirror::ObjectArray<mirror::Object>>(java_array);
    ObjPtr<mirror::Object> value = soa.Decode<mirror::Object>(java_value);
    array->Set<false>(index, value.Ptr());
  }

  static jbooleanArray NewBooleanArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbooleanArray, mirror::BooleanArray>(env, length);
  }

  static jbyteArray NewByteArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbyteArray, mirror::ByteArray>(env, length);
  }

  static jcharArray NewCharArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jcharArray, mirror::CharArray>(env, length);
  }

  static jdoubleArray NewDoubleArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jdoubleArray, mirror::DoubleArray>(env, length);
  }

  static jfloatArray NewFloatArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jfloatArray, mirror::FloatArray>(env, length);
  }

  static jintArray NewIntArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jintArray, mirror::IntArray>(env, length);
  }

  static jlongArray NewLongArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jlongArray, mirror::LongArray>(env, length);
  }

  static jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass element_jclass,
                                     jobject initial_element) {
    if (UNLIKELY(length < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewObjectArray", "negative array length: %d", length);
      return nullptr;
    }
    CHECK_NON_NULL_ARGUMENT(element_jclass);

    // Compute the array class corresponding to the given element class.
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> array_class;
    {
      ObjPtr<mirror::Class> element_class = soa.Decode<mirror::Class>(element_jclass).Ptr();
      if (UNLIKELY(element_class->IsPrimitive())) {
        soa.Vm()->JniAbortF("NewObjectArray",
                            "not an object type: %s",
                            element_class->PrettyDescriptor().c_str());
        return nullptr;
      }
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      array_class = class_linker->FindArrayClass(soa.Self(), &element_class);
      if (UNLIKELY(array_class == nullptr)) {
        return nullptr;
      }
    }

    // Allocate and initialize if necessary.
    mirror::ObjectArray<mirror::Object>* result =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), array_class, length);
    if (result != nullptr && initial_element != nullptr) {
      ObjPtr<mirror::Object> initial_object = soa.Decode<mirror::Object>(initial_element);
      if (initial_object != nullptr) {
        mirror::Class* element_class = result->GetClass()->GetComponentType();
        if (UNLIKELY(!element_class->IsAssignableFrom(initial_object->GetClass()))) {
          soa.Vm()->JniAbortF("NewObjectArray", "cannot assign object of type '%s' to array with "
                              "element type of '%s'",
                              mirror::Class::PrettyDescriptor(initial_object->GetClass()).c_str(),
                              element_class->PrettyDescriptor().c_str());
          return nullptr;
        } else {
          for (jsize i = 0; i < length; ++i) {
            result->SetWithoutChecks<false>(i, initial_object.Ptr());
          }
        }
      }
    }
    return soa.AddLocalReference<jobjectArray>(result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jshortArray, mirror::ShortArray>(env, length);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Array> array = soa.Decode<mirror::Array>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      soa.Vm()->JniAbortF("GetPrimitiveArrayCritical", "expected primitive array, given %s",
                          array->GetClass()->PrettyDescriptor().c_str());
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(array)) {
      if (!kUseReadBarrier) {
        heap->IncrementDisableMovingGC(soa.Self());
      } else {
        // For the CC collector, we only need to wait for the thread flip rather than the whole GC
        // to occur thanks to the to-space invariant.
        heap->IncrementDisableThreadFlip(soa.Self());
      }
      // Re-decode in case the object moved since IncrementDisableGC waits for GC to complete.
      array = soa.Decode<mirror::Array>(java_array);
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return array->GetRawData(array->GetClass()->GetComponentSize(), 0);
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray java_array, void* elements,
                                            jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Array> array = soa.Decode<mirror::Array>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      soa.Vm()->JniAbortF("ReleasePrimitiveArrayCritical", "expected primitive array, given %s",
                          array->GetClass()->PrettyDescriptor().c_str());
      return;
    }
    const size_t component_size = array->GetClass()->GetComponentSize();
    ReleasePrimitiveArray(soa, array.Ptr(), component_size, elements, mode);
  }

  static jboolean* GetBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, is_copy);
  }

  static jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, is_copy);
  }

  static jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, is_copy);
  }

  static jdouble* GetDoubleArrayElements(JNIEnv* env, jdoubleArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, is_copy);
  }

  static jfloat* GetFloatArrayElements(JNIEnv* env, jfloatArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, is_copy);
  }

  static jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, is_copy);
  }

  static jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, is_copy);
  }

  static jshort* GetShortArrayElements(JNIEnv* env, jshortArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, is_copy);
  }

  static void ReleaseBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* elements,
                                          jint mode) {
    ReleasePrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, elements,
                                                                         mode);
  }

  static void ReleaseByteArrayElements(JNIEnv* env, jbyteArray array, jbyte* elements, jint mode) {
    ReleasePrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, elements, mode);
  }

  static void ReleaseCharArrayElements(JNIEnv* env, jcharArray array, jchar* elements, jint mode) {
    ReleasePrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, elements, mode);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env, jdoubleArray array, jdouble* elements,
                                         jint mode) {
    ReleasePrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, elements, mode);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env, jfloatArray array, jfloat* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, elements, mode);
  }

  static void ReleaseIntArrayElements(JNIEnv* env, jintArray array, jint* elements, jint mode) {
    ReleasePrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, elements, mode);
  }

  static void ReleaseLongArrayElements(JNIEnv* env, jlongArray array, jlong* elements, jint mode) {
    ReleasePrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, elements, mode);
  }

  static void ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, elements, mode);
  }

  static void GetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    jboolean* buf) {
    GetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void GetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 jbyte* buf) {
    GetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void GetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 jchar* buf) {
    GetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void GetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   jdouble* buf) {
    GetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void GetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  jfloat* buf) {
    GetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void GetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                jint* buf) {
    GetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void GetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 jlong* buf) {
    GetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void GetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  jshort* buf) {
    GetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    const jboolean* buf) {
    SetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void SetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 const jbyte* buf) {
    SetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void SetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 const jchar* buf) {
    SetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void SetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   const jdouble* buf) {
    SetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void SetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  const jfloat* buf) {
    SetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                const jint* buf) {
    SetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void SetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 const jlong* buf) {
    SetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void SetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  const jshort* buf) {
    SetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static jint RegisterNatives(JNIEnv* env, jclass java_class, const JNINativeMethod* methods,
                              jint method_count) {
    return RegisterNativeMethods(env, java_class, methods, method_count, true);
  }

  static jint RegisterNativeMethods(JNIEnv* env, jclass java_class, const JNINativeMethod* methods,
                                    jint method_count, bool return_errors) {
    if (UNLIKELY(method_count < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("RegisterNatives", "negative method count: %d",
                                       method_count);
      return JNI_ERR;  // Not reached except in unit tests.
    }
    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", java_class, JNI_ERR);
    ScopedObjectAccess soa(env);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> c = hs.NewHandle(soa.Decode<mirror::Class>(java_class));
    if (UNLIKELY(method_count == 0)) {
      LOG(WARNING) << "JNI RegisterNativeMethods: attempt to register 0 native methods for "
          << c->PrettyDescriptor();
      return JNI_OK;
    }
    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", methods, JNI_ERR);
    for (jint i = 0; i < method_count; ++i) {
      const char* name = methods[i].name;
      const char* sig = methods[i].signature;
      const void* fnPtr = methods[i].fnPtr;
      if (UNLIKELY(name == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "method name", i, return_errors);
        return JNI_ERR;
      } else if (UNLIKELY(sig == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "method signature", i, return_errors);
        return JNI_ERR;
      } else if (UNLIKELY(fnPtr == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "native function", i, return_errors);
        return JNI_ERR;
      }
      bool is_fast = false;
      // Notes about fast JNI calls:
      //
      // On a normal JNI call, the calling thread usually transitions
      // from the kRunnable state to the kNative state. But if the
      // called native function needs to access any Java object, it
      // will have to transition back to the kRunnable state.
      //
      // There is a cost to this double transition. For a JNI call
      // that should be quick, this cost may dominate the call cost.
      //
      // On a fast JNI call, the calling thread avoids this double
      // transition by not transitioning from kRunnable to kNative and
      // stays in the kRunnable state.
      //
      // There are risks to using a fast JNI call because it can delay
      // a response to a thread suspension request which is typically
      // used for a GC root scanning, etc. If a fast JNI call takes a
      // long time, it could cause longer thread suspension latency
      // and GC pauses.
      //
      // Thus, fast JNI should be used with care. It should be used
      // for a JNI call that takes a short amount of time (eg. no
      // long-running loop) and does not block (eg. no locks, I/O,
      // etc.)
      //
      // A '!' prefix in the signature in the JNINativeMethod
      // indicates that it's a fast JNI call and the runtime omits the
      // thread state transition from kRunnable to kNative at the
      // entry.
      if (*sig == '!') {
        is_fast = true;
        ++sig;
      }

      // Note: the right order is to try to find the method locally
      // first, either as a direct or a virtual method. Then move to
      // the parent.
      ArtMethod* m = nullptr;
      bool warn_on_going_to_parent = down_cast<JNIEnvExt*>(env)->vm->IsCheckJniEnabled();
      for (ObjPtr<mirror::Class> current_class = c.Get();
           current_class != nullptr;
           current_class = current_class->GetSuperClass()) {
        // Search first only comparing methods which are native.
        m = FindMethod<true>(current_class.Ptr(), name, sig);
        if (m != nullptr) {
          break;
        }

        // Search again comparing to all methods, to find non-native methods that match.
        m = FindMethod<false>(current_class.Ptr(), name, sig);
        if (m != nullptr) {
          break;
        }

        if (warn_on_going_to_parent) {
          LOG(WARNING) << "CheckJNI: method to register \"" << name << "\" not in the given class. "
                       << "This is slow, consider changing your RegisterNatives calls.";
          warn_on_going_to_parent = false;
        }
      }

      if (m == nullptr) {
        c->DumpClass(
            LOG_STREAM(return_errors
                           ? ::android::base::ERROR
                           : ::android::base::FATAL_WITHOUT_ABORT),
            mirror::Class::kDumpClassFullDetail);
        LOG(return_errors ? ::android::base::ERROR : ::android::base::FATAL)
            << "Failed to register native method "
            << c->PrettyDescriptor() << "." << name << sig << " in "
            << c->GetDexCache()->GetLocation()->ToModifiedUtf8();
        ThrowNoSuchMethodError(soa, c.Get(), name, sig, "static or non-static");
        return JNI_ERR;
      } else if (!m->IsNative()) {
        LOG(return_errors ? ::android::base::ERROR : ::android::base::FATAL)
            << "Failed to register non-native method "
            << c->PrettyDescriptor() << "." << name << sig
            << " as native";
        ThrowNoSuchMethodError(soa, c.Get(), name, sig, "native");
        return JNI_ERR;
      }

      VLOG(jni) << "[Registering JNI native method " << m->PrettyMethod() << "]";

      if (UNLIKELY(is_fast)) {
        // There are a few reasons to switch:
        // 1) We don't support !bang JNI anymore, it will turn to a hard error later.
        // 2) @FastNative is actually faster. At least 1.5x faster than !bang JNI.
        //    and switching is super easy, remove ! in C code, add annotation in .java code.
        // 3) Good chance of hitting DCHECK failures in ScopedFastNativeObjectAccess
        //    since that checks for presence of @FastNative and not for ! in the descriptor.
        LOG(WARNING) << "!bang JNI is deprecated. Switch to @FastNative for " << m->PrettyMethod();
        is_fast = false;
        // TODO: make this a hard register error in the future.
      }

      const void* final_function_ptr = m->RegisterNative(fnPtr, is_fast);
      UNUSED(final_function_ptr);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);

    VLOG(jni) << "[Unregistering JNI native methods for " << mirror::Class::PrettyClass(c) << "]";

    size_t unregistered_count = 0;
    auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
    for (auto& m : c->GetMethods(pointer_size)) {
      if (m.IsNative()) {
        m.UnregisterNative();
        unregistered_count++;
      }
    }

    if (unregistered_count == 0) {
      LOG(WARNING) << "JNI UnregisterNatives: attempt to unregister native methods of class '"
          << mirror::Class::PrettyDescriptor(c) << "' that contains no native methods";
    }
    return JNI_OK;
  }

  static jint MonitorEnter(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    o = o->MonitorEnter(soa.Self());
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    soa.Env()->monitors.Add(o);
    return JNI_OK;
  }

  static jint MonitorExit(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    o->MonitorExit(soa.Self());
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    soa.Env()->monitors.Remove(o);
    return JNI_OK;
  }

  static jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
    CHECK_NON_NULL_ARGUMENT_RETURN(vm, JNI_ERR);
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      *vm = runtime->GetJavaVM();
    } else {
      *vm = nullptr;
    }
    return (*vm != nullptr) ? JNI_OK : JNI_ERR;
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    if (capacity < 0) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer", "negative buffer capacity: %" PRId64,
                                       capacity);
      return nullptr;
    }
    if (address == nullptr && capacity != 0) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer",
                                       "non-zero capacity for nullptr pointer: %" PRId64, capacity);
      return nullptr;
    }

    // At the moment, the capacity of DirectByteBuffer is limited to a signed int.
    if (capacity > INT_MAX) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer",
                                       "buffer capacity greater than maximum jint: %" PRId64,
                                       capacity);
      return nullptr;
    }
    jlong address_arg = reinterpret_cast<jlong>(address);
    jint capacity_arg = static_cast<jint>(capacity);

    jobject result = env->NewObject(WellKnownClasses::java_nio_DirectByteBuffer,
                                    WellKnownClasses::java_nio_DirectByteBuffer_init,
                                    address_arg, capacity_arg);
    return static_cast<JNIEnvExt*>(env)->self->IsExceptionPending() ? nullptr : result;
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject java_buffer) {
    return reinterpret_cast<void*>(env->GetLongField(
        java_buffer, WellKnownClasses::java_nio_DirectByteBuffer_effectiveDirectAddress));
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject java_buffer) {
    return static_cast<jlong>(env->GetIntField(
        java_buffer, WellKnownClasses::java_nio_DirectByteBuffer_capacity));
  }

  static jobjectRefType GetObjectRefType(JNIEnv* env ATTRIBUTE_UNUSED, jobject java_object) {
    if (java_object == nullptr) {
      return JNIInvalidRefType;
    }

    // Do we definitely know what kind of reference this is?
    IndirectRef ref = reinterpret_cast<IndirectRef>(java_object);
    IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
    switch (kind) {
    case kLocal:
      return JNILocalRefType;
    case kGlobal:
      return JNIGlobalRefType;
    case kWeakGlobal:
      return JNIWeakGlobalRefType;
    case kHandleScopeOrInvalid:
      // Assume value is in a handle scope.
      return JNILocalRefType;
    }
    LOG(FATAL) << "IndirectRefKind[" << kind << "]";
    UNREACHABLE();
  }

 private:
  static jint EnsureLocalCapacityInternal(ScopedObjectAccess& soa, jint desired_capacity,
                                          const char* caller)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // TODO: we should try to expand the table if necessary.
    if (desired_capacity < 0 || desired_capacity > static_cast<jint>(kLocalsInitial)) {
      LOG(ERROR) << "Invalid capacity given to " << caller << ": " << desired_capacity;
      return JNI_ERR;
    }
    // TODO: this isn't quite right, since "capacity" includes holes.
    const size_t capacity = soa.Env()->locals.Capacity();
    bool okay = (static_cast<jint>(kLocalsInitial - capacity) >= desired_capacity);
    if (!okay) {
      soa.Self()->ThrowOutOfMemoryError(caller);
    }
    return okay ? JNI_OK : JNI_ERR;
  }

  template<typename JniT, typename ArtT>
  static JniT NewPrimitiveArray(JNIEnv* env, jsize length) {
    ScopedObjectAccess soa(env);
    if (UNLIKELY(length < 0)) {
      soa.Vm()->JniAbortF("NewPrimitiveArray", "negative array length: %d", length);
      return nullptr;
    }
    ArtT* result = ArtT::Alloc(soa.Self(), length);
    return soa.AddLocalReference<JniT>(result);
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static ArtArrayT* DecodeAndCheckArrayType(ScopedObjectAccess& soa, JArrayT java_array,
                                           const char* fn_name, const char* operation)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<ArtArrayT> array = soa.Decode<ArtArrayT>(java_array);
    if (UNLIKELY(ArtArrayT::GetArrayClass() != array->GetClass())) {
      soa.Vm()->JniAbortF(fn_name,
                          "attempt to %s %s primitive array elements with an object of type %s",
                          operation,
                          mirror::Class::PrettyDescriptor(
                              ArtArrayT::GetArrayClass()->GetComponentType()).c_str(),
                          mirror::Class::PrettyDescriptor(array->GetClass()).c_str());
      return nullptr;
    }
    DCHECK_EQ(sizeof(ElementT), array->GetClass()->GetComponentSize());
    return array.Ptr();
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static ElementT* GetPrimitiveArray(JNIEnv* env, ArrayT java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                                            "GetArrayElements",
                                                                            "get");
    if (UNLIKELY(array == nullptr)) {
      return nullptr;
    }
    // Only make a copy if necessary.
    if (Runtime::Current()->GetHeap()->IsMovableObject(array)) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      const size_t component_size = sizeof(ElementT);
      size_t size = array->GetLength() * component_size;
      void* data = new uint64_t[RoundUp(size, 8) / 8];
      memcpy(data, array->GetData(), size);
      return reinterpret_cast<ElementT*>(data);
    } else {
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return reinterpret_cast<ElementT*>(array->GetData());
    }
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static void ReleasePrimitiveArray(JNIEnv* env, ArrayT java_array, ElementT* elements, jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                                            "ReleaseArrayElements",
                                                                            "release");
    if (array == nullptr) {
      return;
    }
    ReleasePrimitiveArray(soa, array, sizeof(ElementT), elements, mode);
  }

  static void ReleasePrimitiveArray(ScopedObjectAccess& soa, mirror::Array* array,
                                    size_t component_size, void* elements, jint mode)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    void* array_data = array->GetRawData(component_size, 0);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    bool is_copy = array_data != elements;
    size_t bytes = array->GetLength() * component_size;
    if (is_copy) {
      // Sanity check: If elements is not the same as the java array's data, it better not be a
      // heap address. TODO: This might be slow to check, may be worth keeping track of which
      // copies we make?
      if (heap->IsNonDiscontinuousSpaceHeapAddress(elements)) {
        soa.Vm()->JniAbortF("ReleaseArrayElements",
                            "invalid element pointer %p, array elements are %p",
                            reinterpret_cast<void*>(elements), array_data);
        return;
      }
      if (mode != JNI_ABORT) {
        memcpy(array_data, elements, bytes);
      } else if (kWarnJniAbort && memcmp(array_data, elements, bytes) != 0) {
        // Warn if we have JNI_ABORT and the arrays don't match since this is usually an error.
        LOG(WARNING) << "Possible incorrect JNI_ABORT in Release*ArrayElements";
        soa.Self()->DumpJavaStack(LOG_STREAM(WARNING));
      }
    }
    if (mode != JNI_COMMIT) {
      if (is_copy) {
        delete[] reinterpret_cast<uint64_t*>(elements);
      } else if (heap->IsMovableObject(array)) {
        // Non copy to a movable object must means that we had disabled the moving GC.
        if (!kUseReadBarrier) {
          heap->DecrementDisableMovingGC(soa.Self());
        } else {
          heap->DecrementDisableThreadFlip(soa.Self());
        }
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void GetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array =
        DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                              "GetPrimitiveArrayRegion",
                                                              "get region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || length > array->GetLength() - start) {
        ThrowAIOOBE(soa, array, start, length, "src");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(buf, data + start, length * sizeof(ElementT));
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void SetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, const ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array =
        DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                              "SetPrimitiveArrayRegion",
                                                              "set region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || length > array->GetLength() - start) {
        ThrowAIOOBE(soa, array, start, length, "dst");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(data + start, buf, length * sizeof(ElementT));
      }
    }
  }
};

const JNINativeInterface gJniNativeInterface = {
  nullptr,  // reserved0.
  nullptr,  // reserved1.
  nullptr,  // reserved2.
  nullptr,  // reserved3.
  JNI::GetVersion,
  JNI::DefineClass,
  JNI::FindClass,
  JNI::FromReflectedMethod,
  JNI::FromReflectedField,
  JNI::ToReflectedMethod,
  JNI::GetSuperclass,
  JNI::IsAssignableFrom,
  JNI::ToReflectedField,
  JNI::Throw,
  JNI::ThrowNew,
  JNI::ExceptionOccurred,
  JNI::ExceptionDescribe,
  JNI::ExceptionClear,
  JNI::FatalError,
  JNI::PushLocalFrame,
  JNI::PopLocalFrame,
  JNI::NewGlobalRef,
  JNI::DeleteGlobalRef,
  JNI::DeleteLocalRef,
  JNI::IsSameObject,
  JNI::NewLocalRef,
  JNI::EnsureLocalCapacity,
  JNI::AllocObject,
  JNI::NewObject,
  JNI::NewObjectV,
  JNI::NewObjectA,
  JNI::GetObjectClass,
  JNI::IsInstanceOf,
  JNI::GetMethodID,
  JNI::CallObjectMethod,
  JNI::CallObjectMethodV,
  JNI::CallObjectMethodA,
  JNI::CallBooleanMethod,
  JNI::CallBooleanMethodV,
  JNI::CallBooleanMethodA,
  JNI::CallByteMethod,
  JNI::CallByteMethodV,
  JNI::CallByteMethodA,
  JNI::CallCharMethod,
  JNI::CallCharMethodV,
  JNI::CallCharMethodA,
  JNI::CallShortMethod,
  JNI::CallShortMethodV,
  JNI::CallShortMethodA,
  JNI::CallIntMethod,
  JNI::CallIntMethodV,
  JNI::CallIntMethodA,
  JNI::CallLongMethod,
  JNI::CallLongMethodV,
  JNI::CallLongMethodA,
  JNI::CallFloatMethod,
  JNI::CallFloatMethodV,
  JNI::CallFloatMethodA,
  JNI::CallDoubleMethod,
  JNI::CallDoubleMethodV,
  JNI::CallDoubleMethodA,
  JNI::CallVoidMethod,
  JNI::CallVoidMethodV,
  JNI::CallVoidMethodA,
  JNI::CallNonvirtualObjectMethod,
  JNI::CallNonvirtualObjectMethodV,
  JNI::CallNonvirtualObjectMethodA,
  JNI::CallNonvirtualBooleanMethod,
  JNI::CallNonvirtualBooleanMethodV,
  JNI::CallNonvirtualBooleanMethodA,
  JNI::CallNonvirtualByteMethod,
  JNI::CallNonvirtualByteMethodV,
  JNI::CallNonvirtualByteMethodA,
  JNI::CallNonvirtualCharMethod,
  JNI::CallNonvirtualCharMethodV,
  JNI::CallNonvirtualCharMethodA,
  JNI::CallNonvirtualShortMethod,
  JNI::CallNonvirtualShortMethodV,
  JNI::CallNonvirtualShortMethodA,
  JNI::CallNonvirtualIntMethod,
  JNI::CallNonvirtualIntMethodV,
  JNI::CallNonvirtualIntMethodA,
  JNI::CallNonvirtualLongMethod,
  JNI::CallNonvirtualLongMethodV,
  JNI::CallNonvirtualLongMethodA,
  JNI::CallNonvirtualFloatMethod,
  JNI::CallNonvirtualFloatMethodV,
  JNI::CallNonvirtualFloatMethodA,
  JNI::CallNonvirtualDoubleMethod,
  JNI::CallNonvirtualDoubleMethodV,
  JNI::CallNonvirtualDoubleMethodA,
  JNI::CallNonvirtualVoidMethod,
  JNI::CallNonvirtualVoidMethodV,
  JNI::CallNonvirtualVoidMethodA,
  JNI::GetFieldID,
  JNI::GetObjectField,
  JNI::GetBooleanField,
  JNI::GetByteField,
  JNI::GetCharField,
  JNI::GetShortField,
  JNI::GetIntField,
  JNI::GetLongField,
  JNI::GetFloatField,
  JNI::GetDoubleField,
  JNI::SetObjectField,
  JNI::SetBooleanField,
  JNI::SetByteField,
  JNI::SetCharField,
  JNI::SetShortField,
  JNI::SetIntField,
  JNI::SetLongField,
  JNI::SetFloatField,
  JNI::SetDoubleField,
  JNI::GetStaticMethodID,
  JNI::CallStaticObjectMethod,
  JNI::CallStaticObjectMethodV,
  JNI::CallStaticObjectMethodA,
  JNI::CallStaticBooleanMethod,
  JNI::CallStaticBooleanMethodV,
  JNI::CallStaticBooleanMethodA,
  JNI::CallStaticByteMethod,
  JNI::CallStaticByteMethodV,
  JNI::CallStaticByteMethodA,
  JNI::CallStaticCharMethod,
  JNI::CallStaticCharMethodV,
  JNI::CallStaticCharMethodA,
  JNI::CallStaticShortMethod,
  JNI::CallStaticShortMethodV,
  JNI::CallStaticShortMethodA,
  JNI::CallStaticIntMethod,
  JNI::CallStaticIntMethodV,
  JNI::CallStaticIntMethodA,
  JNI::CallStaticLongMethod,
  JNI::CallStaticLongMethodV,
  JNI::CallStaticLongMethodA,
  JNI::CallStaticFloatMethod,
  JNI::CallStaticFloatMethodV,
  JNI::CallStaticFloatMethodA,
  JNI::CallStaticDoubleMethod,
  JNI::CallStaticDoubleMethodV,
  JNI::CallStaticDoubleMethodA,
  JNI::CallStaticVoidMethod,
  JNI::CallStaticVoidMethodV,
  JNI::CallStaticVoidMethodA,
  JNI::GetStaticFieldID,
  JNI::GetStaticObjectField,
  JNI::GetStaticBooleanField,
  JNI::GetStaticByteField,
  JNI::GetStaticCharField,
  JNI::GetStaticShortField,
  JNI::GetStaticIntField,
  JNI::GetStaticLongField,
  JNI::GetStaticFloatField,
  JNI::GetStaticDoubleField,
  JNI::SetStaticObjectField,
  JNI::SetStaticBooleanField,
  JNI::SetStaticByteField,
  JNI::SetStaticCharField,
  JNI::SetStaticShortField,
  JNI::SetStaticIntField,
  JNI::SetStaticLongField,
  JNI::SetStaticFloatField,
  JNI::SetStaticDoubleField,
  JNI::NewString,
  JNI::GetStringLength,
  JNI::GetStringChars,
  JNI::ReleaseStringChars,
  JNI::NewStringUTF,
  JNI::GetStringUTFLength,
  JNI::GetStringUTFChars,
  JNI::ReleaseStringUTFChars,
  JNI::GetArrayLength,
  JNI::NewObjectArray,
  JNI::GetObjectArrayElement,
  JNI::SetObjectArrayElement,
  JNI::NewBooleanArray,
  JNI::NewByteArray,
  JNI::NewCharArray,
  JNI::NewShortArray,
  JNI::NewIntArray,
  JNI::NewLongArray,
  JNI::NewFloatArray,
  JNI::NewDoubleArray,
  JNI::GetBooleanArrayElements,
  JNI::GetByteArrayElements,
  JNI::GetCharArrayElements,
  JNI::GetShortArrayElements,
  JNI::GetIntArrayElements,
  JNI::GetLongArrayElements,
  JNI::GetFloatArrayElements,
  JNI::GetDoubleArrayElements,
  JNI::ReleaseBooleanArrayElements,
  JNI::ReleaseByteArrayElements,
  JNI::ReleaseCharArrayElements,
  JNI::ReleaseShortArrayElements,
  JNI::ReleaseIntArrayElements,
  JNI::ReleaseLongArrayElements,
  JNI::ReleaseFloatArrayElements,
  JNI::ReleaseDoubleArrayElements,
  JNI::GetBooleanArrayRegion,
  JNI::GetByteArrayRegion,
  JNI::GetCharArrayRegion,
  JNI::GetShortArrayRegion,
  JNI::GetIntArrayRegion,
  JNI::GetLongArrayRegion,
  JNI::GetFloatArrayRegion,
  JNI::GetDoubleArrayRegion,
  JNI::SetBooleanArrayRegion,
  JNI::SetByteArrayRegion,
  JNI::SetCharArrayRegion,
  JNI::SetShortArrayRegion,
  JNI::SetIntArrayRegion,
  JNI::SetLongArrayRegion,
  JNI::SetFloatArrayRegion,
  JNI::SetDoubleArrayRegion,
  JNI::RegisterNatives,
  JNI::UnregisterNatives,
  JNI::MonitorEnter,
  JNI::MonitorExit,
  JNI::GetJavaVM,
  JNI::GetStringRegion,
  JNI::GetStringUTFRegion,
  JNI::GetPrimitiveArrayCritical,
  JNI::ReleasePrimitiveArrayCritical,
  JNI::GetStringCritical,
  JNI::ReleaseStringCritical,
  JNI::NewWeakGlobalRef,
  JNI::DeleteWeakGlobalRef,
  JNI::ExceptionCheck,
  JNI::NewDirectByteBuffer,
  JNI::GetDirectBufferAddress,
  JNI::GetDirectBufferCapacity,
  JNI::GetObjectRefType,
};

const JNINativeInterface* GetJniNativeInterface() {
  return &gJniNativeInterface;
}

void (*gJniSleepForeverStub[])()  = {
  nullptr,  // reserved0.
  nullptr,  // reserved1.
  nullptr,  // reserved2.
  nullptr,  // reserved3.
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
  SleepForever,
};

const JNINativeInterface* GetRuntimeShutdownNativeInterface() {
  return reinterpret_cast<JNINativeInterface*>(&gJniSleepForeverStub);
}

void RegisterNativeMethods(JNIEnv* env, const char* jni_class_name, const JNINativeMethod* methods,
                           jint method_count) {
  ScopedLocalRef<jclass> c(env, env->FindClass(jni_class_name));
  if (c.get() == nullptr) {
    LOG(FATAL) << "Couldn't find class: " << jni_class_name;
  }
  JNI::RegisterNativeMethods(env, c.get(), methods, method_count, false);
}

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs) {
  switch (rhs) {
  case JNIInvalidRefType:
    os << "JNIInvalidRefType";
    return os;
  case JNILocalRefType:
    os << "JNILocalRefType";
    return os;
  case JNIGlobalRefType:
    os << "JNIGlobalRefType";
    return os;
  case JNIWeakGlobalRefType:
    os << "JNIWeakGlobalRefType";
    return os;
  default:
    LOG(FATAL) << "jobjectRefType[" << static_cast<int>(rhs) << "]";
    UNREACHABLE();
  }
}
