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

#include "thread.h"

#if !defined(__APPLE__)
#include <sched.h>
#endif

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>
#include <sstream>

#include "android-base/stringprintf.h"

#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/bit_utils.h"
#include "base/memory_tool.h"
#include "base/mutex.h"
#include "base/timing_logger.h"
#include "base/to_str.h"
#include "base/systrace.h"
#include "class_linker-inl.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "dex_file_annotations.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "gc/space/space-inl.h"
#include "handle_scope-inl.h"
#include "indirect_reference_table-inl.h"
#include "java_vm_ext.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/stack_trace_element.h"
#include "monitor.h"
#include "native_stack_dump.h"
#include "nth_caller_visitor.h"
#include "oat_quick_method_header.h"
#include "obj_ptr-inl.h"
#include "object_lock.h"
#include "quick_exception_handler.h"
#include "quick/quick_method_frame_info.h"
#include "read_barrier-inl.h"
#include "reflection.h"
#include "runtime.h"
#include "runtime_callbacks.h"
#include "scoped_thread_state_change-inl.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "stack.h"
#include "stack_map.h"
#include "thread_list.h"
#include "thread-inl.h"
#include "utils.h"
#include "verifier/method_verifier.h"
#include "verify_object.h"
#include "well_known_classes.h"
#include "interpreter/interpreter.h"

#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
#endif  // ART_USE_FUTEXES

namespace art {

using android::base::StringAppendV;
using android::base::StringPrintf;

extern "C" NO_RETURN void artDeoptimize(Thread* self);

bool Thread::is_started_ = false;
pthread_key_t Thread::pthread_key_self_;
ConditionVariable* Thread::resume_cond_ = nullptr;
const size_t Thread::kStackOverflowImplicitCheckSize = GetStackOverflowReservedBytes(kRuntimeISA);
bool (*Thread::is_sensitive_thread_hook_)() = nullptr;
Thread* Thread::jit_sensitive_thread_ = nullptr;

static constexpr bool kVerifyImageObjectsMarked = kIsDebugBuild;

// For implicit overflow checks we reserve an extra piece of memory at the bottom
// of the stack (lowest memory).  The higher portion of the memory
// is protected against reads and the lower is available for use while
// throwing the StackOverflow exception.
constexpr size_t kStackOverflowProtectedSize = 4 * kMemoryToolStackGuardSizeScale * KB;

static const char* kThreadNameDuringStartup = "<native thread without managed peer>";

void Thread::InitCardTable() {
  tlsPtr_.card_table = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}

static void UnimplementedEntryPoint() {
  UNIMPLEMENTED(FATAL);
}

void InitEntryPoints(JniEntryPoints* jpoints, QuickEntryPoints* qpoints);
void UpdateReadBarrierEntrypoints(QuickEntryPoints* qpoints, bool is_marking);

void Thread::SetIsGcMarkingAndUpdateEntrypoints(bool is_marking) {
  CHECK(kUseReadBarrier);
  tls32_.is_gc_marking = is_marking;
  UpdateReadBarrierEntrypoints(&tlsPtr_.quick_entrypoints, is_marking);
  ResetQuickAllocEntryPointsForThread(is_marking);
}

void Thread::InitTlsEntryPoints() {
  // Insert a placeholder so we can easily tell if we call an unimplemented entry point.
  uintptr_t* begin = reinterpret_cast<uintptr_t*>(&tlsPtr_.jni_entrypoints);
  uintptr_t* end = reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(&tlsPtr_.quick_entrypoints) + sizeof(tlsPtr_.quick_entrypoints));
  for (uintptr_t* it = begin; it != end; ++it) {
    *it = reinterpret_cast<uintptr_t>(UnimplementedEntryPoint);
  }
  InitEntryPoints(&tlsPtr_.jni_entrypoints, &tlsPtr_.quick_entrypoints);
}

void Thread::ResetQuickAllocEntryPointsForThread(bool is_marking) {
  if (kUseReadBarrier && kRuntimeISA != kX86_64) {
    // Allocation entrypoint switching is currently only implemented for X86_64.
    is_marking = true;
  }
  ResetQuickAllocEntryPoints(&tlsPtr_.quick_entrypoints, is_marking);
}

class DeoptimizationContextRecord {
 public:
  DeoptimizationContextRecord(const JValue& ret_val,
                              bool is_reference,
                              bool from_code,
                              ObjPtr<mirror::Throwable> pending_exception,
                              DeoptimizationContextRecord* link)
      : ret_val_(ret_val),
        is_reference_(is_reference),
        from_code_(from_code),
        pending_exception_(pending_exception.Ptr()),
        link_(link) {}

  JValue GetReturnValue() const { return ret_val_; }
  bool IsReference() const { return is_reference_; }
  bool GetFromCode() const { return from_code_; }
  ObjPtr<mirror::Throwable> GetPendingException() const { return pending_exception_; }
  DeoptimizationContextRecord* GetLink() const { return link_; }
  mirror::Object** GetReturnValueAsGCRoot() {
    DCHECK(is_reference_);
    return ret_val_.GetGCRoot();
  }
  mirror::Object** GetPendingExceptionAsGCRoot() {
    return reinterpret_cast<mirror::Object**>(&pending_exception_);
  }

 private:
  // The value returned by the method at the top of the stack before deoptimization.
  JValue ret_val_;

  // Indicates whether the returned value is a reference. If so, the GC will visit it.
  const bool is_reference_;

  // Whether the context was created from an explicit deoptimization in the code.
  const bool from_code_;

  // The exception that was pending before deoptimization (or null if there was no pending
  // exception).
  mirror::Throwable* pending_exception_;

  // A link to the previous DeoptimizationContextRecord.
  DeoptimizationContextRecord* const link_;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizationContextRecord);
};

class StackedShadowFrameRecord {
 public:
  StackedShadowFrameRecord(ShadowFrame* shadow_frame,
                           StackedShadowFrameType type,
                           StackedShadowFrameRecord* link)
      : shadow_frame_(shadow_frame),
        type_(type),
        link_(link) {}

  ShadowFrame* GetShadowFrame() const { return shadow_frame_; }
  StackedShadowFrameType GetType() const { return type_; }
  StackedShadowFrameRecord* GetLink() const { return link_; }

 private:
  ShadowFrame* const shadow_frame_;
  const StackedShadowFrameType type_;
  StackedShadowFrameRecord* const link_;

  DISALLOW_COPY_AND_ASSIGN(StackedShadowFrameRecord);
};

void Thread::PushDeoptimizationContext(const JValue& return_value,
                                       bool is_reference,
                                       bool from_code,
                                       ObjPtr<mirror::Throwable> exception) {
  DeoptimizationContextRecord* record = new DeoptimizationContextRecord(
      return_value,
      is_reference,
      from_code,
      exception,
      tlsPtr_.deoptimization_context_stack);
  tlsPtr_.deoptimization_context_stack = record;
}

void Thread::PopDeoptimizationContext(JValue* result,
                                      ObjPtr<mirror::Throwable>* exception,
                                      bool* from_code) {
  AssertHasDeoptimizationContext();
  DeoptimizationContextRecord* record = tlsPtr_.deoptimization_context_stack;
  tlsPtr_.deoptimization_context_stack = record->GetLink();
  result->SetJ(record->GetReturnValue().GetJ());
  *exception = record->GetPendingException();
  *from_code = record->GetFromCode();
  delete record;
}

void Thread::AssertHasDeoptimizationContext() {
  CHECK(tlsPtr_.deoptimization_context_stack != nullptr)
      << "No deoptimization context for thread " << *this;
}

void Thread::PushStackedShadowFrame(ShadowFrame* sf, StackedShadowFrameType type) {
  StackedShadowFrameRecord* record = new StackedShadowFrameRecord(
      sf, type, tlsPtr_.stacked_shadow_frame_record);
  tlsPtr_.stacked_shadow_frame_record = record;
}

ShadowFrame* Thread::PopStackedShadowFrame(StackedShadowFrameType type, bool must_be_present) {
  StackedShadowFrameRecord* record = tlsPtr_.stacked_shadow_frame_record;
  if (must_be_present) {
    DCHECK(record != nullptr);
  } else {
    if (record == nullptr || record->GetType() != type) {
      return nullptr;
    }
  }
  tlsPtr_.stacked_shadow_frame_record = record->GetLink();
  ShadowFrame* shadow_frame = record->GetShadowFrame();
  delete record;
  return shadow_frame;
}

class FrameIdToShadowFrame {
 public:
  static FrameIdToShadowFrame* Create(size_t frame_id,
                                      ShadowFrame* shadow_frame,
                                      FrameIdToShadowFrame* next,
                                      size_t num_vregs) {
    // Append a bool array at the end to keep track of what vregs are updated by the debugger.
    uint8_t* memory = new uint8_t[sizeof(FrameIdToShadowFrame) + sizeof(bool) * num_vregs];
    return new (memory) FrameIdToShadowFrame(frame_id, shadow_frame, next);
  }

  static void Delete(FrameIdToShadowFrame* f) {
    uint8_t* memory = reinterpret_cast<uint8_t*>(f);
    delete[] memory;
  }

  size_t GetFrameId() const { return frame_id_; }
  ShadowFrame* GetShadowFrame() const { return shadow_frame_; }
  FrameIdToShadowFrame* GetNext() const { return next_; }
  void SetNext(FrameIdToShadowFrame* next) { next_ = next; }
  bool* GetUpdatedVRegFlags() {
    return updated_vreg_flags_;
  }

 private:
  FrameIdToShadowFrame(size_t frame_id,
                       ShadowFrame* shadow_frame,
                       FrameIdToShadowFrame* next)
      : frame_id_(frame_id),
        shadow_frame_(shadow_frame),
        next_(next) {}

  const size_t frame_id_;
  ShadowFrame* const shadow_frame_;
  FrameIdToShadowFrame* next_;
  bool updated_vreg_flags_[0];

  DISALLOW_COPY_AND_ASSIGN(FrameIdToShadowFrame);
};

static FrameIdToShadowFrame* FindFrameIdToShadowFrame(FrameIdToShadowFrame* head,
                                                      size_t frame_id) {
  FrameIdToShadowFrame* found = nullptr;
  for (FrameIdToShadowFrame* record = head; record != nullptr; record = record->GetNext()) {
    if (record->GetFrameId() == frame_id) {
      if (kIsDebugBuild) {
        // Sanity check we have at most one record for this frame.
        CHECK(found == nullptr) << "Multiple records for the frame " << frame_id;
        found = record;
      } else {
        return record;
      }
    }
  }
  return found;
}

ShadowFrame* Thread::FindDebuggerShadowFrame(size_t frame_id) {
  FrameIdToShadowFrame* record = FindFrameIdToShadowFrame(
      tlsPtr_.frame_id_to_shadow_frame, frame_id);
  if (record != nullptr) {
    return record->GetShadowFrame();
  }
  return nullptr;
}

// Must only be called when FindDebuggerShadowFrame(frame_id) returns non-nullptr.
bool* Thread::GetUpdatedVRegFlags(size_t frame_id) {
  FrameIdToShadowFrame* record = FindFrameIdToShadowFrame(
      tlsPtr_.frame_id_to_shadow_frame, frame_id);
  CHECK(record != nullptr);
  return record->GetUpdatedVRegFlags();
}

ShadowFrame* Thread::FindOrCreateDebuggerShadowFrame(size_t frame_id,
                                                     uint32_t num_vregs,
                                                     ArtMethod* method,
                                                     uint32_t dex_pc) {
  ShadowFrame* shadow_frame = FindDebuggerShadowFrame(frame_id);
  if (shadow_frame != nullptr) {
    return shadow_frame;
  }
  VLOG(deopt) << "Create pre-deopted ShadowFrame for " << ArtMethod::PrettyMethod(method);
  shadow_frame = ShadowFrame::CreateDeoptimizedFrame(num_vregs, nullptr, method, dex_pc);
  FrameIdToShadowFrame* record = FrameIdToShadowFrame::Create(frame_id,
                                                              shadow_frame,
                                                              tlsPtr_.frame_id_to_shadow_frame,
                                                              num_vregs);
  for (uint32_t i = 0; i < num_vregs; i++) {
    // Do this to clear all references for root visitors.
    shadow_frame->SetVRegReference(i, nullptr);
    // This flag will be changed to true if the debugger modifies the value.
    record->GetUpdatedVRegFlags()[i] = false;
  }
  tlsPtr_.frame_id_to_shadow_frame = record;
  return shadow_frame;
}

void Thread::RemoveDebuggerShadowFrameMapping(size_t frame_id) {
  FrameIdToShadowFrame* head = tlsPtr_.frame_id_to_shadow_frame;
  if (head->GetFrameId() == frame_id) {
    tlsPtr_.frame_id_to_shadow_frame = head->GetNext();
    FrameIdToShadowFrame::Delete(head);
    return;
  }
  FrameIdToShadowFrame* prev = head;
  for (FrameIdToShadowFrame* record = head->GetNext();
       record != nullptr;
       prev = record, record = record->GetNext()) {
    if (record->GetFrameId() == frame_id) {
      prev->SetNext(record->GetNext());
      FrameIdToShadowFrame::Delete(record);
      return;
    }
  }
  LOG(FATAL) << "No shadow frame for frame " << frame_id;
  UNREACHABLE();
}

void Thread::InitTid() {
  tls32_.tid = ::art::GetTid();
}

void Thread::InitAfterFork() {
  // One thread (us) survived the fork, but we have a new tid so we need to
  // update the value stashed in this Thread*.
  InitTid();
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    LOG(ERROR) << "Thread attaching to non-existent runtime: " << *self;
    return nullptr;
  }
  {
    // TODO: pass self to MutexLock - requires self to equal Thread::Current(), which is only true
    //       after self->Init().
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    // Check that if we got here we cannot be shutting down (as shutdown should never have started
    // while threads are being born).
    CHECK(!runtime->IsShuttingDownLocked());
    // Note: given that the JNIEnv is created in the parent thread, the only failure point here is
    //       a mess in InitStackHwm. We do not have a reasonable way to recover from that, so abort
    //       the runtime in such a case. In case this ever changes, we need to make sure here to
    //       delete the tmp_jni_env, as we own it at this point.
    CHECK(self->Init(runtime->GetThreadList(), runtime->GetJavaVM(), self->tlsPtr_.tmp_jni_env));
    self->tlsPtr_.tmp_jni_env = nullptr;
    Runtime::Current()->EndThreadBirth();
  }
  {
    ScopedObjectAccess soa(self);
    self->InitStringEntryPoints();

    // Copy peer into self, deleting global reference when done.
    CHECK(self->tlsPtr_.jpeer != nullptr);
    self->tlsPtr_.opeer = soa.Decode<mirror::Object>(self->tlsPtr_.jpeer).Ptr();
    self->GetJniEnv()->DeleteGlobalRef(self->tlsPtr_.jpeer);
    self->tlsPtr_.jpeer = nullptr;
    self->SetThreadName(self->GetThreadName()->ToModifiedUtf8().c_str());

    ArtField* priorityField = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_priority);
    self->SetNativePriority(priorityField->GetInt(self->tlsPtr_.opeer));

    runtime->GetRuntimeCallbacks()->ThreadStart(self);

    // Invoke the 'run' method of our java.lang.Thread.
    ObjPtr<mirror::Object> receiver = self->tlsPtr_.opeer;
    jmethodID mid = WellKnownClasses::java_lang_Thread_run;
    ScopedLocalRef<jobject> ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    InvokeVirtualOrInterfaceWithJValues(soa, ref.get(), mid, nullptr);
  }
  // Detach and delete self.
  Runtime::Current()->GetThreadList()->Unregister(self);

  return nullptr;
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessAlreadyRunnable& soa,
                                  ObjPtr<mirror::Object> thread_peer) {
  ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_nativePeer);
  Thread* result = reinterpret_cast<Thread*>(static_cast<uintptr_t>(f->GetLong(thread_peer)));
  // Sanity check that if we have a result it is either suspended or we hold the thread_list_lock_
  // to stop it from going away.
  if (kIsDebugBuild) {
    MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
    if (result != nullptr && !result->IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(soa.Self());
    }
  }
  return result;
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessAlreadyRunnable& soa,
                                  jobject java_thread) {
  return FromManagedThread(soa, soa.Decode<mirror::Object>(java_thread).Ptr());
}

static size_t FixStackSize(size_t stack_size) {
  // A stack size of zero means "use the default".
  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

  // Dalvik used the bionic pthread default stack size for native threads,
  // so include that here to support apps that expect large native stacks.
  stack_size += 1 * MB;

  // It's not possible to request a stack smaller than the system-defined PTHREAD_STACK_MIN.
  if (stack_size < PTHREAD_STACK_MIN) {
    stack_size = PTHREAD_STACK_MIN;
  }

  if (Runtime::Current()->ExplicitStackOverflowChecks()) {
    // It's likely that callers are trying to ensure they have at least a certain amount of
    // stack space, so we should add our reserved space on top of what they requested, rather
    // than implicitly take it away from them.
    stack_size += GetStackOverflowReservedBytes(kRuntimeISA);
  } else {
    // If we are going to use implicit stack checks, allocate space for the protected
    // region at the bottom of the stack.
    stack_size += Thread::kStackOverflowImplicitCheckSize +
        GetStackOverflowReservedBytes(kRuntimeISA);
  }

  // Some systems require the stack size to be a multiple of the system page size, so round up.
  stack_size = RoundUp(stack_size, kPageSize);

  return stack_size;
}

