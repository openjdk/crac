#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "utilities/basicTypeWriter.hpp"

bool FileBasicTypeWriter::open(const char *path, bool overwrite) {
  assert(path != nullptr, "cannot write to null path");

  close();
  errno = 0; // If close() errored, a warning has already been issued

  if (!overwrite && os::file_exists(path)) {
    errno = EEXIST;
    return false;
  }
  errno = 0; // os::file_exists() may use functions that set errno

  FILE *file = os::fopen(path, "wb");
  if (file == nullptr) {
    guarantee(errno != 0, "fopen should set errno on error");
    return false;
  }

  _file = file;
  return true;
}

void FileBasicTypeWriter::close() {
  if (_file == nullptr) {
    return;
  }
  if (log_is_enabled(Warning, data)) {
    int fd = os::get_fileno(_file);
    errno = -1; // To get "Unknown error" from os::strerror() if fclose won't set errno
    if (fclose(_file) != 0) {
      if (fd != -1) {
        log_warning(data)("Failed to close file with FD %i after writing: %s", fd, os::strerror(errno));
      } else {
        log_warning(data)("Failed to close a file after writing: %s", os::strerror(errno));
      }
    }
  } else {
    fclose(_file);
  }
}
