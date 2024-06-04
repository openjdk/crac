#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/bitCast.hpp"

bool BasicTypeReader::read(jfloat *out) {
  u4 tmp;
  if (!read(&tmp)) {
    return false;
  }
  *out = bit_cast<jfloat>(tmp);
  return true;
}

bool BasicTypeReader::read(jdouble *out) {
  u8 tmp;
  if (!read(&tmp)) {
    return false;
  }
  *out = bit_cast<jdouble>(tmp);
  return true;
}

bool BasicTypeReader::read_uint(u8 *out, size_t size) {
  precond(size == sizeof(u1) || size == sizeof(u2) || size == sizeof(u4) || size == sizeof(u8));
  switch (size) {
    case sizeof(u1): {
      u1 id;
      if (!read(&id)) {
        return false;
      }
      *out = id;
      return true;
    }
    case sizeof(u2): {
      u2 id;
      if (!read(&id)) {
        return false;
      }
      *out = id;
      return true;
    }
    case sizeof(u4): {
      u4 id;
      if (!read(&id)) {
        return false;
      }
      *out = id;
      return true;
    }
    case sizeof(u8): {
      u8 id;
      if (!read(&id)) {
        return false;
      }
      *out = id;
      return true;
    }
    default:
      ShouldNotReachHere();
      return false;
  }
}

bool FileBasicTypeReader::open(const char *path) {
  assert(path != nullptr, "cannot read from null path");

  close();
  errno = 0; // If close() errored, a warning has already been issued

  FILE *file = os::fopen(path, "rb");
  if (file == nullptr) {
    guarantee(errno != 0, "fopen should set errno on error");
    return false;
  }

  _file = file;
  return true;
}

void FileBasicTypeReader::close() {
  if (_file == nullptr) {
    return;
  }
  if (log_is_enabled(Warning, data)) {
    int fd = os::get_fileno(_file);
    errno = -1; // To get "Unknown error" from os::strerror() if fclose won't set errno
    if (fclose(_file) != 0) {
      if (fd != -1) {
        log_warning(data)("Failed to close file with FD %i after reading: %s", fd, os::strerror(errno));
      } else {
        log_warning(data)("Failed to close a file after reading: %s", os::strerror(errno));
      }
    }
  } else {
    fclose(_file);
  }
}

bool FileBasicTypeReader::skip(size_t size) {
    precond(_file != nullptr);
    while (size > LONG_MAX) {
      if (fseek(_file, LONG_MAX, SEEK_CUR) != 0) {
        return false;
      }
      size -= LONG_MAX;
    }
    return fseek(_file, checked_cast<long>(size), SEEK_CUR) == 0;
  }

size_t FileBasicTypeReader::pos() const {
  precond(_file != nullptr);
  long pos = ftell(_file);
  if (pos < 0) {
    log_warning(data)("Failed to get position in a file after reading: %s", os::strerror(errno));
    pos = 0;
  }
  return pos;
}