// Return the nearest page-aligned address below the current stack top.
NO_INLINE
static uint8_t* FindStackTop() {
  return reinterpret_cast<uint8_t*>(
      AlignDown(__builtin_frame_address(0), kPageSize));
}

// Install a protected region in the stack.  This is used to trigger a SIGSEGV if a stack
// overflow is detected.  It is located right below the stack_begin_.
ATTRIBUTE_NO_SANITIZE_ADDRESS
void Thread::InstallImplicitProtection() {
  uint8_t* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  // Page containing current top of stack.
  uint8_t* stack_top = FindStackTop();

  // Try to directly protect the stack.
  VLOG(threads) << "installing stack protected region at " << std::hex <<
        static_cast<void*>(pregion) << " to " <<
        static_cast<void*>(pregion + kStackOverflowProtectedSize - 1);
  if (ProtectStack(/* fatal_on_error */ false)) {
    // Tell the kernel that we won't be needing these pages any more.
    // NB. madvise will probably write zeroes into the memory (on linux it does).
    uint32_t unwanted_size = stack_top - pregion - kPageSize;
    madvise(pregion, unwanted_size, MADV_DONTNEED);
    return;
  }

  // There is a little complexity here that deserves a special mention.  On some
  // architectures, the stack is created using a VM_GROWSDOWN flag
  // to prevent memory being allocated when it's not needed.  This flag makes the
  // kernel only allocate memory for the stack by growing down in memory.  Because we
  // want to put an mprotected region far away from that at the stack top, we need
  // to make sure the pages for the stack are mapped in before we call mprotect.
  //
  // The failed mprotect in UnprotectStack is an indication of a thread with VM_GROWSDOWN
  // with a non-mapped stack (usually only the main thread).
  //
  // We map in the stack by reading every page from the stack bottom (highest address)
  // to the stack top. (We then madvise this away.) This must be done by reading from the
  // current stack pointer downwards. Any access more than a page below the current SP
  // might cause a segv.
  // TODO: This comment may be out of date. It seems possible to speed this up. As
  //       this is normally done once in the zygote on startup, ignore for now.
  //
  // AddressSanitizer does not like the part of this functions that reads every stack page.
  // Looks a lot like an out-of-bounds access.

  // (Defensively) first remove the protection on the protected region as will want to read
  // and write it. Ignore errors.
  UnprotectStack();

  VLOG(threads) << "Need to map in stack for thread at " << std::hex <<
      static_cast<void*>(pregion);

  // Read every page from the high address to the low.
  volatile uint8_t dont_optimize_this;
  UNUSED(dont_optimize_this);
  for (uint8_t* p = stack_top; p >= pregion; p -= kPageSize) {
    dont_optimize_this = *p;
  }

  VLOG(threads) << "(again) installing stack protected region at " << std::hex <<
      static_cast<void*>(pregion) << " to " <<
      static_cast<void*>(pregion + kStackOverflowProtectedSize - 1);

  // Protect the bottom of the stack to prevent read/write to it.
  ProtectStack(/* fatal_on_error */ true);

  // Tell the kernel that we won't be needing these pages any more.
  // NB. madvise will probably write zeroes into the memory (on linux it does).
  uint32_t unwanted_size = stack_top - pregion - kPageSize;
  madvise(pregion, unwanted_size, MADV_DONTNEED);
}

void Thread::CreateNativeThread(JNIEnv* env, jobject java_peer, size_t stack_size, bool is_daemon) {
  CHECK(java_peer != nullptr);
  Thread* self = static_cast<JNIEnvExt*>(env)->self;

  if (VLOG_IS_ON(threads)) {
    ScopedObjectAccess soa(env);

    ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_name);
    ObjPtr<mirror::String> java_name =
        f->GetObject(soa.Decode<mirror::Object>(java_peer))->AsString();
    std::string thread_name;
    if (java_name != nullptr) {
      thread_name = java_name->ToModifiedUtf8();
    } else {
      thread_name = "(Unnamed)";
    }

    VLOG(threads) << "Creating native thread for " << thread_name;
    self->Dump(LOG_STREAM(INFO));
  }

  Runtime* runtime = Runtime::Current();

  // Atomically start the birth of the thread ensuring the runtime isn't shutting down.
  bool thread_start_during_shutdown = false;
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDownLocked()) {
      thread_start_during_shutdown = true;
    } else {
      runtime->StartThreadBirth();
    }
  }
  if (thread_start_during_shutdown) {
    ScopedLocalRef<jclass> error_class(env, env->FindClass("java/lang/InternalError"));
    env->ThrowNew(error_class.get(), "Thread starting during runtime shutdown");
    return;
  }

  Thread* child_thread = new Thread(is_daemon);
  // Use global JNI ref to hold peer live while child thread starts.
  child_thread->tlsPtr_.jpeer = env->NewGlobalRef(java_peer);
  stack_size = FixStackSize(stack_size);

  // Thread.start is synchronized, so we know that nativePeer is 0, and know that we're not racing to
  // assign it.
  env->SetLongField(java_peer, WellKnownClasses::java_lang_Thread_nativePeer,
                    reinterpret_cast<jlong>(child_thread));

  // Try to allocate a JNIEnvExt for the thread. We do this here as we might be out of memory and
  // do not have a good way to report this on the child's side.
  std::string error_msg;
  std::unique_ptr<JNIEnvExt> child_jni_env_ext(
      JNIEnvExt::Create(child_thread, Runtime::Current()->GetJavaVM(), &error_msg));

  int pthread_create_result = 0;
  if (child_jni_env_ext.get() != nullptr) {
    pthread_t new_pthread;
    pthread_attr_t attr;
    child_thread->tlsPtr_.tmp_jni_env = child_jni_env_ext.get();
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED),
                       "PTHREAD_CREATE_DETACHED");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
    pthread_create_result = pthread_create(&new_pthread,
                                           &attr,
                                           Thread::CreateCallback,
                                           child_thread);
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");

    if (pthread_create_result == 0) {
      // pthread_create started the new thread. The child is now responsible for managing the
      // JNIEnvExt we created.
      // Note: we can't check for tmp_jni_env == nullptr, as that would require synchronization
      //       between the threads.
      child_jni_env_ext.release();
      return;
    }
  }

  // Either JNIEnvExt::Create or pthread_create(3) failed, so clean up.
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    runtime->EndThreadBirth();
  }
  // Manually delete the global reference since Thread::Init will not have been run.
  env->DeleteGlobalRef(child_thread->tlsPtr_.jpeer);
  child_thread->tlsPtr_.jpeer = nullptr;
  delete child_thread;
  child_thread = nullptr;
  // TODO: remove from thread group?
  env->SetLongField(java_peer, WellKnownClasses::java_lang_Thread_nativePeer, 0);
  {
    std::string msg(child_jni_env_ext.get() == nullptr ?
        StringPrintf("Could not allocate JNI Env: %s", error_msg.c_str()) :
        StringPrintf("pthread_create (%s stack) failed: %s",
                                 PrettySize(stack_size).c_str(), strerror(pthread_create_result)));
    ScopedObjectAccess soa(env);
    soa.Self()->ThrowOutOfMemoryError(msg.c_str());
  }
}

bool Thread::Init(ThreadList* thread_list, JavaVMExt* java_vm, JNIEnvExt* jni_env_ext) {
  // This function does all the initialization that must be run by the native thread it applies to.
  // (When we create a new thread from managed code, we allocate the Thread* in Thread::Create so
  // we can handshake with the corresponding native thread when it's ready.) Check this native
  // thread hasn't been through here already...
  CHECK(Thread::Current() == nullptr);

  // Set pthread_self_ ahead of pthread_setspecific, that makes Thread::Current function, this
  // avoids pthread_self_ ever being invalid when discovered from Thread::Current().
  tlsPtr_.pthread_self = pthread_self();
  CHECK(is_started_);

  SetUpAlternateSignalStack();
  if (!InitStackHwm()) {
    return false;
  }
  InitCpu();
  InitTlsEntryPoints();
  RemoveSuspendTrigger();
  InitCardTable();
  InitTid();
  interpreter::InitInterpreterTls(this);

#ifdef ART_TARGET_ANDROID
  __get_tls()[TLS_SLOT_ART_THREAD_SELF] = this;
#else
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach self");
#endif
  DCHECK_EQ(Thread::Current(), this);

  tls32_.thin_lock_thread_id = thread_list->AllocThreadId(this);

  if (jni_env_ext != nullptr) {
    DCHECK_EQ(jni_env_ext->vm, java_vm);
    DCHECK_EQ(jni_env_ext->self, this);
    tlsPtr_.jni_env = jni_env_ext;
  } else {
    std::string error_msg;
    tlsPtr_.jni_env = JNIEnvExt::Create(this, java_vm, &error_msg);
    if (tlsPtr_.jni_env == nullptr) {
      LOG(ERROR) << "Failed to create JNIEnvExt: " << error_msg;
      return false;
    }
  }

  thread_list->Register(this);
  return true;
}

template <typename PeerAction>
Thread* Thread::Attach(const char* thread_name, bool as_daemon, PeerAction peer_action) {
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    LOG(ERROR) << "Thread attaching to non-existent runtime: " << thread_name;
    return nullptr;
  }
  Thread* self;
  {
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDownLocked()) {
      LOG(WARNING) << "Thread attaching while runtime is shutting down: " << thread_name;
      return nullptr;
    } else {
      Runtime::Current()->StartThreadBirth();
      self = new Thread(as_daemon);
      bool init_success = self->Init(runtime->GetThreadList(), runtime->GetJavaVM());
      Runtime::Current()->EndThreadBirth();
      if (!init_success) {
        delete self;
        return nullptr;
      }
    }
  }

  self->InitStringEntryPoints();

  CHECK_NE(self->GetState(), kRunnable);
  self->SetState(kNative);

  // Run the action that is acting on the peer.
  if (!peer_action(self)) {
    runtime->GetThreadList()->Unregister(self);
    // Unregister deletes self, no need to do this here.
    return nullptr;
  }

  if (VLOG_IS_ON(threads)) {
    if (thread_name != nullptr) {
      VLOG(threads) << "Attaching thread " << thread_name;
    } else {
      VLOG(threads) << "Attaching unnamed thread.";
    }
    ScopedObjectAccess soa(self);
    self->Dump(LOG_STREAM(INFO));
  }

  {
    ScopedObjectAccess soa(self);
    runtime->GetRuntimeCallbacks()->ThreadStart(self);
  }

  return self;
}

Thread* Thread::Attach(const char* thread_name,
                       bool as_daemon,
                       jobject thread_group,
                       bool create_peer) {
  auto create_peer_action = [&](Thread* self) {
    // If we're the main thread, ClassLinker won't be created until after we're attached,
    // so that thread needs a two-stage attach. Regular threads don't need this hack.
    // In the compiler, all threads need this hack, because no-one's going to be getting
    // a native peer!
    if (create_peer) {
      self->CreatePeer(thread_name, as_daemon, thread_group);
      if (self->IsExceptionPending()) {
        // We cannot keep the exception around, as we're deleting self. Try to be helpful and log it.
        {
          ScopedObjectAccess soa(self);
          LOG(ERROR) << "Exception creating thread peer:";
          LOG(ERROR) << self->GetException()->Dump();
          self->ClearException();
        }
        return false;
      }
    } else {
      // These aren't necessary, but they improve diagnostics for unit tests & command-line tools.
      if (thread_name != nullptr) {
        self->tlsPtr_.name->assign(thread_name);
        ::art::SetThreadName(thread_name);
      } else if (self->GetJniEnv()->check_jni) {
        LOG(WARNING) << *Thread::Current() << " attached without supplying a name";
      }
    }
    return true;
  };
  return Attach(thread_name, as_daemon, create_peer_action);
}

Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_peer) {
  auto set_peer_action = [&](Thread* self) {
    // Install the given peer.
    {
      DCHECK(self == Thread::Current());
      ScopedObjectAccess soa(self);
      self->tlsPtr_.opeer = soa.Decode<mirror::Object>(thread_peer).Ptr();
    }
    self->GetJniEnv()->SetLongField(thread_peer,
                                    WellKnownClasses::java_lang_Thread_nativePeer,
                                    reinterpret_cast<jlong>(self));
    return true;
  };
  return Attach(thread_name, as_daemon, set_peer_action);
}

void Thread::CreatePeer(const char* name, bool as_daemon, jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());
  JNIEnv* env = tlsPtr_.jni_env;

  if (thread_group == nullptr) {
    thread_group = runtime->GetMainThreadGroup();
  }
  ScopedLocalRef<jobject> thread_name(env, env->NewStringUTF(name));
  // Add missing null check in case of OOM b/18297817
  if (name != nullptr && thread_name.get() == nullptr) {
    CHECK(IsExceptionPending());
    return;
  }
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  ScopedLocalRef<jobject> peer(env, env->AllocObject(WellKnownClasses::java_lang_Thread));
  if (peer.get() == nullptr) {
    CHECK(IsExceptionPending());
    return;
  }
  {
    ScopedObjectAccess soa(this);
    tlsPtr_.opeer = soa.Decode<mirror::Object>(peer.get()).Ptr();
  }
  env->CallNonvirtualVoidMethod(peer.get(),
                                WellKnownClasses::java_lang_Thread,
                                WellKnownClasses::java_lang_Thread_init,
                                thread_group, thread_name.get(), thread_priority, thread_is_daemon);
  if (IsExceptionPending()) {
    return;
  }

  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  env->SetLongField(peer.get(), WellKnownClasses::java_lang_Thread_nativePeer,
                    reinterpret_cast<jlong>(self));

  ScopedObjectAccess soa(self);
  StackHandleScope<1> hs(self);
  MutableHandle<mirror::String> peer_thread_name(hs.NewHandle(GetThreadName()));
  if (peer_thread_name == nullptr) {
    // The Thread constructor should have set the Thread.name to a
    // non-null value. However, because we can run without code
    // available (in the compiler, in tests), we manually assign the
    // fields the constructor should have set.
    if (runtime->IsActiveTransaction()) {
      InitPeer<true>(soa,
                     tlsPtr_.opeer,
                     thread_is_daemon,
                     thread_group,
                     thread_name.get(),
                     thread_priority);
    } else {
      InitPeer<false>(soa,
                      tlsPtr_.opeer,
                      thread_is_daemon,
                      thread_group,
                      thread_name.get(),
                      thread_priority);
    }
    peer_thread_name.Assign(GetThreadName());
  }
  // 'thread_name' may have been null, so don't trust 'peer_thread_name' to be non-null.
  if (peer_thread_name != nullptr) {
    SetThreadName(peer_thread_name->ToModifiedUtf8().c_str());
  }
}

jobject Thread::CreateCompileTimePeer(JNIEnv* env,
                                      const char* name,
                                      bool as_daemon,
                                      jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(!runtime->IsStarted());

  if (thread_group == nullptr) {
    thread_group = runtime->GetMainThreadGroup();
  }
  ScopedLocalRef<jobject> thread_name(env, env->NewStringUTF(name));
  // Add missing null check in case of OOM b/18297817
  if (name != nullptr && thread_name.get() == nullptr) {
    CHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  ScopedLocalRef<jobject> peer(env, env->AllocObject(WellKnownClasses::java_lang_Thread));
  if (peer.get() == nullptr) {
    CHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }

  // We cannot call Thread.init, as it will recursively ask for currentThread.

  // The Thread constructor should have set the Thread.name to a
  // non-null value. However, because we can run without code
  // available (in the compiler, in tests), we manually assign the
  // fields the constructor should have set.
  ScopedObjectAccessUnchecked soa(Thread::Current());
  if (runtime->IsActiveTransaction()) {
    InitPeer<true>(soa,
                   soa.Decode<mirror::Object>(peer.get()),
                   thread_is_daemon,
                   thread_group,
                   thread_name.get(),
                   thread_priority);
  } else {
    InitPeer<false>(soa,
                    soa.Decode<mirror::Object>(peer.get()),
                    thread_is_daemon,
                    thread_group,
                    thread_name.get(),
                    thread_priority);
  }

  return peer.release();
}

template<bool kTransactionActive>
void Thread::InitPeer(ScopedObjectAccessAlreadyRunnable& soa,
                      ObjPtr<mirror::Object> peer,
                      jboolean thread_is_daemon,
                      jobject thread_group,
                      jobject thread_name,
                      jint thread_priority) {
  jni::DecodeArtField(WellKnownClasses::java_lang_Thread_daemon)->
      SetBoolean<kTransactionActive>(peer, thread_is_daemon);
  jni::DecodeArtField(WellKnownClasses::java_lang_Thread_group)->
      SetObject<kTransactionActive>(peer, soa.Decode<mirror::Object>(thread_group));
  jni::DecodeArtField(WellKnownClasses::java_lang_Thread_name)->
      SetObject<kTransactionActive>(peer, soa.Decode<mirror::Object>(thread_name));
  jni::DecodeArtField(WellKnownClasses::java_lang_Thread_priority)->
      SetInt<kTransactionActive>(peer, thread_priority);
}

void Thread::SetThreadName(const char* name) {
  tlsPtr_.name->assign(name);
  ::art::SetThreadName(name);
  Dbg::DdmSendThreadNotification(this, CHUNK_TYPE("THNM"));
}

static void GetThreadStack(pthread_t thread,
                           void** stack_base,
                           size_t* stack_size,
                           size_t* guard_size) {
#if defined(__APPLE__)
  *stack_size = pthread_get_stacksize_np(thread);
  void* stack_addr = pthread_get_stackaddr_np(thread);

  // Check whether stack_addr is the base or end of the stack.
  // (On Mac OS 10.7, it's the end.)
  int stack_variable;
  if (stack_addr > &stack_variable) {
    *stack_base = reinterpret_cast<uint8_t*>(stack_addr) - *stack_size;
  } else {
    *stack_base = stack_addr;
  }

  // This is wrong, but there doesn't seem to be a way to get the actual value on the Mac.
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
#else
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (thread, &attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, stack_base, stack_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);

#if defined(__GLIBC__)
  // If we're the main thread, check whether we were run with an unlimited stack. In that case,
  // glibc will have reported a 2GB stack for our 32-bit process, and our stack overflow detection
  // will be broken because we'll die long before we get close to 2GB.
  bool is_main_thread = (::art::GetTid() == getpid());
  if (is_main_thread) {
    rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == -1) {
      PLOG(FATAL) << "getrlimit(RLIMIT_STACK) failed";
    }
    if (stack_limit.rlim_cur == RLIM_INFINITY) {
      size_t old_stack_size = *stack_size;

      // Use the kernel default limit as our size, and adjust the base to match.
      *stack_size = 8 * MB;
      *stack_base = reinterpret_cast<uint8_t*>(*stack_base) + (old_stack_size - *stack_size);

      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(*stack_size)
                    << " with base " << *stack_base;
    }
  }
#endif

#endif
}

