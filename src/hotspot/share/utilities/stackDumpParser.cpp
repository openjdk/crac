#include "precompiled.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/bytes.hpp"
#include "utilities/debug.hpp"
#include "utilities/stackDumpParser.hpp"
#include "utilities/stackDumper.hpp"

// Header parsing errors
constexpr char ERR_INVAL_HEADER_STR[] = "invalid header string";
constexpr char ERR_INVAL_ID_SIZE[] = "invalid ID size format";
constexpr char ERR_UNSUPPORTED_ID_SIZE[] = "unsupported ID size";
// Stack trace parsing errors
constexpr char ERR_INVAL_STACK_PREAMBLE[] = "invalid stack trace preamble";
constexpr char ERR_INVAL_FRAME[] = "invalid frame contents";

static constexpr bool is_supported_word_size(u2 size) {
  return size == sizeof(u8) || size == sizeof(u4);
}

class StackTracesParser : public StackObj {
 public:
  StackTracesParser(FileBasicTypeReader *reader, GrowableArray<StackTrace *> *out, u2 word_size)
      : _reader(reader), _out(out), _word_size(word_size) {
    precond(_reader != nullptr && _out != nullptr && is_supported_word_size(_word_size));
  }

  const char *parse_stacks() {
    log_debug(stackdumpparsing)("Parsing stack traces");

    while (true) {
      TracePreamble preamble;
      if (!parse_stack_preamble(&preamble)) {
        return ERR_INVAL_STACK_PREAMBLE;
      }
      if (preamble.finish) {
        break;
      }
      log_debug(stackdumpparsing)("Parsing " UINT32_FORMAT " frame(s) of thread " UINT64_FORMAT,
                                  preamble.frames_num, preamble.thread_id);

      auto *trace = new StackTrace(preamble.thread_id, preamble.frames_num);
      for (u4 i = 0; i < trace->frames_num(); i++) {
        log_trace(stackdumpparsing)("Parsing frame " UINT32_FORMAT, i);
        if (!parse_frame(&trace->frames(i))) {
          delete trace;
          return ERR_INVAL_FRAME;
        }
      }
      _out->append(trace);
    }

    _out->shrink_to_fit();

    return nullptr;
  }

 private:
  FileBasicTypeReader *_reader;
  GrowableArray<StackTrace *> *_out;
  u2 _word_size;

  struct TracePreamble {
    bool finish;
    StackTrace::ID thread_id;
    u4 frames_num;
  };

  bool parse_stack_preamble(TracePreamble *preamble) {
    // Parse thread ID
    u1 buf[sizeof(StackTrace::ID)]; // Using _word_size causes error C2131 on MSVC
    // Read the first byte separately to detect a possible correct EOF
    if (!_reader->read_raw(buf, 1)) {
      if (_reader->eof()) {
        preamble->finish = true;
        return true;
      }
      log_error(stackdumpparsing)("Failed to read thread ID");
      return false;
    }
    // Read the rest of the ID
    if (!_reader->read_raw(buf + 1, _word_size - 1)) {
      log_error(stackdumpparsing)("Failed to read thread ID");
      return false;
    }
    // Convert to the ID type
    switch (_word_size) {
      case sizeof(u4): preamble->thread_id = Bytes::get_Java_u4(buf); break;
      case sizeof(u8): preamble->thread_id = Bytes::get_Java_u8(buf); break;
      default: ShouldNotReachHere();
    }

    // Parse the number of frames dumped
    if (!_reader->read(&preamble->frames_num)) {
      log_error(stackdumpparsing)("Failed to read number of frames in stack of thread " UINT64_FORMAT,
                                  preamble->thread_id);
      return false;
    }

    preamble->finish = false;
    return true;
  }

  bool parse_frame(StackTrace::Frame *frame) {
    if (!_reader->read_uint(&frame->method_name_id, _word_size)) {
      log_error(stackdumpparsing)("Failed to read method name ID");
      return false;
    }
    if (!_reader->read_uint(&frame->method_sig_id, _word_size)) {
      log_error(stackdumpparsing)("Failed to read method signature ID");
      return false;
    }
    if (!_reader->read_uint(&frame->class_id, _word_size)) {
      log_error(stackdumpparsing)("Failed to read class ID");
      return false;
    }
    if (!_reader->read(&frame->bci)) {
      log_error(stackdumpparsing)("Failed to read BCI");
      return false;
    }
    log_trace(stackdumpparsing)("Parsing locals");
    if (!parse_stack_values(&frame->locals)) {
      return false;
    }
    log_trace(stackdumpparsing)("Parsing operands");
    if (!parse_stack_values(&frame->operands)) {
      return false;
    }
    log_trace(stackdumpparsing)("Parsing monitors");
    if (!parse_monitors()) {
      return false;
    }
    return true;
  }

