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

#include "android-base/stringprintf.h"

#include "art_method-inl.h"
#include "base/dumpable.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "check_jni.h"
#include "dex_file-inl.h"
#include "fault_handler.h"
#include "indirect_reference_table-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "nativebridge/native_bridge.h"
#include "nativeloader/native_loader.h"
#include "java_vm_ext.h"
#include "parsed_options.h"
#include "runtime-inl.h"
#include "runtime_options.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change-inl.h"
#include "sigchain.h"
#include "ti/agent.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {

using android::base::StringAppendF;
using android::base::StringAppendV;

static constexpr size_t kGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

static constexpr size_t kWeakGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

bool JavaVMExt::IsBadJniVersion(int version) {
  // We don't support JNI_VERSION_1_1. These are the only other valid versions.
  return version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6;
}

class SharedLibrary {
 public:
  SharedLibrary(JNIEnv* env, Thread* self, const std::string& path, void* handle,
                bool needs_native_bridge, jobject class_loader, void* class_loader_allocator)
      : path_(path),
        handle_(handle),
        needs_native_bridge_(needs_native_bridge),
        class_loader_(env->NewWeakGlobalRef(class_loader)),
        class_loader_allocator_(class_loader_allocator),
        jni_on_load_lock_("JNI_OnLoad lock"),
        jni_on_load_cond_("JNI_OnLoad condition variable", jni_on_load_lock_),
        jni_on_load_thread_id_(self->GetThreadId()),
        jni_on_load_result_(kPending) {
    CHECK(class_loader_allocator_ != nullptr);
  }

  ~SharedLibrary() {
    Thread* self = Thread::Current();
    if (self != nullptr) {
      self->GetJniEnv()->DeleteWeakGlobalRef(class_loader_);
    }

    android::CloseNativeLibrary(handle_, needs_native_bridge_);
  }

  jweak GetClassLoader() const {
    return class_loader_;
  }

  const void* GetClassLoaderAllocator() const {
    return class_loader_allocator_;
  }

  const std::string& GetPath() const {
    return path_;
  }

  /*
   * Check the result of an earlier call to JNI_OnLoad on this library.
   * If the call has not yet finished in another thread, wait for it.
   */
  bool CheckOnLoadResult()
      REQUIRES(!jni_on_load_lock_) {
    Thread* self = Thread::Current();
    bool okay;
    {
      MutexLock mu(self, jni_on_load_lock_);

      if (jni_on_load_thread_id_ == self->GetThreadId()) {
        // Check this so we don't end up waiting for ourselves.  We need to return "true" so the
        // caller can continue.
        LOG(INFO) << *self << " recursive attempt to load library " << "\"" << path_ << "\"";
        okay = true;
      } else {
        while (jni_on_load_result_ == kPending) {
          VLOG(jni) << "[" << *self << " waiting for \"" << path_ << "\" " << "JNI_OnLoad...]";
          jni_on_load_cond_.Wait(self);
        }

        okay = (jni_on_load_result_ == kOkay);
        VLOG(jni) << "[Earlier JNI_OnLoad for \"" << path_ << "\" "
            << (okay ? "succeeded" : "failed") << "]";
      }
    }
    return okay;
  }

  void SetResult(bool result) REQUIRES(!jni_on_load_lock_) {
    Thread* self = Thread::Current();
    MutexLock mu(self, jni_on_load_lock_);

    jni_on_load_result_ = result ? kOkay : kFailed;
    jni_on_load_thread_id_ = 0;

    // Broadcast a wakeup to anybody sleeping on the condition variable.
    jni_on_load_cond_.Broadcast(self);
  }

  void SetNeedsNativeBridge(bool needs) {
    needs_native_bridge_ = needs;
  }

  bool NeedsNativeBridge() const {
    return needs_native_bridge_;
  }

  void* FindSymbol(const std::string& symbol_name, const char* shorty = nullptr) {
    return NeedsNativeBridge()
        ? FindSymbolWithNativeBridge(symbol_name.c_str(), shorty)
        : FindSymbolWithoutNativeBridge(symbol_name.c_str());
  }

  void* FindSymbolWithoutNativeBridge(const std::string& symbol_name) {
    CHECK(!NeedsNativeBridge());

    return dlsym(handle_, symbol_name.c_str());
  }

  void* FindSymbolWithNativeBridge(const std::string& symbol_name, const char* shorty) {
    CHECK(NeedsNativeBridge());

    uint32_t len = 0;
    return android::NativeBridgeGetTrampoline(handle_, symbol_name.c_str(), shorty, len);
  }

 private:
  enum JNI_OnLoadState {
    kPending,
    kFailed,
    kOkay,
  };

  // Path to library "/system/lib/libjni.so".
  const std::string path_;

  // The void* returned by dlopen(3).
  void* const handle_;

  // True if a native bridge is required.
  bool needs_native_bridge_;