bool Thread::InitStackHwm() {
  void* read_stack_base;
  size_t read_stack_size;
  size_t read_guard_size;
  GetThreadStack(tlsPtr_.pthread_self, &read_stack_base, &read_stack_size, &read_guard_size);

  tlsPtr_.stack_begin = reinterpret_cast<uint8_t*>(read_stack_base);
  tlsPtr_.stack_size = read_stack_size;

  // The minimum stack size we can cope with is the overflow reserved bytes (typically
  // 8K) + the protected region size (4K) + another page (4K).  Typically this will
  // be 8+4+4 = 16K.  The thread won't be able to do much with this stack even the GC takes
  // between 8K and 12K.
  uint32_t min_stack = GetStackOverflowReservedBytes(kRuntimeISA) + kStackOverflowProtectedSize
    + 4 * KB;
  if (read_stack_size <= min_stack) {
    // Note, as we know the stack is small, avoid operations that could use a lot of stack.
    LogHelper::LogLineLowStack(__PRETTY_FUNCTION__,
                               __LINE__,
                               ::android::base::ERROR,
                               "Attempt to attach a thread with a too-small stack");
    return false;
  }

  // This is included in the SIGQUIT output, but it's useful here for thread debugging.
  VLOG(threads) << StringPrintf("Native stack is at %p (%s with %s guard)",
                                read_stack_base,
                                PrettySize(read_stack_size).c_str(),
                                PrettySize(read_guard_size).c_str());

  // Set stack_end_ to the bottom of the stack saving space of stack overflows

  Runtime* runtime = Runtime::Current();
  bool implicit_stack_check = !runtime->ExplicitStackOverflowChecks() && !runtime->IsAotCompiler();

  // Valgrind on arm doesn't give the right values here. Do not install the guard page, and
  // effectively disable stack overflow checks (we'll get segfaults, potentially) by setting
  // stack_begin to 0.
  const bool valgrind_on_arm =
      (kRuntimeISA == kArm || kRuntimeISA == kArm64) &&
      kMemoryToolIsValgrind &&
      RUNNING_ON_MEMORY_TOOL != 0;
  if (valgrind_on_arm) {
    tlsPtr_.stack_begin = nullptr;
  }

  ResetDefaultStackEnd();

  // Install the protected region if we are doing implicit overflow checks.
  if (implicit_stack_check && !valgrind_on_arm) {
    // The thread might have protected region at the bottom.  We need
    // to install our own region so we need to move the limits
    // of the stack to make room for it.

    tlsPtr_.stack_begin += read_guard_size + kStackOverflowProtectedSize;
    tlsPtr_.stack_end += read_guard_size + kStackOverflowProtectedSize;
    tlsPtr_.stack_size -= read_guard_size;

    InstallImplicitProtection();
  }

  // Sanity check.
  CHECK_GT(FindStackTop(), reinterpret_cast<void*>(tlsPtr_.stack_end));

  return true;
}

void Thread::ShortDump(std::ostream& os) const {
  os << "Thread[";
  if (GetThreadId() != 0) {
    // If we're in kStarting, we won't have a thin lock id or tid yet.
    os << GetThreadId()
       << ",tid=" << GetTid() << ',';
  }
  os << GetState()
     << ",Thread*=" << this
     << ",peer=" << tlsPtr_.opeer
     << ",\"" << (tlsPtr_.name != nullptr ? *tlsPtr_.name : "null") << "\""
     << "]";
}

void Thread::Dump(std::ostream& os, bool dump_native_stack, BacktraceMap* backtrace_map,
                  bool force_dump_stack) const {
  DumpState(os);
  DumpStack(os, dump_native_stack, backtrace_map, force_dump_stack);
}

mirror::String* Thread::GetThreadName() const {
  ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_name);
  if (tlsPtr_.opeer == nullptr) {
    return nullptr;
  }
  ObjPtr<mirror::Object> name = f->GetObject(tlsPtr_.opeer);
  return name == nullptr ? nullptr : name->AsString();
}

void Thread::GetThreadName(std::string& name) const {
  name.assign(*tlsPtr_.name);
}

uint64_t Thread::GetCpuMicroTime() const {
#if defined(__linux__)
  clockid_t cpu_clock_id;
  pthread_getcpuclockid(tlsPtr_.pthread_self, &cpu_clock_id);
  timespec now;
  clock_gettime(cpu_clock_id, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000) + now.tv_nsec / UINT64_C(1000);
#else  // __APPLE__
  UNIMPLEMENTED(WARNING);
  return -1;
#endif
}

// Attempt to rectify locks so that we dump thread list with required locks before exiting.
static void UnsafeLogFatalForSuspendCount(Thread* self, Thread* thread) NO_THREAD_SAFETY_ANALYSIS {
  LOG(ERROR) << *thread << " suspend count already zero.";
  Locks::thread_suspend_count_lock_->Unlock(self);
  if (!Locks::mutator_lock_->IsSharedHeld(self)) {
    Locks::mutator_lock_->SharedTryLock(self);
    if (!Locks::mutator_lock_->IsSharedHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding mutator_lock_";
    }
  }
  if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
    Locks::thread_list_lock_->TryLock(self);
    if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding thread_list_lock_";
    }
  }
  std::ostringstream ss;
  Runtime::Current()->GetThreadList()->Dump(ss);
  LOG(FATAL) << ss.str();
}

bool Thread::ModifySuspendCountInternal(Thread* self,
                                        int delta,
                                        AtomicInteger* suspend_barrier,
                                        bool for_debugger) {
  if (kIsDebugBuild) {
    DCHECK(delta == -1 || delta == +1 || delta == -tls32_.debug_suspend_count)
          << delta << " " << tls32_.debug_suspend_count << " " << this;
    DCHECK_GE(tls32_.suspend_count, tls32_.debug_suspend_count) << this;
    Locks::thread_suspend_count_lock_->AssertHeld(self);
    if (this != self && !IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(self);
    }
  }
  if (UNLIKELY(delta < 0 && tls32_.suspend_count <= 0)) {
    UnsafeLogFatalForSuspendCount(self, this);
    return false;
  }

  if (kUseReadBarrier && delta > 0 && this != self && tlsPtr_.flip_function != nullptr) {
    // Force retry of a suspend request if it's in the middle of a thread flip to avoid a
    // deadlock. b/31683379.
    return false;
  }

  uint16_t flags = kSuspendRequest;
  if (delta > 0 && suspend_barrier != nullptr) {
    uint32_t available_barrier = kMaxSuspendBarriers;
    for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
      if (tlsPtr_.active_suspend_barriers[i] == nullptr) {
        available_barrier = i;
        break;
      }
    }
    if (available_barrier == kMaxSuspendBarriers) {
      // No barrier spaces available, we can't add another.
      return false;
    }
    tlsPtr_.active_suspend_barriers[available_barrier] = suspend_barrier;
    flags |= kActiveSuspendBarrier;
  }

  tls32_.suspend_count += delta;
  if (for_debugger) {
    tls32_.debug_suspend_count += delta;
  }

  if (tls32_.suspend_count == 0) {
    AtomicClearFlag(kSuspendRequest);
  } else {
    // Two bits might be set simultaneously.
    tls32_.state_and_flags.as_atomic_int.FetchAndOrSequentiallyConsistent(flags);
    TriggerSuspend();
  }
  return true;
}

bool Thread::PassActiveSuspendBarriers(Thread* self) {
  // Grab the suspend_count lock and copy the current set of
  // barriers. Then clear the list and the flag. The ModifySuspendCount
  // function requires the lock so we prevent a race between setting
  // the kActiveSuspendBarrier flag and clearing it.
  AtomicInteger* pass_barriers[kMaxSuspendBarriers];
  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    if (!ReadFlag(kActiveSuspendBarrier)) {
      // quick exit test: the barriers have already been claimed - this is
      // possible as there may be a race to claim and it doesn't matter
      // who wins.
      // All of the callers of this function (except the SuspendAllInternal)
      // will first test the kActiveSuspendBarrier flag without lock. Here
      // double-check whether the barrier has been passed with the
      // suspend_count lock.
      return false;
    }

    for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
      pass_barriers[i] = tlsPtr_.active_suspend_barriers[i];
      tlsPtr_.active_suspend_barriers[i] = nullptr;
    }
    AtomicClearFlag(kActiveSuspendBarrier);
  }

  uint32_t barrier_count = 0;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; i++) {
    AtomicInteger* pending_threads = pass_barriers[i];
    if (pending_threads != nullptr) {
      bool done = false;
      do {
        int32_t cur_val = pending_threads->LoadRelaxed();
        CHECK_GT(cur_val, 0) << "Unexpected value for PassActiveSuspendBarriers(): " << cur_val;
        // Reduce value by 1.
        done = pending_threads->CompareExchangeWeakRelaxed(cur_val, cur_val - 1);
#if ART_USE_FUTEXES
        if (done && (cur_val - 1) == 0) {  // Weak CAS may fail spuriously.
          futex(pending_threads->Address(), FUTEX_WAKE, -1, nullptr, nullptr, 0);
        }
#endif
      } while (!done);
      ++barrier_count;
    }
  }
  CHECK_GT(barrier_count, 0U);
  return true;
}

void Thread::ClearSuspendBarrier(AtomicInteger* target) {
  CHECK(ReadFlag(kActiveSuspendBarrier));
  bool clear_flag = true;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
    AtomicInteger* ptr = tlsPtr_.active_suspend_barriers[i];
    if (ptr == target) {
      tlsPtr_.active_suspend_barriers[i] = nullptr;
    } else if (ptr != nullptr) {
      clear_flag = false;
    }
  }
  if (LIKELY(clear_flag)) {
    AtomicClearFlag(kActiveSuspendBarrier);
  }
}

void Thread::RunCheckpointFunction() {
  bool done = false;
  do {
    // Grab the suspend_count lock and copy the checkpoints one by one. When the last checkpoint is
    // copied, clear the list and the flag. The RequestCheckpoint function will also grab this lock
    // to prevent a race between setting the kCheckpointRequest flag and clearing it.
    Closure* checkpoint = nullptr;
    {
      MutexLock mu(this, *Locks::thread_suspend_count_lock_);
      if (tlsPtr_.checkpoint_function != nullptr) {
        checkpoint = tlsPtr_.checkpoint_function;
        if (!checkpoint_overflow_.empty()) {
          // Overflow list not empty, copy the first one out and continue.
          tlsPtr_.checkpoint_function = checkpoint_overflow_.front();
          checkpoint_overflow_.pop_front();
        } else {
          // No overflow checkpoints, this means that we are on the last pending checkpoint.
          tlsPtr_.checkpoint_function = nullptr;
          AtomicClearFlag(kCheckpointRequest);
          done = true;
        }
      } else {
        LOG(FATAL) << "Checkpoint flag set without pending checkpoint";
      }
    }

    // Outside the lock, run the checkpoint functions that we collected.
    ScopedTrace trace("Run checkpoint function");
    DCHECK(checkpoint != nullptr);
    checkpoint->Run(this);
  } while (!done);
}

void Thread::RunEmptyCheckpoint() {
  DCHECK_EQ(Thread::Current(), this);
  AtomicClearFlag(kEmptyCheckpointRequest);
  Runtime::Current()->GetThreadList()->EmptyCheckpointBarrier()->Pass(this);
}

bool Thread::RequestCheckpoint(Closure* function) {
  union StateAndFlags old_state_and_flags;
  old_state_and_flags.as_int = tls32_.state_and_flags.as_int;
  if (old_state_and_flags.as_struct.state != kRunnable) {
    return false;  // Fail, thread is suspended and so can't run a checkpoint.
  }

  // We must be runnable to request a checkpoint.
  DCHECK_EQ(old_state_and_flags.as_struct.state, kRunnable);
  union StateAndFlags new_state_and_flags;
  new_state_and_flags.as_int = old_state_and_flags.as_int;
  new_state_and_flags.as_struct.flags |= kCheckpointRequest;
  bool success = tls32_.state_and_flags.as_atomic_int.CompareExchangeStrongSequentiallyConsistent(
      old_state_and_flags.as_int, new_state_and_flags.as_int);
  if (success) {
    // Succeeded setting checkpoint flag, now insert the actual checkpoint.
    if (tlsPtr_.checkpoint_function == nullptr) {
      tlsPtr_.checkpoint_function = function;
    } else {
      checkpoint_overflow_.push_back(function);
    }
    CHECK_EQ(ReadFlag(kCheckpointRequest), true);
    TriggerSuspend();
  }
  return success;
}

bool Thread::RequestEmptyCheckpoint() {
  union StateAndFlags old_state_and_flags;
  old_state_and_flags.as_int = tls32_.state_and_flags.as_int;
  if (old_state_and_flags.as_struct.state != kRunnable) {
    // If it's not runnable, we don't need to do anything because it won't be in the middle of a
    // heap access (eg. the read barrier).
    return false;
  }

  // We must be runnable to request a checkpoint.
  DCHECK_EQ(old_state_and_flags.as_struct.state, kRunnable);
  union StateAndFlags new_state_and_flags;
  new_state_and_flags.as_int = old_state_and_flags.as_int;
  new_state_and_flags.as_struct.flags |= kEmptyCheckpointRequest;
  bool success = tls32_.state_and_flags.as_atomic_int.CompareExchangeStrongSequentiallyConsistent(
      old_state_and_flags.as_int, new_state_and_flags.as_int);
  if (success) {
    TriggerSuspend();
  }
  return success;
}

class BarrierClosure : public Closure {
 public:
  explicit BarrierClosure(Closure* wrapped) : wrapped_(wrapped), barrier_(0) {}

  void Run(Thread* self) OVERRIDE {
    wrapped_->Run(self);
    barrier_.Pass(self);
  }

  void Wait(Thread* self) {
    barrier_.Increment(self, 1);
  }

 private:
  Closure* wrapped_;
  Barrier barrier_;
};

void Thread::RequestSynchronousCheckpoint(Closure* function) {
  if (this == Thread::Current()) {
    // Asked to run on this thread. Just run.
    function->Run(this);
    return;
  }
  Thread* self = Thread::Current();

  // The current thread is not this thread.

  for (;;) {
    // If this thread is runnable, try to schedule a checkpoint. Do some gymnastics to not hold the
    // suspend-count lock for too long.
    if (GetState() == ThreadState::kRunnable) {
      BarrierClosure barrier_closure(function);
      bool installed = false;
      {
        MutexLock mu(self, *Locks::thread_suspend_count_lock_);
        installed = RequestCheckpoint(&barrier_closure);
      }
      if (installed) {
        barrier_closure.Wait(self);
        return;
      }
      // Fall-through.
    }

    // This thread is not runnable, make sure we stay suspended, then run the checkpoint.
    // Note: ModifySuspendCountInternal also expects the thread_list_lock to be held in
    //       certain situations.
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);

      if (!ModifySuspendCount(self, +1, nullptr, false)) {
        // Just retry the loop.
        sched_yield();
        continue;
      }
    }

    while (GetState() == ThreadState::kRunnable) {
      // We became runnable again. Wait till the suspend triggered in ModifySuspendCount
      // moves us to suspended.
      sched_yield();
    }

    function->Run(this);

    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);

      DCHECK_NE(GetState(), ThreadState::kRunnable);
      bool updated = ModifySuspendCount(self, -1, nullptr, false);
      DCHECK(updated);
    }

    return;  // We're done, break out of the loop.
  }
}

