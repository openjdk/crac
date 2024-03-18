#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "classfile/vmSymbols.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/symbol.hpp"
#include "runtime/crac.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/cracThreadRestorer.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.hpp"
#include "runtime/handles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/semaphore.hpp"
#include "runtime/signature.hpp"
#include "runtime/stackValue.hpp"
#include "runtime/stackValueCollection.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/vframeArray.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

uint CracThreadRestorer::_prepared_threads_num = 0;
Semaphore *CracThreadRestorer::_start_semaphore = nullptr;

static jlong log_tid(const JavaThread *thread) {
  return java_lang_Thread::thread_id(thread->threadObj());
}

void CracThreadRestorer::start_prepared_threads() {
  if (_prepared_threads_num == 0) {
    return; // No threads to start
  }
  assert(_start_semaphore != nullptr, "must be");
  _start_semaphore->signal(_prepared_threads_num);
  _prepared_threads_num = 0;
}

// Same jlong -> size_t conversion as JVM_StartThread performs.
static size_t get_stack_size(oop thread_obj) {
  const jlong raw_stack_size = java_lang_Thread::stackSize(thread_obj);
  if (raw_stack_size <= 0) {
    return 0;
  }
#ifndef  _LP64
  if (raw_stack_size > SIZE_MAX) {
    return SIZE_MAX;
  }
#endif // _LP64
  return checked_cast<size_t>(raw_stack_size);
}

void CracThreadRestorer::prepare_thread(CracStackTrace *stack, TRAPS) {
  if (_start_semaphore == nullptr) {
    assert(_prepared_threads_num == 0, "must be");
    _start_semaphore = new Semaphore(0);
  }

  const jobject thread_obj = stack->thread();

  // Prepare a JavaThread in the same fashion as JVM_StartThread does
  JavaThread *thread;
  {
    MutexLocker ml(Threads_lock);
    const size_t stack_size = get_stack_size(JNIHandles::resolve_non_null(thread_obj));
    thread = new JavaThread(&prepared_thread_entry, stack_size);
    if (thread->osthread() != nullptr) {
      HandleMark hm(JavaThread::current());
      thread->prepare(thread_obj);
    }
  }
  if (thread->osthread() == nullptr) {
    thread->smr_delete();
    THROW_MSG(vmSymbols::java_lang_OutOfMemoryError(), os::native_thread_creation_failed_msg());
  }

  _prepared_threads_num++;

  // Make the stack available to the restoration code (the thread now owns it)
  thread->set_crac_stack(stack);

  // The thread will wait for the start signal
  Thread::start(thread);
}

// Wait for the start signal from the creator thread before starting.
void CracThreadRestorer::prepared_thread_entry(JavaThread *current, TRAPS) {
  log_debug(crac)("Thread " JLONG_FORMAT ": waiting for start signal", log_tid(current));
  _start_semaphore->wait();
  restore_current_thread_impl(current, CHECK);
}

void CracThreadRestorer::restore_current_thread(CracStackTrace *stack, TRAPS) {
  JavaThread *const current = JavaThread::current();
  assert(JNIHandles::resolve_non_null(stack->thread()) == current->threadObj(), "wrong stack trace");
  current->set_crac_stack(stack); // Restoration code expects it there
  restore_current_thread_impl(current, CHECK);
}

// Make this second-youngest frame the youngest faking the result of the
// callee (i.e. the current youngest) frame.
static void transform_to_youngest(CracStackTrace::Frame *frame, Handle callee_result) {
  const Bytecodes::Code code = frame->method()->code_at(frame->bci());
  assert(Bytecodes::is_invoke(code), "non-youngest frames must be invoking, got %s", Bytecodes::name(code));

  // Push the result onto the operand stack
  if (callee_result.not_null()) {
    const auto operands_num = frame->operands().length();
    assert(operands_num < frame->method()->max_stack(), "cannot push return value: all %i slots taken",
           frame->method()->max_stack());
    frame->operands().reserve(operands_num + 1); // Not bare append because it may allocate more than one slot
    // FIXME append() creates a copy but accepts a reference so no copy elision can occur
    frame->operands().append({}); // Cheap empty->empty copy, empty->empty swap
    *frame->operands().adr_at(operands_num) = CracStackTrace::Frame::Value::of_obj(callee_result); // Cheap resolved->empty swap
  }

  // Increment the BCI past the invoke bytecode
  const int code_len = Bytecodes::length_for(code);
  assert(code_len > 0, "invoke codes don't need special length calculation");
  frame->set_bci(frame->bci() + code_len);
  assert(frame->method()->validate_bci(frame->bci()) >= 0, "transformed to invalid BCI %i", frame->bci());
}