  // The ClassLoader this library is associated with, a weak global JNI reference that is
  // created/deleted with the scope of the library.
  const jweak class_loader_;
  // Used to do equality check on class loaders so we can avoid decoding the weak root and read
  // barriers that mess with class unloading.
  const void* class_loader_allocator_;

  // Guards remaining items.
  Mutex jni_on_load_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  // Wait for JNI_OnLoad in other thread.
  ConditionVariable jni_on_load_cond_ GUARDED_BY(jni_on_load_lock_);
  // Recursive invocation guard.
  uint32_t jni_on_load_thread_id_ GUARDED_BY(jni_on_load_lock_);
  // Result of earlier JNI_OnLoad call.
  JNI_OnLoadState jni_on_load_result_ GUARDED_BY(jni_on_load_lock_);
};

// This exists mainly to keep implementation details out of the header file.
class Libraries {
 public:
  Libraries() {
  }

  ~Libraries() {
    STLDeleteValues(&libraries_);
  }

  // NO_THREAD_SAFETY_ANALYSIS since this may be called from Dumpable. Dumpable can't be annotated
  // properly due to the template. The caller should be holding the jni_libraries_lock_.
  void Dump(std::ostream& os) const NO_THREAD_SAFETY_ANALYSIS {
    Locks::jni_libraries_lock_->AssertHeld(Thread::Current());
    bool first = true;
    for (const auto& library : libraries_) {
      if (!first) {
        os << ' ';
      }
      first = false;
      os << library.first;
    }
  }

  size_t size() const REQUIRES(Locks::jni_libraries_lock_) {
    return libraries_.size();
  }

  SharedLibrary* Get(const std::string& path) REQUIRES(Locks::jni_libraries_lock_) {
    auto it = libraries_.find(path);
    return (it == libraries_.end()) ? nullptr : it->second;
  }

  void Put(const std::string& path, SharedLibrary* library)
      REQUIRES(Locks::jni_libraries_lock_) {
    libraries_.Put(path, library);
  }

  // See section 11.3 "Linking Native Methods" of the JNI spec.
  void* FindNativeMethod(ArtMethod* m, std::string& detail)
      REQUIRES(Locks::jni_libraries_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    std::string jni_short_name(m->JniShortName());
    std::string jni_long_name(m->JniLongName());
    mirror::ClassLoader* const declaring_class_loader = m->GetDeclaringClass()->GetClassLoader();
    ScopedObjectAccessUnchecked soa(Thread::Current());
    void* const declaring_class_loader_allocator =
        Runtime::Current()->GetClassLinker()->GetAllocatorForClassLoader(declaring_class_loader);
    CHECK(declaring_class_loader_allocator != nullptr);
    for (const auto& lib : libraries_) {
      SharedLibrary* const library = lib.second;
      // Use the allocator address for class loader equality to avoid unnecessary weak root decode.
      if (library->GetClassLoaderAllocator() != declaring_class_loader_allocator) {
        // We only search libraries loaded by the appropriate ClassLoader.
        continue;
      }
      // Try the short name then the long name...
      const char* shorty = library->NeedsNativeBridge()
          ? m->GetShorty()
          : nullptr;
      void* fn = library->FindSymbol(jni_short_name, shorty);
      if (fn == nullptr) {
        fn = library->FindSymbol(jni_long_name, shorty);
      }
      if (fn != nullptr) {
        VLOG(jni) << "[Found native code for " << m->PrettyMethod()
                  << " in \"" << library->GetPath() << "\"]";
        return fn;
      }
    }
    detail += "No implementation found for ";
    detail += m->PrettyMethod();
    detail += " (tried " + jni_short_name + " and " + jni_long_name + ")";
    return nullptr;
  }