Closure* Thread::GetFlipFunction() {
  Atomic<Closure*>* atomic_func = reinterpret_cast<Atomic<Closure*>*>(&tlsPtr_.flip_function);
  Closure* func;
  do {
    func = atomic_func->LoadRelaxed();
    if (func == nullptr) {
      return nullptr;
    }
  } while (!atomic_func->CompareExchangeWeakSequentiallyConsistent(func, nullptr));
  DCHECK(func != nullptr);
  return func;
}

void Thread::SetFlipFunction(Closure* function) {
  CHECK(function != nullptr);
  Atomic<Closure*>* atomic_func = reinterpret_cast<Atomic<Closure*>*>(&tlsPtr_.flip_function);
  atomic_func->StoreSequentiallyConsistent(function);
}

void Thread::FullSuspendCheck() {
  ScopedTrace trace(__FUNCTION__);
  VLOG(threads) << this << " self-suspending";
  // Make thread appear suspended to other threads, release mutator_lock_.
  // Transition to suspended and back to runnable, re-acquire share on mutator_lock_.
  ScopedThreadSuspension(this, kSuspended);
  VLOG(threads) << this << " self-reviving";
}

static std::string GetSchedulerGroupName(pid_t tid) {
  // /proc/<pid>/cgroup looks like this:
  // 2:devices:/
  // 1:cpuacct,cpu:/
  // We want the third field from the line whose second field contains the "cpu" token.
  std::string cgroup_file;
  if (!ReadFileToString(StringPrintf("/proc/self/task/%d/cgroup", tid), &cgroup_file)) {
    return "";
  }
  std::vector<std::string> cgroup_lines;
  Split(cgroup_file, '\n', &cgroup_lines);
  for (size_t i = 0; i < cgroup_lines.size(); ++i) {
    std::vector<std::string> cgroup_fields;
    Split(cgroup_lines[i], ':', &cgroup_fields);
    std::vector<std::string> cgroups;
    Split(cgroup_fields[1], ',', &cgroups);
    for (size_t j = 0; j < cgroups.size(); ++j) {
      if (cgroups[j] == "cpu") {
        return cgroup_fields[2].substr(1);  // Skip the leading slash.
      }
    }
  }
  return "";
}


void Thread::DumpState(std::ostream& os, const Thread* thread, pid_t tid) {
  std::string group_name;
  int priority;
  bool is_daemon = false;
  Thread* self = Thread::Current();

  // If flip_function is not null, it means we have run a checkpoint
  // before the thread wakes up to execute the flip function and the
  // thread roots haven't been forwarded.  So the following access to
  // the roots (opeer or methods in the frames) would be bad. Run it
  // here. TODO: clean up.
  if (thread != nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    Thread* this_thread = const_cast<Thread*>(thread);
    Closure* flip_func = this_thread->GetFlipFunction();
    if (flip_func != nullptr) {
      flip_func->Run(this_thread);
    }
  }

  // Don't do this if we are aborting since the GC may have all the threads suspended. This will
  // cause ScopedObjectAccessUnchecked to deadlock.
  if (gAborting == 0 && self != nullptr && thread != nullptr && thread->tlsPtr_.opeer != nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    priority = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_priority)
        ->GetInt(thread->tlsPtr_.opeer);
    is_daemon = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_daemon)
        ->GetBoolean(thread->tlsPtr_.opeer);

    ObjPtr<mirror::Object> thread_group =
        jni::DecodeArtField(WellKnownClasses::java_lang_Thread_group)
            ->GetObject(thread->tlsPtr_.opeer);

    if (thread_group != nullptr) {
      ArtField* group_name_field =
          jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_name);
      ObjPtr<mirror::String> group_name_string =
          group_name_field->GetObject(thread_group)->AsString();
      group_name = (group_name_string != nullptr) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    priority = GetNativePriority();
  }

  std::string scheduler_group_name(GetSchedulerGroupName(tid));
  if (scheduler_group_name.empty()) {
    scheduler_group_name = "default";
  }

  if (thread != nullptr) {
    os << '"' << *thread->tlsPtr_.name << '"';
    if (is_daemon) {
      os << " daemon";
    }
    os << " prio=" << priority
       << " tid=" << thread->GetThreadId()
       << " " << thread->GetState();
    if (thread->IsStillStarting()) {
      os << " (still starting up)";
    }
    os << "\n";
  } else {
    os << '"' << ::art::GetThreadName(tid) << '"'
       << " prio=" << priority
       << " (not attached)\n";
  }

  if (thread != nullptr) {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    os << "  | group=\"" << group_name << "\""
       << " sCount=" << thread->tls32_.suspend_count
       << " dsCount=" << thread->tls32_.debug_suspend_count
       << " flags=" << thread->tls32_.state_and_flags.as_struct.flags
       << " obj=" << reinterpret_cast<void*>(thread->tlsPtr_.opeer)
       << " self=" << reinterpret_cast<const void*>(thread) << "\n";
  }

  os << "  | sysTid=" << tid
     << " nice=" << getpriority(PRIO_PROCESS, tid)
     << " cgrp=" << scheduler_group_name;
  if (thread != nullptr) {
    int policy;
    sched_param sp;
#if !defined(__APPLE__)
    // b/36445592 Don't use pthread_getschedparam since pthread may have exited.
    policy = sched_getscheduler(tid);
    if (policy == -1) {
      PLOG(WARNING) << "sched_getscheduler(" << tid << ")";
    }
    int sched_getparam_result = sched_getparam(tid, &sp);
    if (sched_getparam_result == -1) {
      PLOG(WARNING) << "sched_getparam(" << tid << ", &sp)";
      sp.sched_priority = -1;
    }
#else
    CHECK_PTHREAD_CALL(pthread_getschedparam, (thread->tlsPtr_.pthread_self, &policy, &sp),
                       __FUNCTION__);
#endif
    os << " sched=" << policy << "/" << sp.sched_priority
       << " handle=" << reinterpret_cast<void*>(thread->tlsPtr_.pthread_self);
  }
  os << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", tid), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1);  // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  char native_thread_state = '?';
  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  GetTaskStats(tid, &native_thread_state, &utime, &stime, &task_cpu);

  os << "  | state=" << native_thread_state
     << " schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
  if (thread != nullptr) {
    os << "  | stack=" << reinterpret_cast<void*>(thread->tlsPtr_.stack_begin) << "-"
        << reinterpret_cast<void*>(thread->tlsPtr_.stack_end) << " stackSize="
        << PrettySize(thread->tlsPtr_.stack_size) << "\n";
    // Dump the held mutexes.
    os << "  | held mutexes=";
    for (size_t i = 0; i < kLockLevelCount; ++i) {
      if (i != kMonitorLock) {
        BaseMutex* mutex = thread->GetHeldMutex(static_cast<LockLevel>(i));
        if (mutex != nullptr) {
          os << " \"" << mutex->GetName() << "\"";
          if (mutex->IsReaderWriterMutex()) {
            ReaderWriterMutex* rw_mutex = down_cast<ReaderWriterMutex*>(mutex);
            if (rw_mutex->GetExclusiveOwnerTid() == static_cast<uint64_t>(tid)) {
              os << "(exclusive held)";
            } else {
              os << "(shared held)";
            }
          }
        }
      }
    }
    os << "\n";
  }
}

void Thread::DumpState(std::ostream& os) const {
  Thread::DumpState(os, this, GetTid());
}

struct StackDumpVisitor : public StackVisitor {
  StackDumpVisitor(std::ostream& os_in,
                   Thread* thread_in,
                   Context* context,
                   bool can_allocate_in,
                   bool check_suspended = true,
                   bool dump_locks_in = true)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread_in,
                     context,
                     StackVisitor::StackWalkKind::kIncludeInlinedFrames,
                     check_suspended),
        os(os_in),
        can_allocate(can_allocate_in),
        last_method(nullptr),
        last_line_number(0),
        repetition_count(0),
        frame_count(0),
        dump_locks(dump_locks_in) {}

  virtual ~StackDumpVisitor() {
    if (frame_count == 0) {
      os << "  (no managed stack frames)\n";
    }
  }

  bool VisitFrame() REQUIRES_SHARED(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;
    }
    m = m->GetInterfaceMethodIfProxy(kRuntimePointerSize);
    const int kMaxRepetition = 3;
    ObjPtr<mirror::Class> c = m->GetDeclaringClass();
    ObjPtr<mirror::DexCache> dex_cache = c->GetDexCache();
    int line_number = -1;
    if (dex_cache != nullptr) {  // be tolerant of bad input
      const DexFile* dex_file = dex_cache->GetDexFile();
      line_number = annotations::GetLineNumFromPC(dex_file, m, GetDexPc(false));
    }
    if (line_number == last_line_number && last_method == m) {
      ++repetition_count;
    } else {
      if (repetition_count >= kMaxRepetition) {
        os << "  ... repeated " << (repetition_count - kMaxRepetition) << " times\n";
      }
      repetition_count = 0;
      last_line_number = line_number;
      last_method = m;
    }
    if (repetition_count < kMaxRepetition) {
      os << "  at " << m->PrettyMethod(false);
      if (m->IsNative()) {
        os << "(Native method)";
      } else {
        const char* source_file(m->GetDeclaringClassSourceFile());
        os << "(" << (source_file != nullptr ? source_file : "unavailable")
           << ":" << line_number << ")";
      }
      os << "\n";
      if (frame_count == 0) {
        Monitor::DescribeWait(os, GetThread());
      }
      if (can_allocate && dump_locks) {
        // Visit locks, but do not abort on errors. This would trigger a nested abort.
        // Skip visiting locks if dump_locks is false as it would cause a bad_mutexes_held in
        // RegTypeCache::RegTypeCache due to thread_list_lock.
        Monitor::VisitLocks(this, DumpLockedObject, &os, false);
      }
    }

    ++frame_count;
    return true;
  }

  static void DumpLockedObject(mirror::Object* o, void* context)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    std::ostream& os = *reinterpret_cast<std::ostream*>(context);
    os << "  - locked ";
    if (o == nullptr) {
      os << "an unknown object";
    } else {
      if (kUseReadBarrier && Thread::Current()->GetIsGcMarking()) {
        // We may call Thread::Dump() in the middle of the CC thread flip and this thread's stack
        // may have not been flipped yet and "o" may be a from-space (stale) ref, in which case the
        // IdentityHashCode call below will crash. So explicitly mark/forward it here.
        o = ReadBarrier::Mark(o);
      }
      if ((o->GetLockWord(false).GetState() == LockWord::kThinLocked) &&
          Locks::mutator_lock_->IsExclusiveHeld(Thread::Current())) {
        // Getting the identity hashcode here would result in lock inflation and suspension of the
        // current thread, which isn't safe if this is the only runnable thread.
        os << StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)", reinterpret_cast<intptr_t>(o),
                           o->PrettyTypeOf().c_str());
      } else {
        // IdentityHashCode can cause thread suspension, which would invalidate o if it moved. So
        // we get the pretty type beofre we call IdentityHashCode.
        const std::string pretty_type(o->PrettyTypeOf());
        os << StringPrintf("<0x%08x> (a %s)", o->IdentityHashCode(), pretty_type.c_str());
      }
    }
    os << "\n";
  }

  std::ostream& os;
  const bool can_allocate;
  ArtMethod* last_method;
  int last_line_number;
  int repetition_count;
  int frame_count;
  const bool dump_locks;
};

static bool ShouldShowNativeStack(const Thread* thread)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ThreadState state = thread->GetState();

  // In native code somewhere in the VM (one of the kWaitingFor* states)? That's interesting.
  if (state > kWaiting && state < kStarting) {
    return true;
  }

  // In an Object.wait variant or Thread.sleep? That's not interesting.
  if (state == kTimedWaiting || state == kSleeping || state == kWaiting) {
    return false;
  }

  // Threads with no managed stack frames should be shown.
  const ManagedStack* managed_stack = thread->GetManagedStack();
  if (managed_stack == nullptr || (managed_stack->GetTopQuickFrame() == nullptr &&
      managed_stack->GetTopShadowFrame() == nullptr)) {
    return true;
  }

  // In some other native method? That's interesting.
  // We don't just check kNative because native methods will be in state kSuspended if they're
  // calling back into the VM, or kBlocked if they're blocked on a monitor, or one of the
  // thread-startup states if it's early enough in their life cycle (http://b/7432159).
  ArtMethod* current_method = thread->GetCurrentMethod(nullptr);
  return current_method != nullptr && current_method->IsNative();
}

void Thread::DumpJavaStack(std::ostream& os, bool check_suspended, bool dump_locks) const {
  // If flip_function is not null, it means we have run a checkpoint
  // before the thread wakes up to execute the flip function and the
  // thread roots haven't been forwarded.  So the following access to
  // the roots (locks or methods in the frames) would be bad. Run it
  // here. TODO: clean up.
  {
    Thread* this_thread = const_cast<Thread*>(this);
    Closure* flip_func = this_thread->GetFlipFunction();
    if (flip_func != nullptr) {
      flip_func->Run(this_thread);
    }
  }

  // Dumping the Java stack involves the verifier for locks. The verifier operates under the
  // assumption that there is no exception pending on entry. Thus, stash any pending exception.
  // Thread::Current() instead of this in case a thread is dumping the stack of another suspended
  // thread.
  StackHandleScope<1> scope(Thread::Current());
  Handle<mirror::Throwable> exc;
  bool have_exception = false;
  if (IsExceptionPending()) {
    exc = scope.NewHandle(GetException());
    const_cast<Thread*>(this)->ClearException();
    have_exception = true;
  }

  std::unique_ptr<Context> context(Context::Create());
  StackDumpVisitor dumper(os, const_cast<Thread*>(this), context.get(),
                          !tls32_.throwing_OutOfMemoryError, check_suspended, dump_locks);
  dumper.WalkStack();

  if (have_exception) {
    const_cast<Thread*>(this)->SetException(exc.Get());
  }
}

void Thread::DumpStack(std::ostream& os,
                       bool dump_native_stack,
                       BacktraceMap* backtrace_map,
                       bool force_dump_stack) const {
  // TODO: we call this code when dying but may not have suspended the thread ourself. The
  //       IsSuspended check is therefore racy with the use for dumping (normally we inhibit
  //       the race with the thread_suspend_count_lock_).
  bool dump_for_abort = (gAborting > 0);
  bool safe_to_dump = (this == Thread::Current() || IsSuspended());
  if (!kIsDebugBuild) {
    // We always want to dump the stack for an abort, however, there is no point dumping another
    // thread's stack in debug builds where we'll hit the not suspended check in the stack walk.
    safe_to_dump = (safe_to_dump || dump_for_abort);
  }
  if (safe_to_dump || force_dump_stack) {
    // If we're currently in native code, dump that stack before dumping the managed stack.
    if (dump_native_stack && (dump_for_abort || force_dump_stack || ShouldShowNativeStack(this))) {
      DumpKernelStack(os, GetTid(), "  kernel: ", false);
      ArtMethod* method =
          GetCurrentMethod(nullptr,
                           /*check_suspended*/ !force_dump_stack,
                           /*abort_on_error*/ !(dump_for_abort || force_dump_stack));
      DumpNativeStack(os, GetTid(), backtrace_map, "  native: ", method);
    }
    DumpJavaStack(os,
                  /*check_suspended*/ !force_dump_stack,
                  /*dump_locks*/ !force_dump_stack);
  } else {
    os << "Not able to dump stack of thread that isn't suspended";
  }
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  if (self->tls32_.thread_exit_check_count == 0) {
    LOG(WARNING) << "Native thread exiting without having called DetachCurrentThread (maybe it's "
        "going to use a pthread_key_create destructor?): " << *self;
    CHECK(is_started_);
#ifdef ART_TARGET_ANDROID
    __get_tls()[TLS_SLOT_ART_THREAD_SELF] = self;
#else
    CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, self), "reattach self");
#endif
    self->tls32_.thread_exit_check_count = 1;
  } else {
    LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
  }
}

void Thread::Startup() {
  CHECK(!is_started_);
  is_started_ = true;
  {
    // MutexLock to keep annotalysis happy.
    //
    // Note we use null for the thread because Thread::Current can
    // return garbage since (is_started_ == true) and
    // Thread::pthread_key_self_ is not yet initialized.
    // This was seen on glibc.
    MutexLock mu(nullptr, *Locks::thread_suspend_count_lock_);
    resume_cond_ = new ConditionVariable("Thread resumption condition variable",
                                         *Locks::thread_suspend_count_lock_);
  }

  // Allocate a TLS slot.
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback),
                     "self key");

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != nullptr) {
    LOG(FATAL) << "Newly-created pthread TLS slot is not nullptr";
  }
}

void Thread::FinishStartup() {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());

  // Finish attaching the main thread.
  ScopedObjectAccess soa(Thread::Current());
  Thread::Current()->CreatePeer("main", false, runtime->GetMainThreadGroup());
  Thread::Current()->AssertNoPendingException();

  Runtime::Current()->GetClassLinker()->RunRootClinits();

  // The thread counts as started from now on. We need to add it to the ThreadGroup. For regular
  // threads, this is done in Thread.start() on the Java side.
  {
    // This is only ever done once. There's no benefit in caching the method.
    jmethodID thread_group_add = soa.Env()->GetMethodID(WellKnownClasses::java_lang_ThreadGroup,
                                                        "add",
                                                        "(Ljava/lang/Thread;)V");
    CHECK(thread_group_add != nullptr);
    ScopedLocalRef<jobject> thread_jobject(
        soa.Env(), soa.Env()->AddLocalReference<jobject>(Thread::Current()->GetPeer()));
    soa.Env()->CallNonvirtualVoidMethod(runtime->GetMainThreadGroup(),
                                        WellKnownClasses::java_lang_ThreadGroup,
                                        thread_group_add,
                                        thread_jobject.get());
    Thread::Current()->AssertNoPendingException();
  }
}