// If the youngest frame represents special method requiring a fixup, applies
// the fixup.
static void fixup_youngest_frame_if_special(CracStackTrace *stack, TRAPS) {
  if (stack->frames_num() == 0) {
    return;
  }

  const Method &youngest_m = *stack->frame(stack->frames_num() - 1).method();
  if (!youngest_m.is_native()) { // Only native methods are special
    return;
  }
  const InstanceKlass &holder = *youngest_m.method_holder();

  if (holder.name() == vmSymbols::jdk_crac_Core() && holder.class_loader_data()->is_the_null_class_loader_data() &&
      youngest_m.name() == vmSymbols::checkpointRestore0_name()) {
    // Checkpoint initiation method: handled by imitating a successful return

    // Pop the native frame
    stack->pop();

    if (stack->frames_num() == 0) {
      return; // No Java caller (e.g. called from JNI)
    }

    // Create the return value indicating the successful restoration
    HandleMark hm(Thread::current()); // The handle will either become an oop or a JNI handle
    const Handle bundle_h = crac::cr_return(JVM_CHECKPOINT_OK, {}, {}, {}, {}, CHECK);

    // Push the return value onto the caller's operand stack and move to the next bytecode
    CracStackTrace::Frame &caller = stack->frame(stack->frames_num() - 1);
    transform_to_youngest(&caller, bundle_h);
  } else if (youngest_m.intrinsic_id() == vmIntrinsics::_park) {
    assert(holder.name() == vmSymbols::jdk_internal_misc_Unsafe() &&
           holder.class_loader_data()->is_the_null_class_loader_data() &&
           youngest_m.name() == vmSymbols::park_name(), "must be");
    // Unsafe.park(...): we use the fact that the method's specification allows
    // it to return spuriously, i.e. for no particular reason

    // Pop the native frame
    stack->pop();
    // Move to the next bytecode in the caller's frame
    CracStackTrace::Frame &caller = stack->frame(stack->frames_num() - 1);
    transform_to_youngest(&caller, Handle() /*don't place any return value*/);
  } else {
    log_error(crac)("Unknown native method encountered: %s", youngest_m.external_name());
    ShouldNotReachHere();
  }
}

// Fills the provided arguments with null-values according to the provided
// signature.
class NullArgumentsFiller : public SignatureIterator {
  friend class SignatureIterator;  // so do_parameters_on can call do_type
 public:
  NullArgumentsFiller(Symbol *signature, JavaCallArguments *args) : SignatureIterator(signature), _args(args) {
    do_parameters_on(this);
  }

 private:
  JavaCallArguments *_args;

  void do_type(BasicType type) {
    switch (type) {
      case T_BYTE:
      case T_BOOLEAN:
      case T_CHAR:
      case T_SHORT:
      case T_INT:    _args->push_int(0);        break;
      case T_FLOAT:  _args->push_float(0);      break;
      case T_LONG:   _args->push_long(0);       break;
      case T_DOUBLE: _args->push_double(0);     break;
      case T_ARRAY:
      case T_OBJECT: _args->push_oop(Handle()); break;
      default:       ShouldNotReachHere();
    }
  }
};