  // Unload native libraries with cleared class loaders.
  void UnloadNativeLibraries()
      REQUIRES(!Locks::jni_libraries_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    std::vector<SharedLibrary*> unload_libraries;
    {
      MutexLock mu(soa.Self(), *Locks::jni_libraries_lock_);
      for (auto it = libraries_.begin(); it != libraries_.end(); ) {
        SharedLibrary* const library = it->second;
        // If class loader is null then it was unloaded, call JNI_OnUnload.
        const jweak class_loader = library->GetClassLoader();
        // If class_loader is a null jobject then it is the boot class loader. We should not unload
        // the native libraries of the boot class loader.
        if (class_loader != nullptr &&
            soa.Self()->IsJWeakCleared(class_loader)) {
          unload_libraries.push_back(library);
          it = libraries_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // Do this without holding the jni libraries lock to prevent possible deadlocks.
    typedef void (*JNI_OnUnloadFn)(JavaVM*, void*);
    for (auto library : unload_libraries) {
      void* const sym = library->FindSymbol("JNI_OnUnload", nullptr);
      if (sym == nullptr) {
        VLOG(jni) << "[No JNI_OnUnload found in \"" << library->GetPath() << "\"]";
      } else {
        VLOG(jni) << "[JNI_OnUnload found for \"" << library->GetPath() << "\"]: Calling...";
        JNI_OnUnloadFn jni_on_unload = reinterpret_cast<JNI_OnUnloadFn>(sym);
        jni_on_unload(soa.Vm(), nullptr);
      }
      delete library;
    }
  }

 private:
  AllocationTrackingSafeMap<std::string, SharedLibrary*, kAllocatorTagJNILibraries> libraries_
      GUARDED_BY(Locks::jni_libraries_lock_);
};

class JII {
 public:
  static jint DestroyJavaVM(JavaVM* vm) {
    if (vm == nullptr) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    delete raw_vm->GetRuntime();
    android::ResetNativeLoader();
    return JNI_OK;
  }

  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    return AttachCurrentThreadInternal(vm, p_env, thr_args, false);
  }

  static jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    return AttachCurrentThreadInternal(vm, p_env, thr_args, true);
  }

  static jint DetachCurrentThread(JavaVM* vm) {
    if (vm == nullptr || Thread::Current() == nullptr) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->GetRuntime();
    runtime->DetachCurrentThread();
    return JNI_OK;
  }

  static jint GetEnv(JavaVM* vm, void** env, jint version) {
    if (vm == nullptr || env == nullptr) {
      return JNI_ERR;
    }
    Thread* thread = Thread::Current();
    if (thread == nullptr) {
      *env = nullptr;
      return JNI_EDETACHED;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    return raw_vm->HandleGetEnv(env, version);
  }

 private:
  static jint AttachCurrentThreadInternal(JavaVM* vm, JNIEnv** p_env, void* raw_args, bool as_daemon) {
    if (vm == nullptr || p_env == nullptr) {
      return JNI_ERR;
    }

    // Return immediately if we're already attached.
    Thread* self = Thread::Current();
    if (self != nullptr) {
      *p_env = self->GetJniEnv();
      return JNI_OK;
    }

    Runtime* runtime = reinterpret_cast<JavaVMExt*>(vm)->GetRuntime();

    // No threads allowed in zygote mode.
    if (runtime->IsZygote()) {
      LOG(ERROR) << "Attempt to attach a thread in the zygote";
      return JNI_ERR;
    }

    JavaVMAttachArgs* args = static_cast<JavaVMAttachArgs*>(raw_args);
    const char* thread_name = nullptr;
    jobject thread_group = nullptr;
    if (args != nullptr) {
      if (JavaVMExt::IsBadJniVersion(args->version)) {
        LOG(ERROR) << "Bad JNI version passed to "
                   << (as_daemon ? "AttachCurrentThreadAsDaemon" : "AttachCurrentThread") << ": "
                   << args->version;
        return JNI_EVERSION;
      }
      thread_name = args->name;
      thread_group = args->group;
    }

    if (!runtime->AttachCurrentThread(thread_name, as_daemon, thread_group,
                                      !runtime->IsAotCompiler())) {
      *p_env = nullptr;
      return JNI_ERR;
    } else {
      *p_env = Thread::Current()->GetJniEnv();
      return JNI_OK;
    }
  }
};

const JNIInvokeInterface gJniInvokeInterface = {
  nullptr,  // reserved0
  nullptr,  // reserved1
  nullptr,  // reserved2
  JII::DestroyJavaVM,
  JII::AttachCurrentThread,
  JII::DetachCurrentThread,
  JII::GetEnv,
  JII::AttachCurrentThreadAsDaemon
};

JavaVMExt::JavaVMExt(Runtime* runtime,
                     const RuntimeArgumentMap& runtime_options,
                     std::string* error_msg)
    : runtime_(runtime),
      check_jni_abort_hook_(nullptr),
      check_jni_abort_hook_data_(nullptr),
      check_jni_(false),  // Initialized properly in the constructor body below.
      force_copy_(runtime_options.Exists(RuntimeArgumentMap::JniOptsForceCopy)),
      tracing_enabled_(runtime_options.Exists(RuntimeArgumentMap::JniTrace)
                       || VLOG_IS_ON(third_party_jni)),
      trace_(runtime_options.GetOrDefault(RuntimeArgumentMap::JniTrace)),
      globals_(kGlobalsMax, kGlobal, IndirectReferenceTable::ResizableCapacity::kNo, error_msg),
      libraries_(new Libraries),
      unchecked_functions_(&gJniInvokeInterface),
      weak_globals_(kWeakGlobalsMax,
                    kWeakGlobal,
                    IndirectReferenceTable::ResizableCapacity::kNo,
                    error_msg),
      allow_accessing_weak_globals_(true),
      weak_globals_add_condition_("weak globals add condition",
                                  (CHECK(Locks::jni_weak_globals_lock_ != nullptr),
                                   *Locks::jni_weak_globals_lock_)),
      env_hooks_() {
  functions = unchecked_functions_;
  SetCheckJniEnabled(runtime_options.Exists(RuntimeArgumentMap::CheckJni));
}

JavaVMExt::~JavaVMExt() {
}

// Checking "globals" and "weak_globals" usually requires locks, but we
// don't need the locks to check for validity when constructing the
// object. Use NO_THREAD_SAFETY_ANALYSIS for this.
std::unique_ptr<JavaVMExt> JavaVMExt::Create(Runtime* runtime,
                                             const RuntimeArgumentMap& runtime_options,
                                             std::string* error_msg) NO_THREAD_SAFETY_ANALYSIS {
  std::unique_ptr<JavaVMExt> java_vm(new JavaVMExt(runtime, runtime_options, error_msg));
  if (java_vm && java_vm->globals_.IsValid() && java_vm->weak_globals_.IsValid()) {
    return java_vm;
  }
  return nullptr;
}

jint JavaVMExt::HandleGetEnv(/*out*/void** env, jint version) {
  for (GetEnvHook hook : env_hooks_) {
    jint res = hook(this, env, version);
    if (res == JNI_OK) {
      return JNI_OK;
    } else if (res != JNI_EVERSION) {
      LOG(ERROR) << "Error returned from a plugin GetEnv handler! " << res;
      return res;
    }
  }
  LOG(ERROR) << "Bad JNI version passed to GetEnv: " << version;
  return JNI_EVERSION;
}

// Add a hook to handle getting environments from the GetEnv call.
void JavaVMExt::AddEnvironmentHook(GetEnvHook hook) {
  CHECK(hook != nullptr) << "environment hooks shouldn't be null!";
  env_hooks_.push_back(hook);
}

void JavaVMExt::JniAbort(const char* jni_function_name, const char* msg) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ArtMethod* current_method = self->GetCurrentMethod(nullptr);

  std::ostringstream os;
  os << "JNI DETECTED ERROR IN APPLICATION: " << msg;

  if (jni_function_name != nullptr) {
    os << "\n    in call to " << jni_function_name;
  }
  // TODO: is this useful given that we're about to dump the calling thread's stack?
  if (current_method != nullptr) {
    os << "\n    from " << current_method->PrettyMethod();
  }
  os << "\n";
  self->Dump(os);

  if (check_jni_abort_hook_ != nullptr) {
    check_jni_abort_hook_(check_jni_abort_hook_data_, os.str());
  } else {
    // Ensure that we get a native stack trace for this thread.
    ScopedThreadSuspension sts(self, kNative);
    LOG(FATAL) << os.str();
    UNREACHABLE();
  }
}

void JavaVMExt::JniAbortV(const char* jni_function_name, const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  JniAbort(jni_function_name, msg.c_str());
}

void JavaVMExt::JniAbortF(const char* jni_function_name, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  JniAbortV(jni_function_name, fmt, args);
  va_end(args);
}

bool JavaVMExt::ShouldTrace(ArtMethod* method) {
  // Fast where no tracing is enabled.
  if (trace_.empty() && !VLOG_IS_ON(third_party_jni)) {
    return false;
  }
  // Perform checks based on class name.
  StringPiece class_name(method->GetDeclaringClassDescriptor());
  if (!trace_.empty() && class_name.find(trace_) != std::string::npos) {
    return true;
  }
  if (!VLOG_IS_ON(third_party_jni)) {
    return false;
  }
  // Return true if we're trying to log all third-party JNI activity and 'method' doesn't look
  // like part of Android.
  static const char* gBuiltInPrefixes[] = {
      "Landroid/",
      "Lcom/android/",
      "Lcom/google/android/",
      "Ldalvik/",
      "Ljava/",
      "Ljavax/",
      "Llibcore/",
      "Lorg/apache/harmony/",
  };
  for (size_t i = 0; i < arraysize(gBuiltInPrefixes); ++i) {
    if (class_name.starts_with(gBuiltInPrefixes[i])) {
      return false;
    }
  }
  return true;
}

jobject JavaVMExt::AddGlobalRef(Thread* self, ObjPtr<mirror::Object> obj) {
  // Check for null after decoding the object to handle cleared weak globals.
  if (obj == nullptr) {
    return nullptr;
  }
  WriterMutexLock mu(self, *Locks::jni_globals_lock_);
  IndirectRef ref = globals_.Add(kIRTFirstSegment, obj);
  return reinterpret_cast<jobject>(ref);
}

jweak JavaVMExt::AddWeakGlobalRef(Thread* self, ObjPtr<mirror::Object> obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  // CMS needs this to block for concurrent reference processing because an object allocated during
  // the GC won't be marked and concurrent reference processing would incorrectly clear the JNI weak
  // ref. But CC (kUseReadBarrier == true) doesn't because of the to-space invariant.
  while (!kUseReadBarrier && UNLIKELY(!MayAccessWeakGlobals(self))) {
    // Check and run the empty checkpoint before blocking so the empty checkpoint will work in the
    // presence of threads blocking for weak ref access.
    self->CheckEmptyCheckpointFromWeakRefAccess(Locks::jni_weak_globals_lock_);
    weak_globals_add_condition_.WaitHoldingLocks(self);
  }
  IndirectRef ref = weak_globals_.Add(kIRTFirstSegment, obj);
  return reinterpret_cast<jweak>(ref);
}

void JavaVMExt::DeleteGlobalRef(Thread* self, jobject obj) {
  if (obj == nullptr) {
    return;
  }
  WriterMutexLock mu(self, *Locks::jni_globals_lock_);
  if (!globals_.Remove(kIRTFirstSegment, obj)) {
    LOG(WARNING) << "JNI WARNING: DeleteGlobalRef(" << obj << ") "
                 << "failed to find entry";
  }
}

void JavaVMExt::DeleteWeakGlobalRef(Thread* self, jweak obj) {
  if (obj == nullptr) {
    return;
  }
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  if (!weak_globals_.Remove(kIRTFirstSegment, obj)) {
    LOG(WARNING) << "JNI WARNING: DeleteWeakGlobalRef(" << obj << ") "
                 << "failed to find entry";
  }
}

static void ThreadEnableCheckJni(Thread* thread, void* arg) {
  bool* check_jni = reinterpret_cast<bool*>(arg);
  thread->GetJniEnv()->SetCheckJniEnabled(*check_jni);
}

bool JavaVMExt::SetCheckJniEnabled(bool enabled) {
  bool old_check_jni = check_jni_;
  check_jni_ = enabled;
  functions = enabled ? GetCheckJniInvokeInterface() : unchecked_functions_;
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  runtime_->GetThreadList()->ForEach(ThreadEnableCheckJni, &check_jni_);
  return old_check_jni;
}

void JavaVMExt::DumpForSigQuit(std::ostream& os) {
  os << "JNI: CheckJNI is " << (check_jni_ ? "on" : "off");
  if (force_copy_) {
    os << " (with forcecopy)";
  }
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, *Locks::jni_globals_lock_);
    os << "; globals=" << globals_.Capacity();
  }
  {
    MutexLock mu(self, *Locks::jni_weak_globals_lock_);
    if (weak_globals_.Capacity() > 0) {
      os << " (plus " << weak_globals_.Capacity() << " weak)";
    }
  }
  os << '\n';

  {
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    os << "Libraries: " << Dumpable<Libraries>(*libraries_) << " (" << libraries_->size() << ")\n";
  }
}

