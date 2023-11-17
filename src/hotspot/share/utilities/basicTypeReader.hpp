#ifndef SHARE_UTILITIES_FILE_READER_HPP
#define SHARE_UTILITIES_FILE_READER_HPP

#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/bytes.hpp"
#include "utilities/globalDefinitions.hpp"

// Abstarct class for reading Java-ordered (i.e. in big-endian) bytes as basic
// types.
class BasicTypeReader : public StackObj {
 public:
  // Reads size bytes into buf. Returns true on success.
  virtual bool read_raw(void *buf, size_t size) = 0;

  // Reads integral types (and boolean).
  template <class T, ENABLE_IF(std::is_integral<T>::value && (sizeof(T) == sizeof(u1) ||
                                                              sizeof(T) == sizeof(u2) ||
                                                              sizeof(T) == sizeof(u4) ||
                                                              sizeof(T) == sizeof(u8)))>
  bool read(T *out) {
    if (!read_raw(out, sizeof(T))) {
      return false;
    }
    switch (sizeof(T)) {
      case sizeof(u1): break;
      case sizeof(u2): *out = Bytes::get_Java_u2(static_cast<address>(static_cast<void *>(out))); break;
      case sizeof(u4): *out = Bytes::get_Java_u4(static_cast<address>(static_cast<void *>(out))); break;
      case sizeof(u8): *out = Bytes::get_Java_u8(static_cast<address>(static_cast<void *>(out))); break;
    }
    return true;
  }

  bool read(jfloat *out);
  bool read(jdouble *out);

  // Reads either u1, u2, u4, or u8 into out based on the provided size.
  bool read_uint(u8 *out, size_t size);
};

// Reads from a binary file.
class FileBasicTypeReader : public BasicTypeReader {
 public:
  ~FileBasicTypeReader() { close(); }

  bool open(const char *path);

  bool read_raw(void *buf, size_t size) override {
    precond(_file != nullptr);
    precond(buf != nullptr || size == 0);
    return size == 0 || fread(buf, size, 1, _file) == 1;
  }

  bool skip(long size) {
    precond(_file != nullptr);
    return fseek(_file, size, SEEK_CUR) == 0;
  }

  bool eof() const {
    precond(_file != nullptr);
    return feof(_file) != 0;
  }

  long pos() const {
    precond(_file != nullptr);
    return ftell(_file);
  }

 private:
  FILE *_file = nullptr;

  void close();
};

#endif // SHARE_UTILITIES_FILE_READER_HPP
