#include "precompiled.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/cracStackDumper.hpp"
#include "runtime/os.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/methodKind.hpp"

Method *StackTrace::Frame::resolve_method(const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &classes,
                                          const ParsedHeapDump::RecordTable<HeapDump::UTF8> &symbols, TRAPS) {
  if (method() != nullptr) {
    return method();
  }

  InstanceKlass *holder;
  {
    InstanceKlass **const c = classes.get(method_holder_id());
    guarantee(c != nullptr, "unknown class ID " SDID_FORMAT, method_holder_id());
    holder = InstanceKlass::cast(*c);
  }
  assert(holder->is_linked(), "trying to execute method of unlinked class");

  Symbol *name;
  {
    const HeapDump::UTF8 *r = symbols.get(method_name_id());
    guarantee(r != nullptr, "unknown method name ID " SDID_FORMAT, method_name_id());
    name = r->sym;
  }

  Symbol *sig;
  {
    const HeapDump::UTF8 *r = symbols.get(method_sig_id());
    guarantee(r != nullptr, "unknown method signature ID " SDID_FORMAT, method_sig_id());
    sig = r->sym;
  }


  Method *const method = CracClassDumpParser::find_method(holder, name, sig, method_kind(), true, CHECK_NULL);
  guarantee(method != nullptr, "method %s not found", Method::external_name(holder, name, sig));
  _resolved_method = method;

  return method;
}

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
  StackTracesParser(FileBasicTypeReader *reader, GrowableArrayCHeap<StackTrace *, mtInternal> *out, u2 word_size)
      : _reader(reader), _out(out), _word_size(word_size) {
    precond(_reader != nullptr && _out != nullptr && is_supported_word_size(_word_size));
  }

  const char *parse_stacks() {
    log_debug(crac, stacktrace, parser)("Parsing stack traces");

    while (true) {
      TracePreamble preamble;
      if (!parse_stack_preamble(&preamble)) {
        return ERR_INVAL_STACK_PREAMBLE;
      }
      if (preamble.finish) {
        break;
      }
      log_debug(crac, stacktrace, parser)("Parsing " UINT32_FORMAT " frame(s) of thread " SDID_FORMAT,
                                          preamble.frames_num, preamble.thread_id);

      auto *trace = new StackTrace(preamble.thread_id, preamble.frames_num);
      for (u4 i = 0; i < trace->frames_num(); i++) {
        log_trace(crac, stacktrace, parser)("Parsing frame " UINT32_FORMAT, i);
        if (!parse_frame(&trace->frame(i))) {
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
  GrowableArrayCHeap<StackTrace *, mtInternal> *_out;
  u2 _word_size;

  struct TracePreamble {
    bool finish;
    StackTrace::ID thread_id;
    u4 frames_num;
  };

  bool parse_stack_preamble(TracePreamble *preamble) {
    // Thread ID
    u1 buf[sizeof(StackTrace::ID)]; // Using _word_size as a size would cause error C2131 on MSVC
    // Read the first byte separately to detect a possible correct EOF
    if (!_reader->read_raw(buf, 1)) {
      if (_reader->eos()) {
        preamble->finish = true;
        return true;
      }
      log_error(crac, stacktrace, parser)("Failed to read thread ID");
      return false;
    }
    // Read the rest of the ID
    if (!_reader->read_raw(buf + 1, _word_size - 1)) {
      log_error(crac, stacktrace, parser)("Failed to read thread ID");
      return false;
    }
    // Convert to the ID type
    switch (_word_size) {
      case sizeof(u4): preamble->thread_id = Bytes::get_Java_u4(buf); break;
      case sizeof(u8): preamble->thread_id = Bytes::get_Java_u8(buf); break;
      default: ShouldNotReachHere();
    }

    // Number of frames dumped
    if (!_reader->read(&preamble->frames_num)) {
      log_error(crac, stacktrace, parser)("Failed to read number of frames in stack of thread " SDID_FORMAT,
                                          preamble->thread_id);
      return false;
    }

    preamble->finish = false;
    return true;
  }

  bool parse_method_kind(MethodKind::Enum *kind) {
    u1 raw_kind;
    if (!_reader->read(&raw_kind)) {
      log_error(crac, stacktrace, parser)("Failed to read method signature ID");
      return false;
    }
    if (!MethodKind::is_method_kind(raw_kind)) {
      log_error(crac, stacktrace, parser)("Unknown method kind: %i", raw_kind);
      return false;
    }
    *kind = static_cast<MethodKind::Enum>(raw_kind);
    return true;
  }

  bool parse_frame(StackTrace::Frame *frame) {
    HeapDump::ID method_name_id;
    if (!_reader->read_uint(&method_name_id, _word_size)) {
      log_error(crac, stacktrace, parser)("Failed to read method name ID");
      return false;
    }
    frame->set_method_name_id(method_name_id);

    HeapDump::ID method_sig_id;
    if (!_reader->read_uint(&method_sig_id, _word_size)) {
      log_error(crac, stacktrace, parser)("Failed to read method signature ID");
      return false;
    }
    frame->set_method_sig_id(method_sig_id);

    MethodKind::Enum method_kind;
    if (!parse_method_kind(&method_kind)) {
      return false;
    }
    frame->set_method_kind(method_kind);

    HeapDump::ID method_holder_id;
    if (!_reader->read_uint(&method_holder_id, _word_size)) {
      log_error(crac, stacktrace, parser)("Failed to read class ID");
      return false;
    }
    frame->set_method_holder_id(method_holder_id);

    u2 bci;
    if (!_reader->read(&bci)) {
      log_error(crac, stacktrace, parser)("Failed to read BCI");
      return false;
    }
    frame->set_bci(bci);

    log_trace(crac, stacktrace, parser)("Parsing locals");
    if (!parse_stack_values(&frame->locals())) {
      return false;
    }

    log_trace(crac, stacktrace, parser)("Parsing operands");
    if (!parse_stack_values(&frame->operands())) {
      return false;
    }

    log_trace(crac, stacktrace, parser)("Parsing monitors");
    if (!parse_monitors()) {
      return false;
    }

    return true;
  }

  bool parse_stack_values(ExtendableArray<StackTrace::Frame::Value, u2> *values) {
    u2 values_num;
    if (!_reader->read(&values_num)) {
      log_error(crac, stacktrace, parser)("Failed to read the number of values");
      return false;
    }
    values->extend(values_num);
    log_trace(crac, stacktrace, parser)("Parsing %i value(s)", values_num);

    for (u2 i = 0; i < values_num; i++) {
      u1 type;
      if (!_reader->read(&type)) {
        log_error(crac, stacktrace, parser)("Failed to read the type of value #%i", i);
        return false;
      }

      if (type == DumpedStackValueType::PRIMITIVE) {
        (*values)[i].type = static_cast<DumpedStackValueType>(type);
        if (!_reader->read_uint(&(*values)[i].prim, _word_size)) {
          log_error(crac, stacktrace, parser)("Failed to read value #%i as a primitive", i);
          return false;
        }
      } else if (type == DumpedStackValueType::REFERENCE) {
        (*values)[i].type = DumpedStackValueType::REFERENCE;
        if (!_reader->read_uint(&(*values)[i].obj_id, _word_size)) {
          log_error(crac, stacktrace, parser)("Failed to read value #%i as a reference", i);
          return false;
        }
      } else {
        log_error(crac, stacktrace, parser)("Unknown type of value #%i: " UINT8_FORMAT_X_0, i, type);
        return false;
      }
    }

    return true;
  }

  bool parse_monitors() {
    u2 monitors_num;
    if (!_reader->read(&monitors_num)) {
      log_error(crac, stacktrace, parser)("Failed to read the number of monitors");
      return false;
    }
    // TODO implement monitors parsing after the format is determined
    if (monitors_num > 0) {
      log_error(crac, stacktrace, parser)("Monitors parsing is not yet implemented");
      return false;
    }
    return true;
  }
};

static const char *parse_header(BasicTypeReader *reader, u2 *word_size) {
  constexpr char HEADER_STR[] = "CRAC STACK DUMP 0.1";

  char header_str[sizeof(HEADER_STR)];
  if (!reader->read_raw(header_str, sizeof(header_str))) {
    log_error(crac, stacktrace, parser)("Failed to read header string");
    return ERR_INVAL_HEADER_STR;
  }
  header_str[sizeof(header_str) - 1] = '\0'; // Ensure nul-terminated
  if (strcmp(header_str, HEADER_STR) != 0) {
    log_error(crac, stacktrace, parser)("Unknown header string: %s", header_str);
    return ERR_INVAL_HEADER_STR;
  }

  if (!reader->read(word_size)) {
    log_error(crac, stacktrace, parser)("Failed to read word size");
    return ERR_INVAL_ID_SIZE;
  }
  if (!is_supported_word_size(*word_size)) {
    log_error(crac, stacktrace, parser)("Word size %i is not supported: should be 4 or 8", *word_size);
    return ERR_UNSUPPORTED_ID_SIZE;
  }

  return nullptr;
}

const char *CracStackDumpParser::parse(const char *path, ParsedStackDump *out) {
  precond(path != nullptr);
  precond(out != nullptr);

  log_info(crac, stacktrace, parser)("Started parsing %s", path);

  FileBasicTypeReader reader;
  if (!reader.open(path)) {
    log_error(crac, stacktrace, parser)("Failed to open %s: %s", path, os::strerror(errno));
  }

  u2 word_size;
  const char *err_msg = parse_header(&reader, &word_size);
  if (err_msg != nullptr) {
    return err_msg;
  }
  log_debug(crac, stacktrace, parser)("Word size: %i", word_size);
  out->set_word_size(word_size);

  err_msg = StackTracesParser(&reader, &out->stack_traces(), word_size).parse_stacks();
  if (err_msg == nullptr) {
    log_info(crac, stacktrace, parser)("Successfully parsed %s", path);
  } else {
    log_info(crac, stacktrace, parser)("Position in %s after error: %zu", path, reader.pos());
  }
  return err_msg;
}