void JavaVMExt::DisallowNewWeakGlobals() {
  CHECK(!kUseReadBarrier);
  Thread* const self = Thread::Current();
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  // DisallowNewWeakGlobals is only called by CMS during the pause. It is required to have the
  // mutator lock exclusively held so that we don't have any threads in the middle of
  // DecodeWeakGlobal.
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  allow_accessing_weak_globals_.StoreSequentiallyConsistent(false);
}

void JavaVMExt::AllowNewWeakGlobals() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  allow_accessing_weak_globals_.StoreSequentiallyConsistent(true);
  weak_globals_add_condition_.Broadcast(self);
}

void JavaVMExt::BroadcastForNewWeakGlobals() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  weak_globals_add_condition_.Broadcast(self);
}

ObjPtr<mirror::Object> JavaVMExt::DecodeGlobal(IndirectRef ref) {
  return globals_.SynchronizedGet(ref);
}

void JavaVMExt::UpdateGlobal(Thread* self, IndirectRef ref, ObjPtr<mirror::Object> result) {
  WriterMutexLock mu(self, *Locks::jni_globals_lock_);
  globals_.Update(ref, result);
}

inline bool JavaVMExt::MayAccessWeakGlobals(Thread* self) const {
  return MayAccessWeakGlobalsUnlocked(self);
}