// Initiates thread restoration and won't return until the restored execution
// completes.
//
// The process of thread restoration is as follows:
// 1. This method is called to make a Java-call to the initial method (the
// oldest one in the stack) with the snapshotted arguments, replacing its entry
// point with an entry into assembly restoration code (RestoreBlob).
// 2. Java-call places a CallStub frame for the initial method and calls
// RestoreBlob.
// 3. RestoreBlob calls fetch_frame_info() which prepares restoration info based
// on the stack snapshot. This cannot be perfomed directly in step 1: a
// safepoint can occur on step 2 which the prepared data won't survive.
// 4. RestoreBlob reads the prepared restoration info and creates so-called
// skeletal frames which are walkable interpreter frames of proper sizes but
// with monitors, locals, expression stacks, etc. unfilled.
// 5. RestoreBlob calls fill_in_frames() which also reads the prepared
// restoration info and fills the skeletal frames.
// 6. RestoreBlob jumps into the interpreter to start executing the youngest
// restored stack frame.
void CracThreadRestorer::restore_current_thread_impl(JavaThread *current, TRAPS) {
  assert(current == JavaThread::current(), "must be");
  if (log_is_enabled(Info, crac)) {
    ResourceMark rm;
    log_info(crac)("Thread " JLONG_FORMAT " (%s): starting restoration", log_tid(current), current->name());
  }

  // Get the stack trace to restore
  CracStackTrace *stack = current->crac_stack();
  assert(stack != nullptr, "no stack to restore");

  // Check if there are special frames requiring fixup, this may pop some frames
  fixup_youngest_frame_if_special(stack, CHECK);

  // Early return if empty: stack restoration does not account for this corner case
  if (stack->frames_num() == 0) {
    log_info(crac)("Thread " JLONG_FORMAT ": no frames to restore", log_tid(current));
    delete stack;
    current->set_crac_stack(nullptr);
    return;
  }

  const CracStackTrace::Frame &oldest_frame = stack->frame(0);
  Method *const method = oldest_frame.method();

  JavaCallArguments args;
  // Need to set the receiver (if any): it will be read during the Java call
  if (!method->is_static()) {
    guarantee(oldest_frame.locals().is_nonempty(), "must have 'this' as the first local");
    const CracStackTrace::Frame::Value &receiver = oldest_frame.locals().first();
    args.set_receiver(Handle(current, JNIHandles::resolve_non_null(receiver.as_obj())));
  }
  // The actual values will be filled by the RestoreStub, we just need the Java
  // call code to allocate the right amount of space
  NullArgumentsFiller(method->signature(), &args);
  // Make the CallStub call RestoreStub instead of the actual method entry
  args.set_use_restore_stub(true);

  if (log_is_enabled(Info, crac)) {
    ResourceMark rm;
    log_debug(crac)("Thread " JLONG_FORMAT ": calling %s", log_tid(current), method->external_name());
  }
  JavaValue result(method->result_type());
  JavaCalls::call(&result, methodHandle(current, method), &args, CHECK);
  // The stack snapshot has been freed already by now

  log_info(crac)("Thread " JLONG_FORMAT ": restored execution completed", log_tid(current));
}

class vframeRestoreArrayElement : public vframeArrayElement {
 public:
  void fill_in(const CracStackTrace::Frame &snapshot, bool reexecute) {
    _method = snapshot.method();

    _bci = snapshot.bci();
    guarantee(_method->validate_bci(_bci) == _bci, "invalid bytecode index %i", _bci);

    _reexecute = reexecute;

    _locals = stack_values_from_frame(snapshot.locals());
    _expressions = stack_values_from_frame(snapshot.operands());

    // TODO add monitor info into the snapshot; for now assuming no monitors
    _monitors = nullptr;
    DEBUG_ONLY(_removed_monitors = false;)
  }

 private:
  static StackValueCollection *stack_values_from_frame(const GrowableArrayCHeap<CracStackTrace::Frame::Value, mtInternal> &src) {
    auto *const stack_values = new StackValueCollection(src.length()); // size == 0 until we actually add the values
    // Cannot use the array iterator as it creates copies and we cannot copy
    // resolved reference values in this scope (it requires a Handle allocation)
    for (int i = 0; i < src.length(); i++) {
      const auto &src_value = *src.adr_at(i);
      switch (src_value.type()) {
        // At checkpoint this was either a T_INT or a T_CONFLICT StackValue,
        // in the later case it should have been dumped as 0 for us
        case CracStackTrace::Frame::Value::Type::PRIM: {
          // We've checked that stack slot size of the dump equals ours (right
          // after parsing), so the cast is safe
          LP64_ONLY(const u8 val = src_value.as_primitive());  // Take the whole u8
          NOT_LP64(const u4 val = src_value.as_primitive());   // Take the low half
          const auto int_stack_slot = bit_cast<intptr_t>(val); // 4 or 8 byte slot depending on the platform
          stack_values->add(new StackValue(int_stack_slot));
          break;
        }
        // At checkpoint this was a T_OBJECT StackValue
        case CracStackTrace::Frame::Value::Type::OBJ: {
          const oop o = JNIHandles::resolve(src_value.as_obj()); // May be null
          // Unpacking code of vframeArrayElement expects a raw oop
          stack_values->add(new StackValue(cast_from_oop<intptr_t>(o), T_OBJECT));
          break;
        }
        default:
          ShouldNotReachHere();
      }
    }
    return stack_values;
  }
};