void Thread::Shutdown() {
  CHECK(is_started_);
  is_started_ = false;
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
  MutexLock mu(Thread::Current(), *Locks::thread_suspend_count_lock_);
  if (resume_cond_ != nullptr) {
    delete resume_cond_;
    resume_cond_ = nullptr;
  }
}

Thread::Thread(bool daemon)
    : tls32_(daemon),
      wait_monitor_(nullptr),
      interrupted_(false),
      custom_tls_(nullptr),
      can_call_into_java_(true) {
  wait_mutex_ = new Mutex("a thread wait mutex");
  wait_cond_ = new ConditionVariable("a thread wait condition variable", *wait_mutex_);
  tlsPtr_.instrumentation_stack = new std::deque<instrumentation::InstrumentationStackFrame>;
  tlsPtr_.name = new std::string(kThreadNameDuringStartup);

  static_assert((sizeof(Thread) % 4) == 0U,
                "art::Thread has a size which is not a multiple of 4.");
  tls32_.state_and_flags.as_struct.flags = 0;
  tls32_.state_and_flags.as_struct.state = kNative;
  memset(&tlsPtr_.held_mutexes[0], 0, sizeof(tlsPtr_.held_mutexes));
  std::fill(tlsPtr_.rosalloc_runs,
            tlsPtr_.rosalloc_runs + kNumRosAllocThreadLocalSizeBracketsInThread,
            gc::allocator::RosAlloc::GetDedicatedFullRun());
  tlsPtr_.checkpoint_function = nullptr;
  for (uint32_t i = 0; i < kMaxSuspendBarriers; ++i) {
    tlsPtr_.active_suspend_barriers[i] = nullptr;
  }
  tlsPtr_.flip_function = nullptr;
  tlsPtr_.thread_local_mark_stack = nullptr;
  tls32_.is_transitioning_to_runnable = false;
}

bool Thread::IsStillStarting() const {
  // You might think you can check whether the state is kStarting, but for much of thread startup,
  // the thread is in kNative; it might also be in kVmWait.
  // You might think you can check whether the peer is null, but the peer is actually created and
  // assigned fairly early on, and needs to be.
  // It turns out that the last thing to change is the thread name; that's a good proxy for "has
  // this thread _ever_ entered kRunnable".
  return (tlsPtr_.jpeer == nullptr && tlsPtr_.opeer == nullptr) ||
      (*tlsPtr_.name == kThreadNameDuringStartup);
}

void Thread::AssertPendingException() const {
  CHECK(IsExceptionPending()) << "Pending exception expected.";
}

void Thread::AssertPendingOOMException() const {
  AssertPendingException();
  auto* e = GetException();
  CHECK_EQ(e->GetClass(), DecodeJObject(WellKnownClasses::java_lang_OutOfMemoryError)->AsClass())
      << e->Dump();
}

void Thread::AssertNoPendingException() const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "No pending exception expected: " << GetException()->Dump();
  }
}

void Thread::AssertNoPendingExceptionForNewException(const char* msg) const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Throwing new exception '" << msg << "' with unexpected pending exception: "
        << GetException()->Dump();
  }
}

class MonitorExitVisitor : public SingleRootVisitor {
 public:
  explicit MonitorExitVisitor(Thread* self) : self_(self) { }

  // NO_THREAD_SAFETY_ANALYSIS due to MonitorExit.
  void VisitRoot(mirror::Object* entered_monitor, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    if (self_->HoldsLock(entered_monitor)) {
      LOG(WARNING) << "Calling MonitorExit on object "
                   << entered_monitor << " (" << entered_monitor->PrettyTypeOf() << ")"
                   << " left locked by native thread "
                   << *Thread::Current() << " which is detaching";
      entered_monitor->MonitorExit(self_);
    }
  }

 private:
  Thread* const self_;
};

void Thread::Destroy() {
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());

  if (tlsPtr_.jni_env != nullptr) {
    {
      ScopedObjectAccess soa(self);
      MonitorExitVisitor visitor(self);
      // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
      tlsPtr_.jni_env->monitors.VisitRoots(&visitor, RootInfo(kRootVMInternal));
    }
    // Release locally held global references which releasing may require the mutator lock.
    if (tlsPtr_.jpeer != nullptr) {
      // If pthread_create fails we don't have a jni env here.
      tlsPtr_.jni_env->DeleteGlobalRef(tlsPtr_.jpeer);
      tlsPtr_.jpeer = nullptr;
    }
    if (tlsPtr_.class_loader_override != nullptr) {
      tlsPtr_.jni_env->DeleteGlobalRef(tlsPtr_.class_loader_override);
      tlsPtr_.class_loader_override = nullptr;
    }
  }

  if (tlsPtr_.opeer != nullptr) {
    ScopedObjectAccess soa(self);
    // We may need to call user-supplied managed code, do this before final clean-up.
    HandleUncaughtExceptions(soa);
    RemoveFromThreadGroup(soa);

    // this.nativePeer = 0;
    if (Runtime::Current()->IsActiveTransaction()) {
      jni::DecodeArtField(WellKnownClasses::java_lang_Thread_nativePeer)
          ->SetLong<true>(tlsPtr_.opeer, 0);
    } else {
      jni::DecodeArtField(WellKnownClasses::java_lang_Thread_nativePeer)
          ->SetLong<false>(tlsPtr_.opeer, 0);
    }
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      runtime->GetRuntimeCallbacks()->ThreadDeath(self);
    }


    // Thread.join() is implemented as an Object.wait() on the Thread.lock object. Signal anyone
    // who is waiting.
    ObjPtr<mirror::Object> lock =
        jni::DecodeArtField(WellKnownClasses::java_lang_Thread_lock)->GetObject(tlsPtr_.opeer);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != nullptr) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Object> h_obj(hs.NewHandle(lock));
      ObjectLock<mirror::Object> locker(self, h_obj);
      locker.NotifyAll();
    }
    tlsPtr_.opeer = nullptr;
  }

  {
    ScopedObjectAccess soa(self);
    Runtime::Current()->GetHeap()->RevokeThreadLocalBuffers(this);
    if (kUseReadBarrier) {
      Runtime::Current()->GetHeap()->ConcurrentCopyingCollector()->RevokeThreadLocalMarkStack(this);
    }
  }
}

Thread::~Thread() {
  CHECK(tlsPtr_.class_loader_override == nullptr);
  CHECK(tlsPtr_.jpeer == nullptr);
  CHECK(tlsPtr_.opeer == nullptr);
  bool initialized = (tlsPtr_.jni_env != nullptr);  // Did Thread::Init run?
  if (initialized) {
    delete tlsPtr_.jni_env;
    tlsPtr_.jni_env = nullptr;
  }
  CHECK_NE(GetState(), kRunnable);
  CHECK(!ReadFlag(kCheckpointRequest));
  CHECK(!ReadFlag(kEmptyCheckpointRequest));
  CHECK(tlsPtr_.checkpoint_function == nullptr);
  CHECK_EQ(checkpoint_overflow_.size(), 0u);
  CHECK(tlsPtr_.flip_function == nullptr);
  CHECK_EQ(tls32_.is_transitioning_to_runnable, false);

  // Make sure we processed all deoptimization requests.
  CHECK(tlsPtr_.deoptimization_context_stack == nullptr) << "Missed deoptimization";
  CHECK(tlsPtr_.frame_id_to_shadow_frame == nullptr) <<
      "Not all deoptimized frames have been consumed by the debugger.";

  // We may be deleting a still born thread.
  SetStateUnsafe(kTerminated);

  delete wait_cond_;
  delete wait_mutex_;

  if (tlsPtr_.long_jump_context != nullptr) {
    delete tlsPtr_.long_jump_context;
  }

  if (initialized) {
    CleanupCpu();
  }

  if (tlsPtr_.single_step_control != nullptr) {
    delete tlsPtr_.single_step_control;
  }
  delete tlsPtr_.instrumentation_stack;
  delete tlsPtr_.name;
  delete tlsPtr_.deps_or_stack_trace_sample.stack_trace_sample;

  Runtime::Current()->GetHeap()->AssertThreadLocalBuffersAreRevoked(this);

  TearDownAlternateSignalStack();
}

void Thread::HandleUncaughtExceptions(ScopedObjectAccess& soa) {
  if (!IsExceptionPending()) {
    return;
  }
  ScopedLocalRef<jobject> peer(tlsPtr_.jni_env, soa.AddLocalReference<jobject>(tlsPtr_.opeer));
  ScopedThreadStateChange tsc(this, kNative);

  // Get and clear the exception.
  ScopedLocalRef<jthrowable> exception(tlsPtr_.jni_env, tlsPtr_.jni_env->ExceptionOccurred());
  tlsPtr_.jni_env->ExceptionClear();

  // Call the Thread instance's dispatchUncaughtException(Throwable)
  tlsPtr_.jni_env->CallVoidMethod(peer.get(),
      WellKnownClasses::java_lang_Thread_dispatchUncaughtException,
      exception.get());

  // If the dispatchUncaughtException threw, clear that exception too.
  tlsPtr_.jni_env->ExceptionClear();
}

void Thread::RemoveFromThreadGroup(ScopedObjectAccess& soa) {
  // this.group.removeThread(this);
  // group can be null if we're in the compiler or a test.
  ObjPtr<mirror::Object> ogroup = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_group)
      ->GetObject(tlsPtr_.opeer);
  if (ogroup != nullptr) {
    ScopedLocalRef<jobject> group(soa.Env(), soa.AddLocalReference<jobject>(ogroup));
    ScopedLocalRef<jobject> peer(soa.Env(), soa.AddLocalReference<jobject>(tlsPtr_.opeer));
    ScopedThreadStateChange tsc(soa.Self(), kNative);
    tlsPtr_.jni_env->CallVoidMethod(group.get(),
                                    WellKnownClasses::java_lang_ThreadGroup_removeThread,
                                    peer.get());
  }
}

bool Thread::HandleScopeContains(jobject obj) const {
  StackReference<mirror::Object>* hs_entry =
      reinterpret_cast<StackReference<mirror::Object>*>(obj);
  for (BaseHandleScope* cur = tlsPtr_.top_handle_scope; cur!= nullptr; cur = cur->GetLink()) {
    if (cur->Contains(hs_entry)) {
      return true;
    }
  }
  // JNI code invoked from portable code uses shadow frames rather than the handle scope.
  return tlsPtr_.managed_stack.ShadowFramesContain(hs_entry);
}

void Thread::HandleScopeVisitRoots(RootVisitor* visitor, uint32_t thread_id) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootNativeStack, thread_id));
  for (BaseHandleScope* cur = tlsPtr_.top_handle_scope; cur; cur = cur->GetLink()) {
    cur->VisitRoots(buffered_visitor);
  }
}

ObjPtr<mirror::Object> Thread::DecodeJObject(jobject obj) const {
  if (obj == nullptr) {
    return nullptr;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
  ObjPtr<mirror::Object> result;
  bool expect_null = false;
  // The "kinds" below are sorted by the frequency we expect to encounter them.
  if (kind == kLocal) {
    IndirectReferenceTable& locals = tlsPtr_.jni_env->locals;
    // Local references do not need a read barrier.
    result = locals.Get<kWithoutReadBarrier>(ref);
  } else if (kind == kHandleScopeOrInvalid) {
    // TODO: make stack indirect reference table lookup more efficient.
    // Check if this is a local reference in the handle scope.
    if (LIKELY(HandleScopeContains(obj))) {
      // Read from handle scope.
      result = reinterpret_cast<StackReference<mirror::Object>*>(obj)->AsMirrorPtr();
      VerifyObject(result);
    } else {
      tlsPtr_.jni_env->vm->JniAbortF(nullptr, "use of invalid jobject %p", obj);
      expect_null = true;
      result = nullptr;
    }
  } else if (kind == kGlobal) {
    result = tlsPtr_.jni_env->vm->DecodeGlobal(ref);
  } else {
    DCHECK_EQ(kind, kWeakGlobal);
    result = tlsPtr_.jni_env->vm->DecodeWeakGlobal(const_cast<Thread*>(this), ref);
    if (Runtime::Current()->IsClearedJniWeakGlobal(result)) {
      // This is a special case where it's okay to return null.
      expect_null = true;
      result = nullptr;
    }
  }

  if (UNLIKELY(!expect_null && result == nullptr)) {
    tlsPtr_.jni_env->vm->JniAbortF(nullptr, "use of deleted %s %p",
                                   ToStr<IndirectRefKind>(kind).c_str(), obj);
  }
  return result;
}

bool Thread::IsJWeakCleared(jweak obj) const {
  CHECK(obj != nullptr);
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
  CHECK_EQ(kind, kWeakGlobal);
  return tlsPtr_.jni_env->vm->IsWeakGlobalCleared(const_cast<Thread*>(this), ref);
}

// Implements java.lang.Thread.interrupted.
bool Thread::Interrupted() {
  MutexLock mu(Thread::Current(), *wait_mutex_);
  bool interrupted = IsInterruptedLocked();
  SetInterruptedLocked(false);
  return interrupted;
}

// Implements java.lang.Thread.isInterrupted.
bool Thread::IsInterrupted() {
  MutexLock mu(Thread::Current(), *wait_mutex_);
  return IsInterruptedLocked();
}

void Thread::Interrupt(Thread* self) {
  MutexLock mu(self, *wait_mutex_);
  if (interrupted_) {
    return;
  }
  interrupted_ = true;
  NotifyLocked(self);
}

void Thread::Notify() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *wait_mutex_);
  NotifyLocked(self);
}

void Thread::NotifyLocked(Thread* self) {
  if (wait_monitor_ != nullptr) {
    wait_cond_->Signal(self);
  }
}

void Thread::SetClassLoaderOverride(jobject class_loader_override) {
  if (tlsPtr_.class_loader_override != nullptr) {
    GetJniEnv()->DeleteGlobalRef(tlsPtr_.class_loader_override);
  }
  tlsPtr_.class_loader_override = GetJniEnv()->NewGlobalRef(class_loader_override);
}

using ArtMethodDexPcPair = std::pair<ArtMethod*, uint32_t>;

// Counts the stack trace depth and also fetches the first max_saved_frames frames.
class FetchStackTraceVisitor : public StackVisitor {
 public:
  explicit FetchStackTraceVisitor(Thread* thread,
                                  ArtMethodDexPcPair* saved_frames = nullptr,
                                  size_t max_saved_frames = 0)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        saved_frames_(saved_frames),
        max_saved_frames_(max_saved_frames) {}

  bool VisitFrame() REQUIRES_SHARED(Locks::mutator_lock_) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    ArtMethod* m = GetMethod();
    if (skipping_ && !m->IsRuntimeMethod() &&
        !mirror::Throwable::GetJavaLangThrowable()->IsAssignableFrom(m->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      if (!m->IsRuntimeMethod()) {  // Ignore runtime frames (in particular callee save).
        if (depth_ < max_saved_frames_) {
          saved_frames_[depth_].first = m;
          saved_frames_[depth_].second = m->IsProxyMethod() ? DexFile::kDexNoIndex : GetDexPc();
        }
        ++depth_;
      }
    } else {
      ++skip_depth_;
    }
    return true;
  }

  uint32_t GetDepth() const {
    return depth_;
  }

  uint32_t GetSkipDepth() const {
    return skip_depth_;
  }

 private:
  uint32_t depth_ = 0;
  uint32_t skip_depth_ = 0;
  bool skipping_ = true;
  ArtMethodDexPcPair* saved_frames_;
  const size_t max_saved_frames_;

  DISALLOW_COPY_AND_ASSIGN(FetchStackTraceVisitor);
};

template<bool kTransactionActive>
class BuildInternalStackTraceVisitor : public StackVisitor {
 public:
  BuildInternalStackTraceVisitor(Thread* self, Thread* thread, int skip_depth)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        self_(self),
        skip_depth_(skip_depth),
        pointer_size_(Runtime::Current()->GetClassLinker()->GetImagePointerSize()) {}