inline bool JavaVMExt::MayAccessWeakGlobalsUnlocked(Thread* self) const {
  DCHECK(self != nullptr);
  return kUseReadBarrier ?
      self->GetWeakRefAccessEnabled() :
      allow_accessing_weak_globals_.LoadSequentiallyConsistent();
}

ObjPtr<mirror::Object> JavaVMExt::DecodeWeakGlobal(Thread* self, IndirectRef ref) {
  // It is safe to access GetWeakRefAccessEnabled without the lock since CC uses checkpoints to call
  // SetWeakRefAccessEnabled, and the other collectors only modify allow_accessing_weak_globals_
  // when the mutators are paused.
  // This only applies in the case where MayAccessWeakGlobals goes from false to true. In the other
  // case, it may be racy, this is benign since DecodeWeakGlobalLocked does the correct behavior
  // if MayAccessWeakGlobals is false.
  DCHECK_EQ(IndirectReferenceTable::GetIndirectRefKind(ref), kWeakGlobal);
  if (LIKELY(MayAccessWeakGlobalsUnlocked(self))) {
    return weak_globals_.SynchronizedGet(ref);
  }
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  return DecodeWeakGlobalLocked(self, ref);
}

ObjPtr<mirror::Object> JavaVMExt::DecodeWeakGlobalLocked(Thread* self, IndirectRef ref) {
  if (kDebugLocking) {
    Locks::jni_weak_globals_lock_->AssertHeld(self);
  }
  while (UNLIKELY(!MayAccessWeakGlobals(self))) {
    // Check and run the empty checkpoint before blocking so the empty checkpoint will work in the
    // presence of threads blocking for weak ref access.
    self->CheckEmptyCheckpointFromWeakRefAccess(Locks::jni_weak_globals_lock_);
    weak_globals_add_condition_.WaitHoldingLocks(self);
  }
  return weak_globals_.Get(ref);
}

