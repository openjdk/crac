#ifndef SHARE_UTILITIES_BASICTYPEREADER_HPP
#define SHARE_UTILITIES_BASICTYPEREADER_HPP

#include "metaprogramming/enableIf.hpp"
#include "utilities/bytes.hpp"
#include "utilities/globalDefinitions.hpp"

// Abstarct class for reading Java-ordered (i.e. in big-endian) bytes as basic
// types.
class BasicTypeReader {
 public:
  // Reads size bytes into buf. Returns true on success.
  virtual bool read_raw(void *buf, size_t size) = 0;

  // Reads integral types (and boolean as a byte).
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

  // Skips size bytes. Returns true on success.
  virtual bool skip(size_t size) = 0;
  // Returns the current reading position.
  virtual size_t pos() const = 0;
  // Tells whether the end of stream has been reached.
  virtual bool eos() const = 0;

 protected:
  ~BasicTypeReader() = default;
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

  bool skip(size_t size) override;

  size_t pos() const override;

  bool eos() const override {
    precond(_file != nullptr);
    return feof(_file) != 0;
  }

 private:
  FILE *_file = nullptr;

  void close();
};

#endif // SHARE_UTILITIES_BASICTYPEREADER_HPP