  bool Init(int depth) REQUIRES_SHARED(Locks::mutator_lock_) ACQUIRE(Roles::uninterruptible_) {
    // Allocate method trace as an object array where the first element is a pointer array that
    // contains the ArtMethod pointers and dex PCs. The rest of the elements are the declaring
    // class of the ArtMethod pointers.
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    StackHandleScope<1> hs(self_);
    ObjPtr<mirror::Class> array_class = class_linker->GetClassRoot(ClassLinker::kObjectArrayClass);
    // The first element is the methods and dex pc array, the other elements are declaring classes
    // for the methods to ensure classes in the stack trace don't get unloaded.
    Handle<mirror::ObjectArray<mirror::Object>> trace(
        hs.NewHandle(
            mirror::ObjectArray<mirror::Object>::Alloc(hs.Self(), array_class, depth + 1)));
    if (trace == nullptr) {
      // Acquire uninterruptible_ in all paths.
      self_->StartAssertNoThreadSuspension("Building internal stack trace");
      self_->AssertPendingOOMException();
      return false;
    }
    ObjPtr<mirror::PointerArray> methods_and_pcs =
        class_linker->AllocPointerArray(self_, depth * 2);
    const char* last_no_suspend_cause =
        self_->StartAssertNoThreadSuspension("Building internal stack trace");
    if (methods_and_pcs == nullptr) {
      self_->AssertPendingOOMException();
      return false;
    }
    trace->Set(0, methods_and_pcs);
    trace_ = trace.Get();
    // If We are called from native, use non-transactional mode.
    CHECK(last_no_suspend_cause == nullptr) << last_no_suspend_cause;
    return true;
  }

  virtual ~BuildInternalStackTraceVisitor() RELEASE(Roles::uninterruptible_) {
    self_->EndAssertNoThreadSuspension(nullptr);
  }

  bool VisitFrame() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (trace_ == nullptr) {
      return true;  // We're probably trying to fillInStackTrace for an OutOfMemoryError.
    }
    if (skip_depth_ > 0) {
      skip_depth_--;
      return true;
    }
    ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;  // Ignore runtime frames (in particular callee save).
    }
    AddFrame(m, m->IsProxyMethod() ? DexFile::kDexNoIndex : GetDexPc());
    return true;
  }

  void AddFrame(ArtMethod* method, uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<mirror::PointerArray> trace_methods_and_pcs = GetTraceMethodsAndPCs();
    trace_methods_and_pcs->SetElementPtrSize<kTransactionActive>(count_, method, pointer_size_);
    trace_methods_and_pcs->SetElementPtrSize<kTransactionActive>(
        trace_methods_and_pcs->GetLength() / 2 + count_,
        dex_pc,
        pointer_size_);
    // Save the declaring class of the method to ensure that the declaring classes of the methods
    // do not get unloaded while the stack trace is live.
    trace_->Set(count_ + 1, method->GetDeclaringClass());
    ++count_;
  }

  ObjPtr<mirror::PointerArray> GetTraceMethodsAndPCs() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return ObjPtr<mirror::PointerArray>::DownCast(MakeObjPtr(trace_->Get(0)));
  }

  mirror::ObjectArray<mirror::Object>* GetInternalStackTrace() const {
    return trace_;
  }

 private:
  Thread* const self_;
  // How many more frames to skip.
  int32_t skip_depth_;
  // Current position down stack trace.
  uint32_t count_ = 0;
  // An object array where the first element is a pointer array that contains the ArtMethod
  // pointers on the stack and dex PCs. The rest of the elements are the declaring
  // class of the ArtMethod pointers. trace_[i+1] contains the declaring class of the ArtMethod of
  // the i'th frame.
  mirror::ObjectArray<mirror::Object>* trace_ = nullptr;
  // For cross compilation.
  const PointerSize pointer_size_;

  DISALLOW_COPY_AND_ASSIGN(BuildInternalStackTraceVisitor);
};

template<bool kTransactionActive>
jobject Thread::CreateInternalStackTrace(const ScopedObjectAccessAlreadyRunnable& soa) const {
  // Compute depth of stack, save frames if possible to avoid needing to recompute many.
  constexpr size_t kMaxSavedFrames = 256;
  std::unique_ptr<ArtMethodDexPcPair[]> saved_frames(new ArtMethodDexPcPair[kMaxSavedFrames]);
  FetchStackTraceVisitor count_visitor(const_cast<Thread*>(this),
                                       &saved_frames[0],
                                       kMaxSavedFrames);
  count_visitor.WalkStack();
  const uint32_t depth = count_visitor.GetDepth();
  const uint32_t skip_depth = count_visitor.GetSkipDepth();

  // Build internal stack trace.
  BuildInternalStackTraceVisitor<kTransactionActive> build_trace_visitor(soa.Self(),
                                                                         const_cast<Thread*>(this),
                                                                         skip_depth);
  if (!build_trace_visitor.Init(depth)) {
    return nullptr;  // Allocation failed.
  }
  // If we saved all of the frames we don't even need to do the actual stack walk. This is faster
  // than doing the stack walk twice.
  if (depth < kMaxSavedFrames) {
    for (size_t i = 0; i < depth; ++i) {
      build_trace_visitor.AddFrame(saved_frames[i].first, saved_frames[i].second);
    }
  } else {
    build_trace_visitor.WalkStack();
  }

  mirror::ObjectArray<mirror::Object>* trace = build_trace_visitor.GetInternalStackTrace();
  if (kIsDebugBuild) {
    ObjPtr<mirror::PointerArray> trace_methods = build_trace_visitor.GetTraceMethodsAndPCs();
    // Second half of trace_methods is dex PCs.
    for (uint32_t i = 0; i < static_cast<uint32_t>(trace_methods->GetLength() / 2); ++i) {
      auto* method = trace_methods->GetElementPtrSize<ArtMethod*>(
          i, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
      CHECK(method != nullptr);
    }
  }
  return soa.AddLocalReference<jobject>(trace);
}
template jobject Thread::CreateInternalStackTrace<false>(
    const ScopedObjectAccessAlreadyRunnable& soa) const;
template jobject Thread::CreateInternalStackTrace<true>(
    const ScopedObjectAccessAlreadyRunnable& soa) const;

bool Thread::IsExceptionThrownByCurrentMethod(ObjPtr<mirror::Throwable> exception) const {
  // Only count the depth since we do not pass a stack frame array as an argument.
  FetchStackTraceVisitor count_visitor(const_cast<Thread*>(this));
  count_visitor.WalkStack();
  return count_visitor.GetDepth() == static_cast<uint32_t>(exception->GetStackDepth());
}

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(
    const ScopedObjectAccessAlreadyRunnable& soa,
    jobject internal,
    jobjectArray output_array,
    int* stack_depth) {
  // Decode the internal stack trace into the depth, method trace and PC trace.
  // Subtract one for the methods and PC trace.
  int32_t depth = soa.Decode<mirror::Array>(internal)->GetLength() - 1;
  DCHECK_GE(depth, 0);

  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

  jobjectArray result;

  if (output_array != nullptr) {
    // Reuse the array we were given.
    result = output_array;
    // ...adjusting the number of frames we'll write to not exceed the array length.
    const int32_t traces_length =
        soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>>(result)->GetLength();
    depth = std::min(depth, traces_length);
  } else {
    // Create java_trace array and place in local reference table
    mirror::ObjectArray<mirror::StackTraceElement>* java_traces =
        class_linker->AllocStackTraceElementArray(soa.Self(), depth);
    if (java_traces == nullptr) {
      return nullptr;
    }
    result = soa.AddLocalReference<jobjectArray>(java_traces);
  }

  if (stack_depth != nullptr) {
    *stack_depth = depth;
  }

  for (int32_t i = 0; i < depth; ++i) {
    ObjPtr<mirror::ObjectArray<mirror::Object>> decoded_traces =
        soa.Decode<mirror::Object>(internal)->AsObjectArray<mirror::Object>();
    // Methods and dex PC trace is element 0.
    DCHECK(decoded_traces->Get(0)->IsIntArray() || decoded_traces->Get(0)->IsLongArray());
    ObjPtr<mirror::PointerArray> const method_trace =
        ObjPtr<mirror::PointerArray>::DownCast(MakeObjPtr(decoded_traces->Get(0)));
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    ArtMethod* method = method_trace->GetElementPtrSize<ArtMethod*>(i, kRuntimePointerSize);
    uint32_t dex_pc = method_trace->GetElementPtrSize<uint32_t>(
        i + method_trace->GetLength() / 2, kRuntimePointerSize);
    int32_t line_number;
    StackHandleScope<3> hs(soa.Self());
    auto class_name_object(hs.NewHandle<mirror::String>(nullptr));
    auto source_name_object(hs.NewHandle<mirror::String>(nullptr));
    if (method->IsProxyMethod()) {
      line_number = -1;
      class_name_object.Assign(method->GetDeclaringClass()->GetName());
      // source_name_object intentionally left null for proxy methods
    } else {
      line_number = method->GetLineNumFromDexPC(dex_pc);
      // Allocate element, potentially triggering GC
      // TODO: reuse class_name_object via Class::name_?
      const char* descriptor = method->GetDeclaringClassDescriptor();
      CHECK(descriptor != nullptr);
      std::string class_name(PrettyDescriptor(descriptor));
      class_name_object.Assign(
          mirror::String::AllocFromModifiedUtf8(soa.Self(), class_name.c_str()));
      if (class_name_object == nullptr) {
        soa.Self()->AssertPendingOOMException();
        return nullptr;
      }
      const char* source_file = method->GetDeclaringClassSourceFile();
      if (line_number == -1) {
        // Make the line_number field of StackTraceElement hold the dex pc.
        // source_name_object is intentionally left null if we failed to map the dex pc to
        // a line number (most probably because there is no debug info). See b/30183883.
        line_number = dex_pc;
      } else {
        if (source_file != nullptr) {
          source_name_object.Assign(mirror::String::AllocFromModifiedUtf8(soa.Self(), source_file));
          if (source_name_object == nullptr) {
            soa.Self()->AssertPendingOOMException();
            return nullptr;
          }
        }
      }
    }
    const char* method_name = method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetName();
    CHECK(method_name != nullptr);
    Handle<mirror::String> method_name_object(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), method_name)));
    if (method_name_object == nullptr) {
      return nullptr;
    }
    ObjPtr<mirror::StackTraceElement> obj = mirror::StackTraceElement::Alloc(soa.Self(),
                                                                             class_name_object,
                                                                             method_name_object,
                                                                             source_name_object,
                                                                             line_number);
    if (obj == nullptr) {
      return nullptr;
    }
    // We are called from native: use non-transactional mode.
    soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>>(result)->Set<false>(i, obj);
  }
  return result;
}

void Thread::ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(exception_class_descriptor, fmt, args);
  va_end(args);
}

void Thread::ThrowNewExceptionV(const char* exception_class_descriptor,
                                const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  ThrowNewException(exception_class_descriptor, msg.c_str());
}

void Thread::ThrowNewException(const char* exception_class_descriptor,
                               const char* msg) {
  // Callers should either clear or call ThrowNewWrappedException.
  AssertNoPendingExceptionForNewException(msg);
  ThrowNewWrappedException(exception_class_descriptor, msg);
}

static ObjPtr<mirror::ClassLoader> GetCurrentClassLoader(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* method = self->GetCurrentMethod(nullptr);
  return method != nullptr
      ? method->GetDeclaringClass()->GetClassLoader()
      : nullptr;
}

void Thread::ThrowNewWrappedException(const char* exception_class_descriptor,
                                      const char* msg) {
  DCHECK_EQ(this, Thread::Current());
  ScopedObjectAccessUnchecked soa(this);
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(GetCurrentClassLoader(soa.Self())));
  ScopedLocalRef<jobject> cause(GetJniEnv(), soa.AddLocalReference<jobject>(GetException()));
  ClearException();
  Runtime* runtime = Runtime::Current();
  auto* cl = runtime->GetClassLinker();
  Handle<mirror::Class> exception_class(
      hs.NewHandle(cl->FindClass(this, exception_class_descriptor, class_loader)));
  if (UNLIKELY(exception_class == nullptr)) {
    CHECK(IsExceptionPending());
    LOG(ERROR) << "No exception class " << PrettyDescriptor(exception_class_descriptor);
    return;
  }

  if (UNLIKELY(!runtime->GetClassLinker()->EnsureInitialized(soa.Self(), exception_class, true,
                                                             true))) {
    DCHECK(IsExceptionPending());
    return;
  }
  DCHECK(!runtime->IsStarted() || exception_class->IsThrowableClass());
  Handle<mirror::Throwable> exception(
      hs.NewHandle(ObjPtr<mirror::Throwable>::DownCast(exception_class->AllocObject(this))));

  // If we couldn't allocate the exception, throw the pre-allocated out of memory exception.
  if (exception == nullptr) {
    SetException(Runtime::Current()->GetPreAllocatedOutOfMemoryError());
    return;
  }

  // Choose an appropriate constructor and set up the arguments.
  const char* signature;
  ScopedLocalRef<jstring> msg_string(GetJniEnv(), nullptr);
  if (msg != nullptr) {
    // Ensure we remember this and the method over the String allocation.
    msg_string.reset(
        soa.AddLocalReference<jstring>(mirror::String::AllocFromModifiedUtf8(this, msg)));
    if (UNLIKELY(msg_string.get() == nullptr)) {
      CHECK(IsExceptionPending());  // OOME.
      return;
    }
    if (cause.get() == nullptr) {
      signature = "(Ljava/lang/String;)V";
    } else {
      signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    }
  } else {
    if (cause.get() == nullptr) {
      signature = "()V";
    } else {
      signature = "(Ljava/lang/Throwable;)V";
    }
  }
  ArtMethod* exception_init_method =
      exception_class->FindDeclaredDirectMethod("<init>", signature, cl->GetImagePointerSize());

  CHECK(exception_init_method != nullptr) << "No <init>" << signature << " in "
      << PrettyDescriptor(exception_class_descriptor);

  if (UNLIKELY(!runtime->IsStarted())) {
    // Something is trying to throw an exception without a started runtime, which is the common
    // case in the compiler. We won't be able to invoke the constructor of the exception, so set
    // the exception fields directly.
    if (msg != nullptr) {
      exception->SetDetailMessage(DecodeJObject(msg_string.get())->AsString());
    }
    if (cause.get() != nullptr) {
      exception->SetCause(DecodeJObject(cause.get())->AsThrowable());
    }
    ScopedLocalRef<jobject> trace(GetJniEnv(),
                                  Runtime::Current()->IsActiveTransaction()
                                      ? CreateInternalStackTrace<true>(soa)
                                      : CreateInternalStackTrace<false>(soa));
    if (trace.get() != nullptr) {
      exception->SetStackState(DecodeJObject(trace.get()).Ptr());
    }
    SetException(exception.Get());
  } else {
    jvalue jv_args[2];
    size_t i = 0;

    if (msg != nullptr) {
      jv_args[i].l = msg_string.get();
      ++i;
    }
    if (cause.get() != nullptr) {
      jv_args[i].l = cause.get();
      ++i;
    }
    ScopedLocalRef<jobject> ref(soa.Env(), soa.AddLocalReference<jobject>(exception.Get()));
    InvokeWithJValues(soa, ref.get(), jni::EncodeArtMethod(exception_init_method), jv_args);
    if (LIKELY(!IsExceptionPending())) {
      SetException(exception.Get());
    }
  }
}

void Thread::ThrowOutOfMemoryError(const char* msg) {
  LOG(WARNING) << StringPrintf("Throwing OutOfMemoryError \"%s\"%s",
      msg, (tls32_.throwing_OutOfMemoryError ? " (recursive case)" : ""));
  if (!tls32_.throwing_OutOfMemoryError) {
    tls32_.throwing_OutOfMemoryError = true;
    ThrowNewException("Ljava/lang/OutOfMemoryError;", msg);
    tls32_.throwing_OutOfMemoryError = false;
  } else {
    Dump(LOG_STREAM(WARNING));  // The pre-allocated OOME has no stack, so help out and log one.
    SetException(Runtime::Current()->GetPreAllocatedOutOfMemoryError());
  }
}

Thread* Thread::CurrentFromGdb() {
  return Thread::Current();
}

void Thread::DumpFromGdb() const {
  std::ostringstream ss;
  Dump(ss);
  std::string str(ss.str());
  // log to stderr for debugging command line processes
  std::cerr << str;
#ifdef ART_TARGET_ANDROID
  // log to logcat for debugging frameworks processes
  LOG(INFO) << str;
#endif
}

// Explicitly instantiate 32 and 64bit thread offset dumping support.
template
void Thread::DumpThreadOffset<PointerSize::k32>(std::ostream& os, uint32_t offset);
template
void Thread::DumpThreadOffset<PointerSize::k64>(std::ostream& os, uint32_t offset);