ObjPtr<mirror::Object> JavaVMExt::DecodeWeakGlobalDuringShutdown(Thread* self, IndirectRef ref) {
  DCHECK_EQ(IndirectReferenceTable::GetIndirectRefKind(ref), kWeakGlobal);
  DCHECK(Runtime::Current()->IsShuttingDown(self));
  if (self != nullptr) {
    return DecodeWeakGlobal(self, ref);
  }
  // self can be null during a runtime shutdown. ~Runtime()->~ClassLinker()->DecodeWeakGlobal().
  if (!kUseReadBarrier) {
    DCHECK(allow_accessing_weak_globals_.LoadSequentiallyConsistent());
  }
  return weak_globals_.SynchronizedGet(ref);
}

bool JavaVMExt::IsWeakGlobalCleared(Thread* self, IndirectRef ref) {
  DCHECK_EQ(IndirectReferenceTable::GetIndirectRefKind(ref), kWeakGlobal);
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  while (UNLIKELY(!MayAccessWeakGlobals(self))) {
    // Check and run the empty checkpoint before blocking so the empty checkpoint will work in the
    // presence of threads blocking for weak ref access.
    self->CheckEmptyCheckpointFromWeakRefAccess(Locks::jni_weak_globals_lock_);
    weak_globals_add_condition_.WaitHoldingLocks(self);
  }
  // When just checking a weak ref has been cleared, avoid triggering the read barrier in decode
  // (DecodeWeakGlobal) so that we won't accidentally mark the object alive. Since the cleared
  // sentinel is a non-moving object, we can compare the ref to it without the read barrier and
  // decide if it's cleared.
  return Runtime::Current()->IsClearedJniWeakGlobal(weak_globals_.Get<kWithoutReadBarrier>(ref));
}

void JavaVMExt::UpdateWeakGlobal(Thread* self, IndirectRef ref, ObjPtr<mirror::Object> result) {
  MutexLock mu(self, *Locks::jni_weak_globals_lock_);
  weak_globals_.Update(ref, result);
}

void JavaVMExt::DumpReferenceTables(std::ostream& os) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, *Locks::jni_globals_lock_);
    globals_.Dump(os);
  }
  {
    MutexLock mu(self, *Locks::jni_weak_globals_lock_);
    weak_globals_.Dump(os);
  }
}

void JavaVMExt::UnloadNativeLibraries() {
  libraries_.get()->UnloadNativeLibraries();
}