class vframeRestoreArray : public vframeArray {
 public:
  static vframeRestoreArray *allocate(const CracStackTrace &stack) {
    guarantee(stack.frames_num() <= INT_MAX, "stack trace of thread " SDID_FORMAT " is too long: " UINT32_FORMAT " > %i",
              stack.thread_id(), stack.frames_num(), INT_MAX);
    auto *const result = reinterpret_cast<vframeRestoreArray *>(AllocateHeap(sizeof(vframeRestoreArray) + // fixed part
                                                                             sizeof(vframeRestoreArrayElement) * (stack.frames_num() - 1), // variable part
                                                                             mtInternal));
    result->_frames = static_cast<int>(stack.frames_num());
    result->set_unroll_block(nullptr); // The actual value should be set by the caller later

    // We don't use these
    result->_owner_thread = nullptr; // Would have been JavaThread::current()
    result->_sender = frame();       // Will be the CallStub frame called before the restored frames
    result->_caller = frame();       // Seems to be the same as _sender
    result->_original = frame();     // Deoptimized frame which we don't have

    result->fill_in(stack);
    return result;
  }

 private:
  void fill_in(const CracStackTrace &stack) {
    _frame_size = 0; // Unused (no frame is being deoptimized)
    const JavaThread *current = log_is_enabled(Trace, crac) ? JavaThread::current() : nullptr;

    // vframeRestoreArray: the first frame is the youngest, the last is the oldest
    // CracStackTrace:     the first frame is the oldest, the last is the youngest
    log_trace(crac)("Thread " JLONG_FORMAT ": filling stack trace " SDID_FORMAT, log_tid(current), stack.thread_id());
    precond(frames() == checked_cast<int>(stack.frames_num()));
    for (int i = 0; i < frames(); i++) {
      log_trace(crac)("Thread " JLONG_FORMAT ": filling frame %i", log_tid(current), i);
      auto *const elem = static_cast<vframeRestoreArrayElement *>(element(i));
      // Note: youngest frame's BCI is always re-executed -- this is important
      // because otherwise deopt's unpacking code will try to use ToS caching
      // which we don't account for
      elem->fill_in(stack.frame(frames() - 1 - i), /*reexecute when youngest*/ i == 0);
      assert(!elem->method()->is_native(), "native methods are not restored");
    }
  }
};

