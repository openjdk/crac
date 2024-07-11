#ifndef SHARE_UTILITIES_BARRIER_HPP
#define SHARE_UTILITIES_BARRIER_HPP

#include "memory/allocation.hpp"
#include "runtime/semaphore.hpp"
#include "utilities/globalDefinitions.hpp"

// Allows an arbitrary amount of threads to wait for each other before
// proceeding past the barrier.
//
// The barrier is not reusable: once the specified number of threads have
// arrived it should not be used anymore.
//
// Before deleting the barrier make sure all threads have left its methods.
class Barrier : public CHeapObj<mtInternal> {
 public:
  explicit Barrier(uint num_threads) : _num_threads_total(num_threads) {};

  void arrive();

 private:
  const uint _num_threads_total;
  volatile uint _num_threads_ready = 0;
  Semaphore _sem{0};
};

#endif // SHARE_UTILITIES_BARRIER_HPP