bool JavaVMExt::LoadNativeLibrary(JNIEnv* env,
                                  const std::string& path,
                                  jobject class_loader,
                                  jstring library_path,
                                  std::string* error_msg) {
  error_msg->clear();

  // See if we've already loaded this library.  If we have, and the class loader
  // matches, return successfully without doing anything.
  // TODO: for better results we should canonicalize the pathname (or even compare
  // inodes). This implementation is fine if everybody is using System.loadLibrary.
  SharedLibrary* library;
  Thread* self = Thread::Current();
  {
    // TODO: move the locking (and more of this logic) into Libraries.
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    library = libraries_->Get(path);
  }
  void* class_loader_allocator = nullptr;
  {
    ScopedObjectAccess soa(env);
    // As the incoming class loader is reachable/alive during the call of this function,
    // it's okay to decode it without worrying about unexpectedly marking it alive.
    ObjPtr<mirror::ClassLoader> loader = soa.Decode<mirror::ClassLoader>(class_loader);

    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (class_linker->IsBootClassLoader(soa, loader.Ptr())) {
      loader = nullptr;
      class_loader = nullptr;
    }

    class_loader_allocator = class_linker->GetAllocatorForClassLoader(loader.Ptr());
    CHECK(class_loader_allocator != nullptr);
  }
  if (library != nullptr) {
    // Use the allocator pointers for class loader equality to avoid unnecessary weak root decode.
    if (library->GetClassLoaderAllocator() != class_loader_allocator) {
      // The library will be associated with class_loader. The JNI
      // spec says we can't load the same library into more than one
      // class loader.
      StringAppendF(error_msg, "Shared library \"%s\" already opened by "
          "ClassLoader %p; can't open in ClassLoader %p",
          path.c_str(), library->GetClassLoader(), class_loader);
      LOG(WARNING) << error_msg;
      return false;
    }
    VLOG(jni) << "[Shared library \"" << path << "\" already loaded in "
              << " ClassLoader " << class_loader << "]";
    if (!library->CheckOnLoadResult()) {
      StringAppendF(error_msg, "JNI_OnLoad failed on a previous attempt "
          "to load \"%s\"", path.c_str());
      return false;
    }
    return true;
  }

  // Open the shared library.  Because we're using a full path, the system
  // doesn't have to search through LD_LIBRARY_PATH.  (It may do so to
  // resolve this library's dependencies though.)

  // Failures here are expected when java.library.path has several entries
  // and we have to hunt for the lib.

  // Below we dlopen but there is no paired dlclose, this would be necessary if we supported
  // class unloading. Libraries will only be unloaded when the reference count (incremented by
  // dlopen) becomes zero from dlclose.

  Locks::mutator_lock_->AssertNotHeld(self);
  const char* path_str = path.empty() ? nullptr : path.c_str();
  bool needs_native_bridge = false;
  void* handle = android::OpenNativeLibrary(env,
                                            runtime_->GetTargetSdkVersion(),
                                            path_str,
                                            class_loader,
                                            library_path,
                                            &needs_native_bridge,
                                            error_msg);

  VLOG(jni) << "[Call to dlopen(\"" << path << "\", RTLD_NOW) returned " << handle << "]";

  if (handle == nullptr) {
    VLOG(jni) << "dlopen(\"" << path << "\", RTLD_NOW) failed: " << *error_msg;
    return false;
  }

  if (env->ExceptionCheck() == JNI_TRUE) {
    LOG(ERROR) << "Unexpected exception:";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  // Create a new entry.
  // TODO: move the locking (and more of this logic) into Libraries.
  bool created_library = false;
  {
    // Create SharedLibrary ahead of taking the libraries lock to maintain lock ordering.
    std::unique_ptr<SharedLibrary> new_library(
        new SharedLibrary(env,
                          self,
                          path,
                          handle,
                          needs_native_bridge,
                          class_loader,
                          class_loader_allocator));

    MutexLock mu(self, *Locks::jni_libraries_lock_);
    library = libraries_->Get(path);
    if (library == nullptr) {  // We won race to get libraries_lock.
      library = new_library.release();
      libraries_->Put(path, library);
      created_library = true;
    }
  }
  if (!created_library) {
    LOG(INFO) << "WOW: we lost a race to add shared library: "
        << "\"" << path << "\" ClassLoader=" << class_loader;
    return library->CheckOnLoadResult();
  }
  VLOG(jni) << "[Added shared library \"" << path << "\" for ClassLoader " << class_loader << "]";

  bool was_successful = false;
  void* sym = library->FindSymbol("JNI_OnLoad", nullptr);
  if (sym == nullptr) {
    VLOG(jni) << "[No JNI_OnLoad found in \"" << path << "\"]";
    was_successful = true;
  } else {
    // Call JNI_OnLoad.  We have to override the current class
    // loader, which will always be "null" since the stuff at the
    // top of the stack is around Runtime.loadLibrary().  (See
    // the comments in the JNI FindClass function.)
    ScopedLocalRef<jobject> old_class_loader(env, env->NewLocalRef(self->GetClassLoaderOverride()));
    self->SetClassLoaderOverride(class_loader);

    VLOG(jni) << "[Calling JNI_OnLoad in \"" << path << "\"]";
    typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
    JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
    int version = (*jni_on_load)(this, nullptr);

    if (runtime_->GetTargetSdkVersion() != 0 && runtime_->GetTargetSdkVersion() <= 21) {
      // Make sure that sigchain owns SIGSEGV.
      EnsureFrontOfChain(SIGSEGV);
    }

    self->SetClassLoaderOverride(old_class_loader.get());

    if (version == JNI_ERR) {
      StringAppendF(error_msg, "JNI_ERR returned from JNI_OnLoad in \"%s\"", path.c_str());
    } else if (JavaVMExt::IsBadJniVersion(version)) {
      StringAppendF(error_msg, "Bad JNI version returned from JNI_OnLoad in \"%s\": %d",
                    path.c_str(), version);
      // It's unwise to call dlclose() here, but we can mark it
      // as bad and ensure that future load attempts will fail.
      // We don't know how far JNI_OnLoad got, so there could
      // be some partially-initialized stuff accessible through
      // newly-registered native method calls.  We could try to
      // unregister them, but that doesn't seem worthwhile.
    } else {
      was_successful = true;
    }
    VLOG(jni) << "[Returned " << (was_successful ? "successfully" : "failure")
              << " from JNI_OnLoad in \"" << path << "\"]";
  }

  library->SetResult(was_successful);
  return was_successful;
}

static void* FindCodeForNativeMethodInAgents(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string jni_short_name(m->JniShortName());
  std::string jni_long_name(m->JniLongName());
  for (const ti::Agent& agent : Runtime::Current()->GetAgents()) {
    void* fn = agent.FindSymbol(jni_short_name);
    if (fn != nullptr) {
      VLOG(jni) << "Found implementation for " << m->PrettyMethod()
                << " (symbol: " << jni_short_name << ") in " << agent;
      return fn;
    }
    fn = agent.FindSymbol(jni_long_name);
    if (fn != nullptr) {
      VLOG(jni) << "Found implementation for " << m->PrettyMethod()
                << " (symbol: " << jni_long_name << ") in " << agent;
      return fn;
    }
  }
  return nullptr;
}

void* JavaVMExt::FindCodeForNativeMethod(ArtMethod* m) {
  CHECK(m->IsNative());
  mirror::Class* c = m->GetDeclaringClass();
  // If this is a static method, it could be called before the class has been initialized.
  CHECK(c->IsInitializing()) << c->GetStatus() << " " << m->PrettyMethod();
  std::string detail;
  void* native_method;
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    native_method = libraries_->FindNativeMethod(m, detail);
  }
  if (native_method == nullptr) {
    // Lookup JNI native methods from native TI Agent libraries. See runtime/ti/agent.h for more
    // information. Agent libraries are searched for native methods after all jni libraries.
    native_method = FindCodeForNativeMethodInAgents(m);
  }
  // Throwing can cause libraries_lock to be reacquired.
  if (native_method == nullptr) {
    LOG(ERROR) << detail;
    self->ThrowNewException("Ljava/lang/UnsatisfiedLinkError;", detail.c_str());
  }
  return native_method;
}

