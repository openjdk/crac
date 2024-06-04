#ifndef SHARE_UTILITIES_BASICTYPEWRITER_HPP
#define SHARE_UTILITIES_BASICTYPEWRITER_HPP

#include "metaprogramming/enableIf.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/bytes.hpp"
#include "utilities/debug.hpp"
#include <type_traits>

class BasicTypeWriter {
 public:
  // Writes size bytes into buf. Returns true on success.
  virtual bool write_raw(const void *buf, size_t size) = 0;

  // Writes integral types (and boolean as a byte).
  template <class T, ENABLE_IF(std::is_integral<T>::value &&
                               (sizeof(T) == sizeof(u1) || sizeof(T) == sizeof(u2) ||
                                sizeof(T) == sizeof(u4) || sizeof(T) == sizeof(u8)))>
  bool write(T value) {
    T tmp;
    switch (sizeof(value)) {
      case sizeof(u1): tmp = value;                                                break;
      case sizeof(u2): Bytes::put_Java_u2(reinterpret_cast<address>(&tmp), value); break;
      case sizeof(u4): Bytes::put_Java_u4(reinterpret_cast<address>(&tmp), value); break;
      case sizeof(u8): Bytes::put_Java_u8(reinterpret_cast<address>(&tmp), value); break;
    }
    return write_raw(&tmp, sizeof(tmp));
  }

  bool write(jfloat value)  { return write(bit_cast<u4>(value)); }
  bool write(jdouble value) { return write(bit_cast<u8>(value)); }

 protected:
  ~BasicTypeWriter() = default;
};

// Writes into a binary file.
class FileBasicTypeWriter : public BasicTypeWriter {
 public:
  ~FileBasicTypeWriter() { close(); };

  bool open(const char *path, bool overwrite = false);

  bool write_raw(const void *buf, size_t size) override {
    precond(_file != nullptr);
    precond(buf != nullptr || size == 0);
    return size == 0 || fwrite(buf, size, 1, _file) == 1;
  }

 private:
  FILE *_file = nullptr;

  void close();
};

#endif // SHARE_UTILITIES_BASICTYPEWRITER_HPP
