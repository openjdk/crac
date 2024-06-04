#include "precompiled.hpp"
#include "runtime/atomic.hpp"
#include "utilities/barrier.hpp"

void Barrier::arrive() {
  // Increment the number of threads arrived
  Atomic::inc(&_num_threads_ready);

  assert(_num_threads_ready <= _num_threads_total,
         "too many threads arrived: " UINT32_FORMAT " > " UINT32_FORMAT,
         _num_threads_ready, _num_threads_total);

  if (_num_threads_ready == _num_threads_total) {
    // All threads have arrived, wake up the waiting ones.
    // Note: this code can be called by multiple threads, not only the last one,
    // so the semaphore counter can reach up to num_threads_total^2.
    _sem.signal(_num_threads_total);
  } else {
    // Wait for more threads to arrive
    _sem.wait();
  }
}