// Called by RestoreBlob to get the info about the frames to restore. This is
// analogous to Deoptimization::fetch_unroll_info() except that we fetch the
// info from the stack snapshot instead of a deoptee frame. This is also a leaf
// (in contrast with fetch_unroll_info) since no reallocation is needed (see the
// comment before fetch_unroll_info).
JRT_LEAF(Deoptimization::UnrollBlock *, CracThreadRestorer::fetch_frame_info(JavaThread *current))
  precond(current == JavaThread::current());
  log_debug(crac)("Thread " JLONG_FORMAT ": fetching frame info", log_tid(current));

  // Heap-allocated resource mark to use resource-allocated StackValues
  // and free them before starting executing the restored code
  guarantee(current->deopt_mark() == nullptr, "No deopt should be pending");
  current->set_deopt_mark(new DeoptResourceMark(current));

  // Create vframe descriptions based on the stack snapshot -- no safepoint
  // should happen after this array is filled until we're done with it
  vframeRestoreArray *array;
  {
    const CracStackTrace *stack = current->crac_stack();
    assert(stack->frames_num() > 0, "should be checked when starting");

    array = vframeRestoreArray::allocate(*stack);
    postcond(array->frames() == static_cast<int>(stack->frames_num()));

    delete stack;
    current->set_crac_stack(nullptr);
  }
  postcond(array->frames() > 0);
  log_trace(crac)("Thread " JLONG_FORMAT ": filled frame array (%i frames)", log_tid(current), array->frames());

  // Determine sizes and return pcs of the constructed frames.
  //
  // The order of frames is the reverse of the array above:
  // frame_sizes and frame_pcs: 0th -- the oldest frame,   nth -- the youngest.
  // vframeRestoreArray *array: 0th -- the youngest frame, nth -- the oldest.
  auto *const frame_sizes = NEW_C_HEAP_ARRAY(intptr_t, array->frames(), mtInternal);
  // +1 because the last element is an address to jump into the interpreter
  auto *const frame_pcs = NEW_C_HEAP_ARRAY(address, array->frames() + 1, mtInternal);
  // Create an interpreter return address for the assembly code to use as its
  // return address so the skeletal frames are perfectly walkable
  frame_pcs[array->frames()] = Interpreter::deopt_entry(vtos, 0);

  // We start from the youngest frame, which has no callee
  int callee_params = 0;
  int callee_locals = 0;
  for (int i = 0; i < array->frames(); i++) {
    // Deopt code uses this to account for possible JVMTI's PopFrame function
    // usage which is irrelevant in our case
    static constexpr int popframe_extra_args = 0;

    // i == 0 is the youngest frame, i == array->frames() - 1 is the oldest
    frame_sizes[array->frames() - i - 1] =
        BytesPerWord * array->element(i)->on_stack_size(callee_params, callee_locals, i == 0, popframe_extra_args);

    frame_pcs[array->frames() - i - 1] = i < array->frames() - 1 ?
    // Setting the pcs the same way as the deopt code does. It is needed to
    // identify the skeleton frames as interpreted and make them walkable. The
    // correct pcs will be patched later when filling the frames.
                                         Interpreter::deopt_entry(vtos, 0) - frame::pc_return_offset :
    // The oldest frame always returns to CallStub
                                         StubRoutines::call_stub_return_address();

    callee_params = array->element(i)->method()->size_of_parameters();
    callee_locals = array->element(i)->method()->max_locals();
  }

  // Adjustment of the CallStub to accomodate the locals of the oldest restored
  // frame, if any
  const int caller_adjustment = Deoptimization::last_frame_adjust(callee_params, callee_locals);

  auto *const info = new Deoptimization::UnrollBlock(
    0,                           // Deoptimized frame size, unused (no frame is being deoptimized)
    caller_adjustment * BytesPerWord,
    0,                           // Amount of params in the CallStub frame, unused (known via the oldest frame's method)
    array->frames(),
    frame_sizes,
    frame_pcs,
    BasicType::T_ILLEGAL,        // Return type, unused (we are not in the process of returning a value)
    Deoptimization::Unpack_deopt // fill_in_frames() always specifies Unpack_deopt, regardless of what's set here
  );
  array->set_unroll_block(info);

  guarantee(current->vframe_array_head() == nullptr, "no deopt should be pending");
  current->set_vframe_array_head(array);

  log_debug(crac)("Thread " JLONG_FORMAT ": frame info fetched", log_tid(current));
  return info;
JRT_END

// Called by RestoreBlob after skeleton frames have been pushed on stack to fill
// them. This is analogous to Deoptimization::unpack_frames().
JRT_LEAF(void, CracThreadRestorer::fill_in_frames(JavaThread *current))
  precond(current == JavaThread::current());
  log_debug(crac)("Thread " JLONG_FORMAT ": filling skeletal frames", log_tid(current));

  // Reset NoHandleMark created by JRT_LEAF (see related comments in
  // Deoptimization::unpack_frames() on why this is ok). Handles are used e.g.
  // in trace printing.
  ResetNoHandleMark rnhm;
  HandleMark hm(current);

  // Array created by fetch_frame_info()
  vframeArray *const array = current->vframe_array_head();
  // Java frame between the skeleton frames and the frame of this function
  const frame unpack_frame = current->last_frame();
  // Amount of parameters in the CallStub frame = amount of parameters of the
  // oldest skeleton frame
  const int initial_caller_parameters = array->element(array->frames() - 1)->method()->size_of_parameters();

  // TODO save, clear, restore last Java sp like the deopt code does?

  assert(current->deopt_compiled_method() == nullptr, "no method is being deoptimized");
  guarantee(current->frames_to_pop_failed_realloc() == 0,
            "we don't deoptimize, so no reallocations of scalar replaced objects can happen and fail");
  array->unpack_to_stack(unpack_frame, Deoptimization::Unpack_deopt /* TODO this or reexecute? */, initial_caller_parameters);
  log_debug(crac)("Thread " JLONG_FORMAT ": skeletal frames filled", log_tid(current));

  // Cleanup, analogous to Deoptimization::cleanup_deopt_info()
  current->set_vframe_array_head(nullptr);
  delete array->unroll_block(); // Also deletes frame_sizes and frame_pcs
  delete array;
  delete current->deopt_mark();
  current->set_deopt_mark(nullptr);

  // TODO more verifications, like the ones Deoptimization::unpack_frames() does
  DEBUG_ONLY(current->validate_frame_layout();)
JRT_END