  bool parse_stack_values(ExtendableArray<StackTrace::Frame::Value, u2> *values) {
    u2 values_num;
    if (!_reader->read(&values_num)) {
      log_error(stackdumpparsing)("Failed to read the number of values");
      return false;
    }
    values->extend(values_num);
    log_trace(stackdumpparsing)("Parsing %i value(s)", values_num);

    for (u2 i = 0; i < values_num; i++) {
      u1 type;
      if (!_reader->read(&type)) {
        log_error(stackdumpparsing)("Failed to read the type of value #%i", i);
        return false;
      }

      if (type == DumpedStackValueType::PRIMITIVE) {
        (*values)[i].type = static_cast<DumpedStackValueType>(type);
        if (!_reader->read_uint(&(*values)[i].prim, _word_size)) {
          log_error(stackdumpparsing)("Failed to read value #%i as a primitive", i);
          return false;
        }
      } else if (type == DumpedStackValueType::REFERENCE) {
        (*values)[i].type = DumpedStackValueType::REFERENCE;
        if (!_reader->read_uint(&(*values)[i].obj_id, _word_size)) {
          log_error(stackdumpparsing)("Failed to read value #%i as a reference", i);
          return false;
        }
      } else {
        log_error(stackdumpparsing)("Unknown type of value #%i: " UINT8_FORMAT_X_0, i, type);
        return false;
      }
    }

    return true;
  }

  bool parse_monitors() {
    u2 monitors_num;
    if (!_reader->read(&monitors_num)) {
      log_error(stackdumpparsing)("Failed to read the number of monitors");
      return false;
    }
    // TODO implement monitors parsing after the format is determined
    if (monitors_num > 0) {
      log_error(stackdumpparsing)("Monitors parsing is not yet implemented");
      return false;
    }
    return true;
  }
};

static const char *parse_header(BasicTypeReader *reader, u2 *word_size) {
  constexpr char HEADER_STR[] = "JAVA STACK DUMP 0.1";

  char header_str[sizeof(HEADER_STR)];
  if (!reader->read_raw(header_str, sizeof(header_str))) {
    log_error(stackdumpparsing)("Failed to read header string");
    return ERR_INVAL_HEADER_STR;
  }
  header_str[sizeof(header_str) - 1] = '\0'; // Ensure nul-terminated
  if (strcmp(header_str, HEADER_STR) != 0) {
    log_error(stackdumpparsing)("Unknown header string: %s", header_str);
    return ERR_INVAL_HEADER_STR;
  }

  if (!reader->read(word_size)) {
    log_error(stackdumpparsing)("Failed to read ID size");
    return ERR_INVAL_ID_SIZE;
  }
  if (!is_supported_word_size(*word_size)) {
    log_error(stackdumpparsing)("Word size %i is not supported: should be 4 or 8", *word_size);
    return ERR_UNSUPPORTED_ID_SIZE;
  }

  return nullptr;
}

const char *StackDumpParser::parse(const char *path, ParsedStackDump *out) {
  guarantee(path != nullptr, "cannot parse from null path");
  guarantee(out != nullptr, "cannot save results into null container");

  log_info(stackdumpparsing)("Started parsing %s", path);

  FileBasicTypeReader reader;
  if (!reader.open(path)) {
    log_error(stackdumpparsing)("Failed to open %s: %s", path, os::strerror(errno));
  }

  u2 word_size;
  const char *err_msg = parse_header(&reader, &word_size);
  if (err_msg != nullptr) {
    return err_msg;
  }
  log_debug(stackdumpparsing)("Word size: %i", word_size);
  out->set_word_size(word_size);

  err_msg = StackTracesParser(&reader, &out->stack_traces(), word_size).parse_stacks();
  if (err_msg == nullptr) {
    log_info(stackdumpparsing)("Successfully parsed %s", path);
  } else {
    log_info(stackdumpparsing)("Position in %s after error: %li", path, reader.pos());
  }
  return err_msg;
}