template<PointerSize ptr_size>
void Thread::DumpThreadOffset(std::ostream& os, uint32_t offset) {
#define DO_THREAD_OFFSET(x, y) \
    if (offset == (x).Uint32Value()) { \
      os << (y); \
      return; \
    }
  DO_THREAD_OFFSET(ThreadFlagsOffset<ptr_size>(), "state_and_flags")
  DO_THREAD_OFFSET(CardTableOffset<ptr_size>(), "card_table")
  DO_THREAD_OFFSET(ExceptionOffset<ptr_size>(), "exception")
  DO_THREAD_OFFSET(PeerOffset<ptr_size>(), "peer");
  DO_THREAD_OFFSET(JniEnvOffset<ptr_size>(), "jni_env")
  DO_THREAD_OFFSET(SelfOffset<ptr_size>(), "self")
  DO_THREAD_OFFSET(StackEndOffset<ptr_size>(), "stack_end")
  DO_THREAD_OFFSET(ThinLockIdOffset<ptr_size>(), "thin_lock_thread_id")
  DO_THREAD_OFFSET(TopOfManagedStackOffset<ptr_size>(), "top_quick_frame_method")
  DO_THREAD_OFFSET(TopShadowFrameOffset<ptr_size>(), "top_shadow_frame")
  DO_THREAD_OFFSET(TopHandleScopeOffset<ptr_size>(), "top_handle_scope")
  DO_THREAD_OFFSET(ThreadSuspendTriggerOffset<ptr_size>(), "suspend_trigger")
#undef DO_THREAD_OFFSET

#define JNI_ENTRY_POINT_INFO(x) \
    if (JNI_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  JNI_ENTRY_POINT_INFO(pDlsymLookup)
#undef JNI_ENTRY_POINT_INFO

#define QUICK_ENTRY_POINT_INFO(x) \
    if (QUICK_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved8)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved16)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved32)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved64)
  QUICK_ENTRY_POINT_INFO(pAllocObjectResolved)
  QUICK_ENTRY_POINT_INFO(pAllocObjectInitialized)
  QUICK_ENTRY_POINT_INFO(pAllocObjectWithChecks)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromBytes)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromChars)
  QUICK_ENTRY_POINT_INFO(pAllocStringFromString)
  QUICK_ENTRY_POINT_INFO(pInstanceofNonTrivial)
  QUICK_ENTRY_POINT_INFO(pCheckInstanceOf)
  QUICK_ENTRY_POINT_INFO(pInitializeStaticStorage)
  QUICK_ENTRY_POINT_INFO(pInitializeTypeAndVerifyAccess)
  QUICK_ENTRY_POINT_INFO(pInitializeType)
  QUICK_ENTRY_POINT_INFO(pResolveString)
  QUICK_ENTRY_POINT_INFO(pSet8Instance)
  QUICK_ENTRY_POINT_INFO(pSet8Static)
  QUICK_ENTRY_POINT_INFO(pSet16Instance)
  QUICK_ENTRY_POINT_INFO(pSet16Static)
  QUICK_ENTRY_POINT_INFO(pSet32Instance)
  QUICK_ENTRY_POINT_INFO(pSet32Static)
  QUICK_ENTRY_POINT_INFO(pSet64Instance)
  QUICK_ENTRY_POINT_INFO(pSet64Static)
  QUICK_ENTRY_POINT_INFO(pSetObjInstance)
  QUICK_ENTRY_POINT_INFO(pSetObjStatic)
  QUICK_ENTRY_POINT_INFO(pGetByteInstance)
  QUICK_ENTRY_POINT_INFO(pGetBooleanInstance)
  QUICK_ENTRY_POINT_INFO(pGetByteStatic)
  QUICK_ENTRY_POINT_INFO(pGetBooleanStatic)
  QUICK_ENTRY_POINT_INFO(pGetShortInstance)
  QUICK_ENTRY_POINT_INFO(pGetCharInstance)
  QUICK_ENTRY_POINT_INFO(pGetShortStatic)
  QUICK_ENTRY_POINT_INFO(pGetCharStatic)
  QUICK_ENTRY_POINT_INFO(pGet32Instance)
  QUICK_ENTRY_POINT_INFO(pGet32Static)
  QUICK_ENTRY_POINT_INFO(pGet64Instance)
  QUICK_ENTRY_POINT_INFO(pGet64Static)
  QUICK_ENTRY_POINT_INFO(pGetObjInstance)
  QUICK_ENTRY_POINT_INFO(pGetObjStatic)
  QUICK_ENTRY_POINT_INFO(pAputObject)
  QUICK_ENTRY_POINT_INFO(pJniMethodStart)
  QUICK_ENTRY_POINT_INFO(pJniMethodStartSynchronized)
  QUICK_ENTRY_POINT_INFO(pJniMethodEnd)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndSynchronized)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndWithReference)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndWithReferenceSynchronized)
  QUICK_ENTRY_POINT_INFO(pQuickGenericJniTrampoline)
  QUICK_ENTRY_POINT_INFO(pLockObject)
  QUICK_ENTRY_POINT_INFO(pUnlockObject)
  QUICK_ENTRY_POINT_INFO(pCmpgDouble)
  QUICK_ENTRY_POINT_INFO(pCmpgFloat)
  QUICK_ENTRY_POINT_INFO(pCmplDouble)
  QUICK_ENTRY_POINT_INFO(pCmplFloat)
  QUICK_ENTRY_POINT_INFO(pCos)
  QUICK_ENTRY_POINT_INFO(pSin)
  QUICK_ENTRY_POINT_INFO(pAcos)
  QUICK_ENTRY_POINT_INFO(pAsin)
  QUICK_ENTRY_POINT_INFO(pAtan)
  QUICK_ENTRY_POINT_INFO(pAtan2)
  QUICK_ENTRY_POINT_INFO(pCbrt)
  QUICK_ENTRY_POINT_INFO(pCosh)
  QUICK_ENTRY_POINT_INFO(pExp)
  QUICK_ENTRY_POINT_INFO(pExpm1)
  QUICK_ENTRY_POINT_INFO(pHypot)
  QUICK_ENTRY_POINT_INFO(pLog)
  QUICK_ENTRY_POINT_INFO(pLog10)
  QUICK_ENTRY_POINT_INFO(pNextAfter)
  QUICK_ENTRY_POINT_INFO(pSinh)
  QUICK_ENTRY_POINT_INFO(pTan)
  QUICK_ENTRY_POINT_INFO(pTanh)
  QUICK_ENTRY_POINT_INFO(pFmod)
  QUICK_ENTRY_POINT_INFO(pL2d)
  QUICK_ENTRY_POINT_INFO(pFmodf)
  QUICK_ENTRY_POINT_INFO(pL2f)
  QUICK_ENTRY_POINT_INFO(pD2iz)
  QUICK_ENTRY_POINT_INFO(pF2iz)
  QUICK_ENTRY_POINT_INFO(pIdivmod)
  QUICK_ENTRY_POINT_INFO(pD2l)
  QUICK_ENTRY_POINT_INFO(pF2l)
  QUICK_ENTRY_POINT_INFO(pLdiv)
  QUICK_ENTRY_POINT_INFO(pLmod)
  QUICK_ENTRY_POINT_INFO(pLmul)
  QUICK_ENTRY_POINT_INFO(pShlLong)
  QUICK_ENTRY_POINT_INFO(pShrLong)
  QUICK_ENTRY_POINT_INFO(pUshrLong)
  QUICK_ENTRY_POINT_INFO(pIndexOf)
  QUICK_ENTRY_POINT_INFO(pStringCompareTo)
  QUICK_ENTRY_POINT_INFO(pMemcpy)
  QUICK_ENTRY_POINT_INFO(pQuickImtConflictTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickResolutionTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickToInterpreterBridge)
  QUICK_ENTRY_POINT_INFO(pInvokeDirectTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeInterfaceTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeStaticTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeSuperTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeVirtualTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokePolymorphic)
  QUICK_ENTRY_POINT_INFO(pTestSuspend)
  QUICK_ENTRY_POINT_INFO(pDeliverException)
  QUICK_ENTRY_POINT_INFO(pThrowArrayBounds)
  QUICK_ENTRY_POINT_INFO(pThrowDivZero)
  QUICK_ENTRY_POINT_INFO(pThrowNullPointer)
  QUICK_ENTRY_POINT_INFO(pThrowStackOverflow)
  QUICK_ENTRY_POINT_INFO(pDeoptimize)
  QUICK_ENTRY_POINT_INFO(pA64Load)
  QUICK_ENTRY_POINT_INFO(pA64Store)
  QUICK_ENTRY_POINT_INFO(pNewEmptyString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_B)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BI)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIIString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BIICharset)
  QUICK_ENTRY_POINT_INFO(pNewStringFromBytes_BCharset)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_C)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_CII)
  QUICK_ENTRY_POINT_INFO(pNewStringFromChars_IIC)
  QUICK_ENTRY_POINT_INFO(pNewStringFromCodePoints)
  QUICK_ENTRY_POINT_INFO(pNewStringFromString)
  QUICK_ENTRY_POINT_INFO(pNewStringFromStringBuffer)
  QUICK_ENTRY_POINT_INFO(pNewStringFromStringBuilder)
  QUICK_ENTRY_POINT_INFO(pReadBarrierJni)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg00)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg01)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg02)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg03)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg04)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg05)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg06)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg07)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg08)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg09)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg10)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg11)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg12)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg13)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg14)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg15)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg16)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg17)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg18)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg19)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg20)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg21)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg22)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg23)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg24)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg25)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg26)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg27)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg28)
  QUICK_ENTRY_POINT_INFO(pReadBarrierMarkReg29)
  QUICK_ENTRY_POINT_INFO(pReadBarrierSlow)
  QUICK_ENTRY_POINT_INFO(pReadBarrierForRootSlow)

  QUICK_ENTRY_POINT_INFO(pJniMethodFastStart)
  QUICK_ENTRY_POINT_INFO(pJniMethodFastEnd)
#undef QUICK_ENTRY_POINT_INFO

  os << offset;
}

void Thread::QuickDeliverException() {
  // Get exception from thread.
  ObjPtr<mirror::Throwable> exception = GetException();
  CHECK(exception != nullptr);
  if (exception == GetDeoptimizationException()) {
    artDeoptimize(this);
    UNREACHABLE();
  }

  // This is a real exception: let the instrumentation know about it.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (instrumentation->HasExceptionCaughtListeners() &&
      IsExceptionThrownByCurrentMethod(exception)) {
    // Instrumentation may cause GC so keep the exception object safe.
    StackHandleScope<1> hs(this);
    HandleWrapperObjPtr<mirror::Throwable> h_exception(hs.NewHandleWrapper(&exception));
    instrumentation->ExceptionCaughtEvent(this, exception.Ptr());
  }
  // Does instrumentation need to deoptimize the stack?
  // Note: we do this *after* reporting the exception to instrumentation in case it
  // now requires deoptimization. It may happen if a debugger is attached and requests
  // new events (single-step, breakpoint, ...) when the exception is reported.
  if (Dbg::IsForcedInterpreterNeededForException(this)) {
    NthCallerVisitor visitor(this, 0, false);
    visitor.WalkStack();
    if (Runtime::Current()->IsAsyncDeoptimizeable(visitor.caller_pc)) {
      // Save the exception into the deoptimization context so it can be restored
      // before entering the interpreter.
      PushDeoptimizationContext(
          JValue(), /*is_reference */ false, /* from_code */ false, exception);
      artDeoptimize(this);
      UNREACHABLE();
    } else {
      LOG(WARNING) << "Got a deoptimization request on un-deoptimizable method "
                   << visitor.caller->PrettyMethod();
    }
  }

  // Don't leave exception visible while we try to find the handler, which may cause class
  // resolution.
  ClearException();
  QuickExceptionHandler exception_handler(this, false);
  exception_handler.FindCatch(exception);
  exception_handler.UpdateInstrumentationStack();
  exception_handler.DoLongJump();
}

Context* Thread::GetLongJumpContext() {
  Context* result = tlsPtr_.long_jump_context;
  if (result == nullptr) {
    result = Context::Create();
  } else {
    tlsPtr_.long_jump_context = nullptr;  // Avoid context being shared.
    result->Reset();
  }
  return result;
}

// Note: this visitor may return with a method set, but dex_pc_ being DexFile:kDexNoIndex. This is
//       so we don't abort in a special situation (thinlocked monitor) when dumping the Java stack.
struct CurrentMethodVisitor FINAL : public StackVisitor {
  CurrentMethodVisitor(Thread* thread, Context* context, bool check_suspended, bool abort_on_error)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread,
                     context,
                     StackVisitor::StackWalkKind::kIncludeInlinedFrames,
                     check_suspended),
        this_object_(nullptr),
        method_(nullptr),
        dex_pc_(0),
        abort_on_error_(abort_on_error) {}
  bool VisitFrame() OVERRIDE REQUIRES_SHARED(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      // Continue if this is a runtime method.
      return true;
    }
    if (context_ != nullptr) {
      this_object_ = GetThisObject();
    }
    method_ = m;
    dex_pc_ = GetDexPc(abort_on_error_);
    return false;
  }
  ObjPtr<mirror::Object> this_object_;
  ArtMethod* method_;
  uint32_t dex_pc_;
  const bool abort_on_error_;
};

ArtMethod* Thread::GetCurrentMethod(uint32_t* dex_pc,
                                    bool check_suspended,
                                    bool abort_on_error) const {
  CurrentMethodVisitor visitor(const_cast<Thread*>(this),
                               nullptr,
                               check_suspended,
                               abort_on_error);
  visitor.WalkStack(false);
  if (dex_pc != nullptr) {
    *dex_pc = visitor.dex_pc_;
  }
  return visitor.method_;
}

bool Thread::HoldsLock(ObjPtr<mirror::Object> object) const {
  return object != nullptr && object->GetLockOwnerThreadId() == GetThreadId();
}

// RootVisitor parameters are: (const Object* obj, size_t vreg, const StackVisitor* visitor).
template <typename RootVisitor, bool kPrecise = false>
class ReferenceMapVisitor : public StackVisitor {
 public:
  ReferenceMapVisitor(Thread* thread, Context* context, RootVisitor& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
        // We are visiting the references in compiled frames, so we do not need
        // to know the inlined frames.
      : StackVisitor(thread, context, StackVisitor::StackWalkKind::kSkipInlinedFrames),
        visitor_(visitor) {}

  bool VisitFrame() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (false) {
      LOG(INFO) << "Visiting stack roots in " << ArtMethod::PrettyMethod(GetMethod())
                << StringPrintf("@ PC:%04x", GetDexPc());
    }
    ShadowFrame* shadow_frame = GetCurrentShadowFrame();
    if (shadow_frame != nullptr) {
      VisitShadowFrame(shadow_frame);
    } else {
      VisitQuickFrame();
    }
    return true;
  }

  void VisitShadowFrame(ShadowFrame* shadow_frame) REQUIRES_SHARED(Locks::mutator_lock_) {
    ArtMethod* m = shadow_frame->GetMethod();
    VisitDeclaringClass(m);
    DCHECK(m != nullptr);
    size_t num_regs = shadow_frame->NumberOfVRegs();
    DCHECK(m->IsNative() || shadow_frame->HasReferenceArray());
    // handle scope for JNI or References for interpreter.
    for (size_t reg = 0; reg < num_regs; ++reg) {
      mirror::Object* ref = shadow_frame->GetVRegReference(reg);
      if (ref != nullptr) {
        mirror::Object* new_ref = ref;
        visitor_(&new_ref, reg, this);
        if (new_ref != ref) {
          shadow_frame->SetVRegReference(reg, new_ref);
        }
      }
    }
    // Mark lock count map required for structured locking checks.
    shadow_frame->GetLockCountData().VisitMonitors(visitor_, -1, this);
  }

 private:
  // Visiting the declaring class is necessary so that we don't unload the class of a method that
  // is executing. We need to ensure that the code stays mapped. NO_THREAD_SAFETY_ANALYSIS since
  // the threads do not all hold the heap bitmap lock for parallel GC.
  void VisitDeclaringClass(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_)
      NO_THREAD_SAFETY_ANALYSIS {
    ObjPtr<mirror::Class> klass = method->GetDeclaringClassUnchecked<kWithoutReadBarrier>();
    // klass can be null for runtime methods.
    if (klass != nullptr) {
      if (kVerifyImageObjectsMarked) {
        gc::Heap* const heap = Runtime::Current()->GetHeap();
        gc::space::ContinuousSpace* space = heap->FindContinuousSpaceFromObject(klass,
                                                                                /*fail_ok*/true);
        if (space != nullptr && space->IsImageSpace()) {
          bool failed = false;
          if (!space->GetLiveBitmap()->Test(klass.Ptr())) {
            failed = true;
            LOG(FATAL_WITHOUT_ABORT) << "Unmarked object in image " << *space;
          } else if (!heap->GetLiveBitmap()->Test(klass.Ptr())) {
            failed = true;
            LOG(FATAL_WITHOUT_ABORT) << "Unmarked object in image through live bitmap " << *space;
          }
          if (failed) {
            GetThread()->Dump(LOG_STREAM(FATAL_WITHOUT_ABORT));
            space->AsImageSpace()->DumpSections(LOG_STREAM(FATAL_WITHOUT_ABORT));
            LOG(FATAL_WITHOUT_ABORT) << "Method@" << method->GetDexMethodIndex() << ":" << method
                                     << " klass@" << klass.Ptr();
            // Pretty info last in case it crashes.
            LOG(FATAL) << "Method " << method->PrettyMethod() << " klass "
                       << klass->PrettyClass();
          }
        }
      }
      mirror::Object* new_ref = klass.Ptr();
      visitor_(&new_ref, -1, this);
      if (new_ref != klass) {
        method->CASDeclaringClass(klass.Ptr(), new_ref->AsClass());
      }
    }
  }

