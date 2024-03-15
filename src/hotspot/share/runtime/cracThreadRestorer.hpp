#ifndef SHARE_RUNTIME_CRACTHREADRESTORER_HPP
#define SHARE_RUNTIME_CRACTHREADRESTORER_HPP

#include "memory/allStatic.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/semaphore.hpp"
#include "utilities/exceptions.hpp"

// Thread restoration for CRaC's portable mode.
//
// It is intended to be used as follows:
// 1. A pre-existing Java thread (typically the main thread) initiates
//    restoration of other threads.
// 2. The pre-existing thread restores its own execution.
class CracThreadRestorer : public AllStatic {
 public:
  // Creates a new JavaThread and prepares it to restore its saved execution.
  static void prepare_thread(CracStackTrace *stack, TRAPS);
  // Starts execution of all threads that have been prepared.
  static void start_prepared_threads();

  // Restores the provided execution on the current thread.
  static void restore_current_thread(CracStackTrace *stack, TRAPS);

  // Called by RestoreStub to prepare information about frames to restore.
  static Deoptimization::UnrollBlock *fetch_frame_info(JavaThread *current);
  // Called by RestoreStub to fill in the skeletal frames just created.
  static void fill_in_frames(JavaThread *current);

 private:
  static uint _prepared_threads_num;
  static Semaphore *_start_semaphore;

  static void prepared_thread_entry(JavaThread *current, TRAPS);
  static void restore_current_thread_impl(JavaThread *current, TRAPS);
};

#endif // SHARE_RUNTIME_CRACTHREADRESTORER_HPP