void JavaVMExt::SweepJniWeakGlobals(IsMarkedVisitor* visitor) {
  MutexLock mu(Thread::Current(), *Locks::jni_weak_globals_lock_);
  Runtime* const runtime = Runtime::Current();
  for (auto* entry : weak_globals_) {
    // Need to skip null here to distinguish between null entries and cleared weak ref entries.
    if (!entry->IsNull()) {
      // Since this is called by the GC, we don't need a read barrier.
      mirror::Object* obj = entry->Read<kWithoutReadBarrier>();
      mirror::Object* new_obj = visitor->IsMarked(obj);
      if (new_obj == nullptr) {
        new_obj = runtime->GetClearedJniWeakGlobal();
      }
      *entry = GcRoot<mirror::Object>(new_obj);
    }
  }
}

void JavaVMExt::TrimGlobals() {
  WriterMutexLock mu(Thread::Current(), *Locks::jni_globals_lock_);
  globals_.Trim();
}

void JavaVMExt::VisitRoots(RootVisitor* visitor) {
  Thread* self = Thread::Current();
  ReaderMutexLock mu(self, *Locks::jni_globals_lock_);
  globals_.VisitRoots(visitor, RootInfo(kRootJNIGlobal));
  // The weak_globals table is visited by the GC itself (because it mutates the table).
}

// JNI Invocation interface.

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, JNIEnv** p_env, void* vm_args) {
  ScopedTrace trace(__FUNCTION__);
  const JavaVMInitArgs* args = static_cast<JavaVMInitArgs*>(vm_args);
  if (JavaVMExt::IsBadJniVersion(args->version)) {
    LOG(ERROR) << "Bad JNI version passed to CreateJavaVM: " << args->version;
    return JNI_EVERSION;
  }
  RuntimeOptions options;
  for (int i = 0; i < args->nOptions; ++i) {
    JavaVMOption* option = &args->options[i];
    options.push_back(std::make_pair(std::string(option->optionString), option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  if (!Runtime::Create(options, ignore_unrecognized)) {
    return JNI_ERR;
  }

  // Initialize native loader. This step makes sure we have
  // everything set up before we start using JNI.
  android::InitializeNativeLoader();

  Runtime* runtime = Runtime::Current();
  bool started = runtime->Start();
  if (!started) {
    delete Thread::Current()->GetJniEnv();
    delete runtime->GetJavaVM();
    LOG(WARNING) << "CreateJavaVM failed";
    return JNI_ERR;
  }

  *p_env = Thread::Current()->GetJniEnv();
  *p_vm = runtime->GetJavaVM();
  return JNI_OK;
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vms_buf, jsize buf_len, jsize* vm_count) {
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr || buf_len == 0) {
    *vm_count = 0;
  } else {
    *vm_count = 1;
    vms_buf[0] = runtime->GetJavaVM();
  }
  return JNI_OK;
}

// Historically unsupported.
extern "C" jint JNI_GetDefaultJavaVMInitArgs(void* /*vm_args*/) {
  return JNI_ERR;
}

}  // namespace art