  template <typename T>
  ALWAYS_INLINE
  inline void VisitQuickFrameWithVregCallback() REQUIRES_SHARED(Locks::mutator_lock_) {
    ArtMethod** cur_quick_frame = GetCurrentQuickFrame();
    DCHECK(cur_quick_frame != nullptr);
    ArtMethod* m = *cur_quick_frame;
    VisitDeclaringClass(m);

    // Process register map (which native and runtime methods don't have)
    if (!m->IsNative() && !m->IsRuntimeMethod() && (!m->IsProxyMethod() || m->IsConstructor())) {
      const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
      DCHECK(method_header->IsOptimized());
      auto* vreg_base = reinterpret_cast<StackReference<mirror::Object>*>(
          reinterpret_cast<uintptr_t>(cur_quick_frame));
      uintptr_t native_pc_offset = method_header->NativeQuickPcOffset(GetCurrentQuickFramePc());
      CodeInfo code_info = method_header->GetOptimizedCodeInfo();
      CodeInfoEncoding encoding = code_info.ExtractEncoding();
      StackMap map = code_info.GetStackMapForNativePcOffset(native_pc_offset, encoding);
      DCHECK(map.IsValid());

      T vreg_info(m, code_info, encoding, map, visitor_);

      // Visit stack entries that hold pointers.
      const size_t number_of_bits = code_info.GetNumberOfStackMaskBits(encoding);
      BitMemoryRegion stack_mask = code_info.GetStackMaskOf(encoding, map);
      for (size_t i = 0; i < number_of_bits; ++i) {
        if (stack_mask.LoadBit(i)) {
          auto* ref_addr = vreg_base + i;
          mirror::Object* ref = ref_addr->AsMirrorPtr();
          if (ref != nullptr) {
            mirror::Object* new_ref = ref;
            vreg_info.VisitStack(&new_ref, i, this);
            if (ref != new_ref) {
              ref_addr->Assign(new_ref);
           }
          }
        }
      }
      // Visit callee-save registers that hold pointers.
      uint32_t register_mask = code_info.GetRegisterMaskOf(encoding, map);
      for (size_t i = 0; i < BitSizeOf<uint32_t>(); ++i) {
        if (register_mask & (1 << i)) {
          mirror::Object** ref_addr = reinterpret_cast<mirror::Object**>(GetGPRAddress(i));
          if (kIsDebugBuild && ref_addr == nullptr) {
            std::string thread_name;
            GetThread()->GetThreadName(thread_name);
            LOG(FATAL_WITHOUT_ABORT) << "On thread " << thread_name;
            DescribeStack(GetThread());
            LOG(FATAL) << "Found an unsaved callee-save register " << i << " (null GPRAddress) "
                       << "set in register_mask=" << register_mask << " at " << DescribeLocation();
          }
          if (*ref_addr != nullptr) {
            vreg_info.VisitRegister(ref_addr, i, this);
          }
        }
      }
    }
  }

  void VisitQuickFrame() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kPrecise) {
      VisitQuickFramePrecise();
    } else {
      VisitQuickFrameNonPrecise();
    }
  }

  void VisitQuickFrameNonPrecise() REQUIRES_SHARED(Locks::mutator_lock_) {
    struct UndefinedVRegInfo {
      UndefinedVRegInfo(ArtMethod* method ATTRIBUTE_UNUSED,
                        const CodeInfo& code_info ATTRIBUTE_UNUSED,
                        const CodeInfoEncoding& encoding ATTRIBUTE_UNUSED,
                        const StackMap& map ATTRIBUTE_UNUSED,
                        RootVisitor& _visitor)
          : visitor(_visitor) {
      }

      ALWAYS_INLINE
      void VisitStack(mirror::Object** ref,
                      size_t stack_index ATTRIBUTE_UNUSED,
                      const StackVisitor* stack_visitor)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        visitor(ref, -1, stack_visitor);
      }

      ALWAYS_INLINE
      void VisitRegister(mirror::Object** ref,
                         size_t register_index ATTRIBUTE_UNUSED,
                         const StackVisitor* stack_visitor)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        visitor(ref, -1, stack_visitor);
      }

      RootVisitor& visitor;
    };
    VisitQuickFrameWithVregCallback<UndefinedVRegInfo>();
  }

  void VisitQuickFramePrecise() REQUIRES_SHARED(Locks::mutator_lock_) {
    struct StackMapVRegInfo {
      StackMapVRegInfo(ArtMethod* method,
                       const CodeInfo& _code_info,
                       const CodeInfoEncoding& _encoding,
                       const StackMap& map,
                       RootVisitor& _visitor)
          : number_of_dex_registers(method->GetCodeItem()->registers_size_),
            code_info(_code_info),
            encoding(_encoding),
            dex_register_map(code_info.GetDexRegisterMapOf(map,
                                                           encoding,
                                                           number_of_dex_registers)),
            visitor(_visitor) {
      }

      // TODO: If necessary, we should consider caching a reverse map instead of the linear
      //       lookups for each location.
      void FindWithType(const size_t index,
                        const DexRegisterLocation::Kind kind,
                        mirror::Object** ref,
                        const StackVisitor* stack_visitor)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        bool found = false;
        for (size_t dex_reg = 0; dex_reg != number_of_dex_registers; ++dex_reg) {
          DexRegisterLocation location = dex_register_map.GetDexRegisterLocation(
              dex_reg, number_of_dex_registers, code_info, encoding);
          if (location.GetKind() == kind && static_cast<size_t>(location.GetValue()) == index) {
            visitor(ref, dex_reg, stack_visitor);
            found = true;
          }
        }

        if (!found) {
          // If nothing found, report with -1.
          visitor(ref, -1, stack_visitor);
        }
      }

      void VisitStack(mirror::Object** ref, size_t stack_index, const StackVisitor* stack_visitor)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        const size_t stack_offset = stack_index * kFrameSlotSize;
        FindWithType(stack_offset,
                     DexRegisterLocation::Kind::kInStack,
                     ref,
                     stack_visitor);
      }

      void VisitRegister(mirror::Object** ref,
                         size_t register_index,
                         const StackVisitor* stack_visitor)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        FindWithType(register_index,
                     DexRegisterLocation::Kind::kInRegister,
                     ref,
                     stack_visitor);
      }

      size_t number_of_dex_registers;
      const CodeInfo& code_info;
      const CodeInfoEncoding& encoding;
      DexRegisterMap dex_register_map;
      RootVisitor& visitor;
    };
    VisitQuickFrameWithVregCallback<StackMapVRegInfo>();
  }

  // Visitor for when we visit a root.
  RootVisitor& visitor_;
};

class RootCallbackVisitor {
 public:
  RootCallbackVisitor(RootVisitor* visitor, uint32_t tid) : visitor_(visitor), tid_(tid) {}

  void operator()(mirror::Object** obj, size_t vreg, const StackVisitor* stack_visitor) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    visitor_->VisitRoot(obj, JavaFrameRootInfo(tid_, stack_visitor, vreg));
  }

 private:
  RootVisitor* const visitor_;
  const uint32_t tid_;
};

template <bool kPrecise>
void Thread::VisitRoots(RootVisitor* visitor) {
  const uint32_t thread_id = GetThreadId();
  visitor->VisitRootIfNonNull(&tlsPtr_.opeer, RootInfo(kRootThreadObject, thread_id));
  if (tlsPtr_.exception != nullptr && tlsPtr_.exception != GetDeoptimizationException()) {
    visitor->VisitRoot(reinterpret_cast<mirror::Object**>(&tlsPtr_.exception),
                       RootInfo(kRootNativeStack, thread_id));
  }
  visitor->VisitRootIfNonNull(&tlsPtr_.monitor_enter_object, RootInfo(kRootNativeStack, thread_id));
  tlsPtr_.jni_env->locals.VisitRoots(visitor, RootInfo(kRootJNILocal, thread_id));
  tlsPtr_.jni_env->monitors.VisitRoots(visitor, RootInfo(kRootJNIMonitor, thread_id));
  HandleScopeVisitRoots(visitor, thread_id);
  if (tlsPtr_.debug_invoke_req != nullptr) {
    tlsPtr_.debug_invoke_req->VisitRoots(visitor, RootInfo(kRootDebugger, thread_id));
  }
  // Visit roots for deoptimization.
  if (tlsPtr_.stacked_shadow_frame_record != nullptr) {
    RootCallbackVisitor visitor_to_callback(visitor, thread_id);
    ReferenceMapVisitor<RootCallbackVisitor, kPrecise> mapper(this, nullptr, visitor_to_callback);
    for (StackedShadowFrameRecord* record = tlsPtr_.stacked_shadow_frame_record;
         record != nullptr;
         record = record->GetLink()) {
      for (ShadowFrame* shadow_frame = record->GetShadowFrame();
           shadow_frame != nullptr;
           shadow_frame = shadow_frame->GetLink()) {
        mapper.VisitShadowFrame(shadow_frame);
      }
    }
  }
  for (DeoptimizationContextRecord* record = tlsPtr_.deoptimization_context_stack;
       record != nullptr;
       record = record->GetLink()) {
    if (record->IsReference()) {
      visitor->VisitRootIfNonNull(record->GetReturnValueAsGCRoot(),
                                  RootInfo(kRootThreadObject, thread_id));
    }
    visitor->VisitRootIfNonNull(record->GetPendingExceptionAsGCRoot(),
                                RootInfo(kRootThreadObject, thread_id));
  }
  if (tlsPtr_.frame_id_to_shadow_frame != nullptr) {
    RootCallbackVisitor visitor_to_callback(visitor, thread_id);
    ReferenceMapVisitor<RootCallbackVisitor, kPrecise> mapper(this, nullptr, visitor_to_callback);
    for (FrameIdToShadowFrame* record = tlsPtr_.frame_id_to_shadow_frame;
         record != nullptr;
         record = record->GetNext()) {
      mapper.VisitShadowFrame(record->GetShadowFrame());
    }
  }
  for (auto* verifier = tlsPtr_.method_verifier; verifier != nullptr; verifier = verifier->link_) {
    verifier->VisitRoots(visitor, RootInfo(kRootNativeStack, thread_id));
  }
  // Visit roots on this thread's stack
  Context* context = GetLongJumpContext();
  RootCallbackVisitor visitor_to_callback(visitor, thread_id);
  ReferenceMapVisitor<RootCallbackVisitor, kPrecise> mapper(this, context, visitor_to_callback);
  mapper.template WalkStack<StackVisitor::CountTransitions::kNo>(false);
  ReleaseLongJumpContext(context);
  for (instrumentation::InstrumentationStackFrame& frame : *GetInstrumentationStack()) {
    visitor->VisitRootIfNonNull(&frame.this_object_, RootInfo(kRootVMInternal, thread_id));
  }
}

void Thread::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  if ((flags & VisitRootFlags::kVisitRootFlagPrecise) != 0) {
    VisitRoots<true>(visitor);
  } else {
    VisitRoots<false>(visitor);
  }
}

class VerifyRootVisitor : public SingleRootVisitor {
 public:
  void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE REQUIRES_SHARED(Locks::mutator_lock_) {
    VerifyObject(root);
  }
};

void Thread::VerifyStackImpl() {
  VerifyRootVisitor visitor;
  std::unique_ptr<Context> context(Context::Create());
  RootCallbackVisitor visitor_to_callback(&visitor, GetThreadId());
  ReferenceMapVisitor<RootCallbackVisitor> mapper(this, context.get(), visitor_to_callback);
  mapper.WalkStack();
}

// Set the stack end to that to be used during a stack overflow
void Thread::SetStackEndForStackOverflow() {
  // During stack overflow we allow use of the full stack.
  if (tlsPtr_.stack_end == tlsPtr_.stack_begin) {
    // However, we seem to have already extended to use the full stack.
    LOG(ERROR) << "Need to increase kStackOverflowReservedBytes (currently "
               << GetStackOverflowReservedBytes(kRuntimeISA) << ")?";
    DumpStack(LOG_STREAM(ERROR));
    LOG(FATAL) << "Recursive stack overflow.";
  }

  tlsPtr_.stack_end = tlsPtr_.stack_begin;

  // Remove the stack overflow protection if is it set up.
  bool implicit_stack_check = !Runtime::Current()->ExplicitStackOverflowChecks();
  if (implicit_stack_check) {
    if (!UnprotectStack()) {
      LOG(ERROR) << "Unable to remove stack protection for stack overflow";
    }
  }
}

void Thread::SetTlab(uint8_t* start, uint8_t* end, uint8_t* limit) {
  DCHECK_LE(start, end);
  DCHECK_LE(end, limit);
  tlsPtr_.thread_local_start = start;
  tlsPtr_.thread_local_pos  = tlsPtr_.thread_local_start;
  tlsPtr_.thread_local_end = end;
  tlsPtr_.thread_local_limit = limit;
  tlsPtr_.thread_local_objects = 0;
}

bool Thread::HasTlab() const {
  bool has_tlab = tlsPtr_.thread_local_pos != nullptr;
  if (has_tlab) {
    DCHECK(tlsPtr_.thread_local_start != nullptr && tlsPtr_.thread_local_end != nullptr);
  } else {
    DCHECK(tlsPtr_.thread_local_start == nullptr && tlsPtr_.thread_local_end == nullptr);
  }
  return has_tlab;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  thread.ShortDump(os);
  return os;
}

bool Thread::ProtectStack(bool fatal_on_error) {
  void* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  VLOG(threads) << "Protecting stack at " << pregion;
  if (mprotect(pregion, kStackOverflowProtectedSize, PROT_NONE) == -1) {
    if (fatal_on_error) {
      LOG(FATAL) << "Unable to create protected region in stack for implicit overflow check. "
          "Reason: "
          << strerror(errno) << " size:  " << kStackOverflowProtectedSize;
    }
    return false;
  }
  return true;
}

bool Thread::UnprotectStack() {
  void* pregion = tlsPtr_.stack_begin - kStackOverflowProtectedSize;
  VLOG(threads) << "Unprotecting stack at " << pregion;
  return mprotect(pregion, kStackOverflowProtectedSize, PROT_READ|PROT_WRITE) == 0;
}

void Thread::ActivateSingleStepControl(SingleStepControl* ssc) {
  CHECK(Dbg::IsDebuggerActive());
  CHECK(GetSingleStepControl() == nullptr) << "Single step already active in thread " << *this;
  CHECK(ssc != nullptr);
  tlsPtr_.single_step_control = ssc;
}

void Thread::DeactivateSingleStepControl() {
  CHECK(Dbg::IsDebuggerActive());
  CHECK(GetSingleStepControl() != nullptr) << "Single step not active in thread " << *this;
  SingleStepControl* ssc = GetSingleStepControl();
  tlsPtr_.single_step_control = nullptr;
  delete ssc;
}

void Thread::SetDebugInvokeReq(DebugInvokeReq* req) {
  CHECK(Dbg::IsDebuggerActive());
  CHECK(GetInvokeReq() == nullptr) << "Debug invoke req already active in thread " << *this;
  CHECK(Thread::Current() != this) << "Debug invoke can't be dispatched by the thread itself";
  CHECK(req != nullptr);
  tlsPtr_.debug_invoke_req = req;
}

void Thread::ClearDebugInvokeReq() {
  CHECK(GetInvokeReq() != nullptr) << "Debug invoke req not active in thread " << *this;
  CHECK(Thread::Current() == this) << "Debug invoke must be finished by the thread itself";
  DebugInvokeReq* req = tlsPtr_.debug_invoke_req;
  tlsPtr_.debug_invoke_req = nullptr;
  delete req;
}

void Thread::PushVerifier(verifier::MethodVerifier* verifier) {
  verifier->link_ = tlsPtr_.method_verifier;
  tlsPtr_.method_verifier = verifier;
}

void Thread::PopVerifier(verifier::MethodVerifier* verifier) {
  CHECK_EQ(tlsPtr_.method_verifier, verifier);
  tlsPtr_.method_verifier = verifier->link_;
}

size_t Thread::NumberOfHeldMutexes() const {
  size_t count = 0;
  for (BaseMutex* mu : tlsPtr_.held_mutexes) {
    count += mu != nullptr ? 1 : 0;
  }
  return count;
}

void Thread::DeoptimizeWithDeoptimizationException(JValue* result) {
  DCHECK_EQ(GetException(), Thread::GetDeoptimizationException());
  ClearException();
  ShadowFrame* shadow_frame =
      PopStackedShadowFrame(StackedShadowFrameType::kDeoptimizationShadowFrame);
  ObjPtr<mirror::Throwable> pending_exception;
  bool from_code = false;
  PopDeoptimizationContext(result, &pending_exception, &from_code);
  SetTopOfStack(nullptr);
  SetTopOfShadowStack(shadow_frame);

  // Restore the exception that was pending before deoptimization then interpret the
  // deoptimized frames.
  if (pending_exception != nullptr) {
    SetException(pending_exception);
  }
  interpreter::EnterInterpreterFromDeoptimize(this, shadow_frame, from_code, result);
}

void Thread::SetException(ObjPtr<mirror::Throwable> new_exception) {
  CHECK(new_exception != nullptr);
  // TODO: DCHECK(!IsExceptionPending());
  tlsPtr_.exception = new_exception.Ptr();
}

bool Thread::IsAotCompiler() {
  return Runtime::Current()->IsAotCompiler();
}

mirror::Object* Thread::GetPeerFromOtherThread() const {
  DCHECK(tlsPtr_.jpeer == nullptr);
  mirror::Object* peer = tlsPtr_.opeer;
  if (kUseReadBarrier && Current()->GetIsGcMarking()) {
    // We may call Thread::Dump() in the middle of the CC thread flip and this thread's stack
    // may have not been flipped yet and peer may be a from-space (stale) ref. So explicitly
    // mark/forward it here.
    peer = art::ReadBarrier::Mark(peer);
  }
  return peer;
}

}  // namespace art
