#ifndef SHARE_RUNTIME_CRACTHREADRESTORER_HPP
#define SHARE_RUNTIME_CRACTHREADRESTORER_HPP

#include "memory/allStatic.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/exceptions.hpp"

class Barrier;

// Thread restoration for CRaC's portable mode.
//
// It is intended to be used as follows:
// 1. A pre-existing Java thread (typically the main thread) initiates
//    restoration of other threads.
// 2. The pre-existing thread restores its own execution.
//
// After that the restorer cannot be reused.
class CracThreadRestorer : public AllStatic {
 public:
  // Prepares this class to restore the specified amount of threads. The threads
  // will wait for all of them to get restored before starting executing Java.
  static void prepare(uint num_threads);
  // Creates a new JavaThread and asynchronously restores the provided stack.
  static void restore_on_new_thread(CracStackTrace *stack, TRAPS);
  // Synchronously restores the provided stack on the current thread.
  static void restore_on_current_thread(CracStackTrace *stack, TRAPS);

  // Called by RestoreStub to prepare information about frames to restore.
  static Deoptimization::UnrollBlock *fetch_frame_info(JavaThread *current);
  // Called by RestoreStub to fill in the skeletal frames just created.
  static void fill_in_frames(JavaThread *current);

 private:
  // TODO delete the barrier after the threads have been restored.
  static Barrier *_start_barrier;

  static void restore_current_thread_impl(JavaThread *current, TRAPS);
};

#endif // SHARE_RUNTIME_CRACTHREADRESTORER_HPP
