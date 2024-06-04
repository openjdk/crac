#ifndef SHARE_UTILITIES_EXTENDABLE_ARRAY_HPP
#define SHARE_UTILITIES_EXTENDABLE_ARRAY_HPP

#include "memory/allocation.hpp"
#include "runtime/os.hpp"

// C-heap array which can be dynamically extended.
//
// Differs from a heap-allocated GrowableArray in that:
// 1. Size == capacity.
// 2. Accessible elements are allowed to be left uninitialized.
// 3. Index type can be specified via a template parameter.
template <class ElemT, class SizeT = size_t, MEMFLAGS F = mtInternal>
class ExtendableArray : public CHeapObj<F> {
 private:
  SizeT _size;
  ElemT *_mem = nullptr;

 public:
  explicit ExtendableArray(SizeT size) : _size(size) {
    guarantee(size >= 0, "Size cannot be negative");
    if (size == 0) {
      return;
    }
    _mem = static_cast<ElemT *>(os::malloc(size * sizeof(ElemT), mtInternal));
    if (mem() == nullptr) {
      vm_exit_out_of_memory(size * sizeof(ElemT), OOM_MALLOC_ERROR, "extendable array construction");
    }
  }

  ExtendableArray() : ExtendableArray(0){};
  ~ExtendableArray() { os::free(mem()); }
  NONCOPYABLE(ExtendableArray);

  SizeT size() const { return _size; }
  ElemT *mem() const { return _mem; }

  void extend(SizeT new_size) {
    precond(new_size >= size());
    if (new_size == size()) {
      return;
    }
    _mem = static_cast<ElemT *>(os::realloc(mem(), new_size * sizeof(ElemT), mtInternal));
    if (mem() == nullptr) {
      vm_exit_out_of_memory(new_size * sizeof(ElemT), OOM_MALLOC_ERROR, "extendable array extension");
    }
    _size = new_size;
  }

  ElemT &operator[](SizeT index) {
    precond(0 <= index && index < _size);
    precond(_mem != nullptr);
    return _mem[index];
  }

  const ElemT &operator[](SizeT index) const {
    precond(0 <= index && index < _size);
    precond(_mem != nullptr);
    return _mem[index];
  }
};

#endif // SHARE_UTILITIES_EXTENDABLE_ARRAY_HPP
