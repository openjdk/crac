#include "precompiled.hpp"
#include "classfile/symbolTable.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/timerTrace.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/debug.hpp"
#include "utilities/extendableArray.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/hprofTag.hpp"

constexpr HeapDump::ID HeapDump::NULL_ID;

constexpr char ERR_INVAL_HEADER_STR[] = "invalid header string";
constexpr char ERR_INVAL_ID_SIZE[] = "invalid ID size format";
constexpr char ERR_UNSUPPORTED_ID_SIZE[] = "unsupported ID size";
constexpr char ERR_INVAL_DUMP_TIMESTAMP[] = "invalid dump timestamp format";

constexpr char ERR_INVAL_RECORD_PREAMBLE[] = "invalid (sub-)record preamble";
constexpr char ERR_INVAL_RECORD_BODY[] = "invalid (sub-)record body";
constexpr char ERR_INVAL_RECORD_TAG_POS[] = "illegal position of a (sub-)record tag";
constexpr char ERR_UNKNOWN_RECORD_TAG[] = "unknown (sub-)record tag";

constexpr char ERR_REPEATED_ID[] = "found a repeated ID";

// For logging.
const char *hprof_version2str(HeapDump::Version version) {
  switch (version) {
    case HeapDump::Version::V102:    return "v1.0.2";
    case HeapDump::Version::V101:    return "v1.0.1";
    case HeapDump::Version::UNKNOWN: return "<unknown version>";
    default: ShouldNotReachHere();   return nullptr;
  }
}

static constexpr bool is_supported_id_size(u4 size) {
  return size == sizeof(u8) || size == sizeof(u4) || size == sizeof(u2) || size == sizeof(u1);
}

class RecordsParser : public StackObj {
 public:
  RecordsParser(FileBasicTypeReader *reader, ParsedHeapDump *out, HeapDump::Version version, u4 id_size)
      : _reader(reader), _out(out), _version(version), _id_size(id_size) {
    precond(_reader != nullptr && _out != nullptr && _version != HeapDump::Version::UNKNOWN && is_supported_id_size(_id_size));
  }

  const char *parse_records() {
    State state;
    const char *err_msg = nullptr;

    log_debug(heapdump, parser)("Parsing records");

    while (!_reader->eos() && err_msg == nullptr) {
      switch (state.position()) {
        case State::Position::TOPLEVEL:
          err_msg = step_toplevel(&state);
          break;
        case State::Position::AMONG_HEAP_DUMP_SEGMENTS:
          err_msg = step_heap_segments(&state);
          break;
        case State::Position::IN_HEAP_DUMP_SEGMENT:
          precond(_version >= HeapDump::Version::V102);
        case State::Position::IN_HEAP_DUMP:
          err_msg = step_heap_dump(&state);
      }
    }

    return err_msg;
  }

 private:
  FileBasicTypeReader *_reader;
  ParsedHeapDump *_out;

  HeapDump::Version _version;
  u4 _id_size;

  ExtendableArray<char, u4> _sym_buf{1 * M};

  // Monitors parsing state and correctness of its transitions.
  class State {
   public:
    enum class Position {
      // Parsing top-level records.
      TOPLEVEL,
      // Parsing HPROF_HEAP_DUMP subrecords.
      IN_HEAP_DUMP,
      // Parsing HPROF_HEAP_DUMP_SEGMENT subrecords.
      IN_HEAP_DUMP_SEGMENT,
      // Just finished parsing a HPROF_HEAP_DUMP_SEGMENT.
      AMONG_HEAP_DUMP_SEGMENTS
    };

    // Where the parser currently is.
    Position position() const { return _position; }

    // When found a HPROF_HEAP_DUMP.
    bool enter_heap_dump(u4 size) {
      if (position() != Position::TOPLEVEL) {
        log_error(heapdump, parser)("Illegal position transition: %s -> %s",
                                   pos2str(position()), pos2str(Position::IN_HEAP_DUMP));
        return false;
      }
      precond(_remaining_record_size == 0);

      if (size > 0) {
        log_debug(heapdump, parser)("Position transition: %s -> %s (size " UINT32_FORMAT ")",
                                   pos2str(position()), pos2str(Position::IN_HEAP_DUMP), size);
        _position = Position::IN_HEAP_DUMP;
        _remaining_record_size = size;
      } else {
        log_debug(heapdump, parser)("Got HPROF_HEAP_DUMP of size 0 -- no position transition");
      }

      return true;
    }

    // When found a HPROF_HEAP_DUMP_SEGMENT.
    bool enter_heap_dump_segment(u4 size) {
      if (position() != Position::AMONG_HEAP_DUMP_SEGMENTS && position() != Position::TOPLEVEL) {
        log_error(heapdump, parser)("Illegal position transition: %s -> %s",
                                   pos2str(position()), pos2str(Position::IN_HEAP_DUMP_SEGMENT));
        return false;
      }
      precond(_remaining_record_size == 0);

      if (size > 0) {
        log_debug(heapdump, parser)("Position transition: %s -> %s (size " UINT32_FORMAT ")",
                                   pos2str(position()), pos2str(Position::IN_HEAP_DUMP_SEGMENT), size);
        _position = Position::IN_HEAP_DUMP_SEGMENT;
        _remaining_record_size = size;
      } else {
        log_debug(heapdump, parser)("Got HPROF_HEAP_DUMP_SEGMENT of size 0 -- position transition: %s -> %s",
                                   pos2str(position()), pos2str(Position::AMONG_HEAP_DUMP_SEGMENTS));
        _position = Position::AMONG_HEAP_DUMP_SEGMENTS;
      }

      return true;
    }

    // When found a HPROF_HEAP_DUMP_END.
    bool exit_heap_dump_segments() {
      // Allow top-level position for sequences of zero segments
      if (position() != Position::AMONG_HEAP_DUMP_SEGMENTS && position() != Position::TOPLEVEL) {
        log_error(heapdump, parser)("Illegal position transition: %s -> %s", pos2str(position()), pos2str(Position::TOPLEVEL));
        return false;
      }
      assert(_remaining_record_size == 0, "must be 0 outside a record");
      log_debug(heapdump, parser)("Position transition: %s -> %s", pos2str(position()), pos2str(Position::TOPLEVEL));
      _position = Position::TOPLEVEL;
      return true;
    }

    // When parsed the specified portion of the current record.
    bool reduce_remaining_record_size(u4 amount) {
      assert(position() != Position::TOPLEVEL && position() != Position::AMONG_HEAP_DUMP_SEGMENTS, "must be inside a record");
      assert(_remaining_record_size > 0, "must be > 0 inside a record");

      if (_remaining_record_size < amount) {
        log_error(heapdump, parser)("Tried to read " UINT32_FORMAT " bytes "
                                   "from a subrecord with " UINT32_FORMAT " bytes left",
                                   amount, _remaining_record_size);
        return false;
      }

      _remaining_record_size -= amount;

      if (_remaining_record_size == 0) {
        if (position() == Position::IN_HEAP_DUMP) {
          log_debug(heapdump, parser)("Position transition: %s -> %s", pos2str(position()), pos2str(Position::TOPLEVEL));
          _position = Position::TOPLEVEL;
        } else if (position() == Position::IN_HEAP_DUMP_SEGMENT) {
          log_debug(heapdump, parser)("Position transition: %s -> %s", pos2str(position()), pos2str(Position::AMONG_HEAP_DUMP_SEGMENTS));
          _position = Position::AMONG_HEAP_DUMP_SEGMENTS;
        } else {
          ShouldNotReachHere(); // We should be inside a record
        }
      }

      return true;
    }

   private:
    Position _position = Position::TOPLEVEL;
    u4 _remaining_record_size = 0;

    // For logging.
    static const char *pos2str(Position position) {
      switch (position) {
        case Position::TOPLEVEL:                 return "TOPLEVEL";
        case Position::IN_HEAP_DUMP:             return "IN_HEAP_DUMP";
        case Position::IN_HEAP_DUMP_SEGMENT:     return "IN_HEAP_DUMP_SEGMENT";
        case Position::AMONG_HEAP_DUMP_SEGMENTS: return "AMONG_HEAP_DUMP_SEGMENTS";
        default: ShouldNotReachHere();           return nullptr;
      }
    }
  };

  // High-level parsing

  const char *step_toplevel(State *state) {
    precond(state->position() == State::Position::TOPLEVEL);

    RecordPreamble preamble;
    if (!parse_record_preamble(&preamble)) {
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    if (preamble.finish) {
      return nullptr;
    }
    log_trace(heapdump, parser)("Record (toplevel): tag " UINT8_FORMAT_X_0 ", size " UINT32_FORMAT,
                               preamble.tag, preamble.body_size);

    Result body_res = Result::OK;
    switch (preamble.tag) {
      case HPROF_UTF8:
        body_res = parse_UTF8(preamble.body_size, &_out->utf8s);
        break;
      case HPROF_LOAD_CLASS:
        body_res = parse_load_class(preamble.body_size, &_out->load_classes);
        break;
      case HPROF_HEAP_DUMP:
        if (!state->enter_heap_dump(preamble.body_size)) {
          return ERR_INVAL_RECORD_TAG_POS;
        }
        break;
      case HPROF_HEAP_DUMP_SEGMENT:
        if (_version < HeapDump::Version::V102) {
          log_error(heapdump, parser)("HPROF_HEAP_DUMP_SEGMENT is not allowed in HPROF %s", hprof_version2str(_version));
          return ERR_UNKNOWN_RECORD_TAG;
        }
        if (!state->enter_heap_dump_segment(preamble.body_size)) {
          return ERR_INVAL_RECORD_TAG_POS;
        }
        break;
      case HPROF_HEAP_DUMP_END:
        if (_version < HeapDump::Version::V102) {
          log_error(heapdump, parser)("HPROF_HEAP_DUMP_END is not allowed in HPROF %s", hprof_version2str(_version));
          return ERR_UNKNOWN_RECORD_TAG;
        }
        if (preamble.body_size != 0) {
          log_error(heapdump, parser)(
              "HPROF_HEAP_DUMP_END must have no body, "
              "but its preamble specifies it to have " UINT32_FORMAT " bytes",
              preamble.body_size);
          return ERR_INVAL_RECORD_PREAMBLE;
        }
        // Assume this terminates a sequence of zero heap dump segments
        if (!state->exit_heap_dump_segments()) {
          return ERR_INVAL_RECORD_TAG_POS;
        }
        break;
      case HPROF_UNLOAD_CLASS:
      case HPROF_FRAME:
      case HPROF_TRACE:
      case HPROF_ALLOC_SITES:
      case HPROF_HEAP_SUMMARY:
      case HPROF_START_THREAD:
      case HPROF_END_THREAD:
      case HPROF_CPU_SAMPLES:
      case HPROF_CONTROL_SETTINGS:
        if (!_reader->skip(preamble.body_size)) {
          log_error(heapdump, parser)("Failed to read past a " UINT8_FORMAT_X_0 " tagged record body (" UINT32_FORMAT " bytes)",
                                     preamble.tag, preamble.body_size);
          body_res = Result::FAILED;
        }
        break;
      default:
        log_error(heapdump, parser)("Unknown record tag: " UINT8_FORMAT_X_0, preamble.tag);
        return ERR_UNKNOWN_RECORD_TAG;
    }

    switch (body_res) {
      case Result::OK:
        return nullptr;
      case Result::FAILED:
        return ERR_INVAL_RECORD_BODY;
      case Result::REPEATED_ID:
        return ERR_REPEATED_ID;
      default:
        ShouldNotReachHere();
        return nullptr; // Make compilers happy
    }
  }

  const char *step_heap_segments(State *state) {
    precond(state->position() == State::Position::AMONG_HEAP_DUMP_SEGMENTS);
    precond(_version >= HeapDump::Version::V102);

    RecordPreamble preamble;
    if (!parse_record_preamble(&preamble)) {
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    if (preamble.finish) {
      log_error(heapdump, parser)("Reached EOF, but HPROF_HEAP_DUMP_END was expected");
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    log_trace(heapdump, parser)("Record (heap segments): tag " UINT8_FORMAT_X_0 ", size " UINT32_FORMAT,
                               preamble.tag, preamble.body_size);

    switch (preamble.tag) {
      case HPROF_HEAP_DUMP_SEGMENT:
        if (state->enter_heap_dump_segment(preamble.body_size)) {
          return nullptr;
        }
        return ERR_INVAL_RECORD_TAG_POS;
      case HPROF_HEAP_DUMP_END:
        if (preamble.body_size != 0) {
          log_error(heapdump, parser)(
              "HPROF_HEAP_DUMP_END must have no body, "
              "but its preamble specifies it to have " UINT32_FORMAT " bytes",
              preamble.body_size);
          return ERR_INVAL_RECORD_PREAMBLE;
        }
        if (state->exit_heap_dump_segments()) {
          return nullptr;
        }
        return ERR_INVAL_RECORD_TAG_POS;
      case HPROF_UTF8:
      case HPROF_LOAD_CLASS:
      case HPROF_UNLOAD_CLASS:
      case HPROF_FRAME:
      case HPROF_TRACE:
      case HPROF_ALLOC_SITES:
      case HPROF_HEAP_SUMMARY:
      case HPROF_HEAP_DUMP:
      case HPROF_START_THREAD:
      case HPROF_END_THREAD:
      case HPROF_CPU_SAMPLES:
      case HPROF_CONTROL_SETTINGS:
        log_error(heapdump, parser)("Record tag " UINT8_FORMAT_X_0 " is not allowed among heap dump segments", preamble.tag);
        return ERR_INVAL_RECORD_TAG_POS;
      default:
        log_error(heapdump, parser)("Unknown record tag: " UINT8_FORMAT_X_0, preamble.tag);
        return ERR_UNKNOWN_RECORD_TAG;
    }
  }

  const char *step_heap_dump(State *state) {
    precond(state->position() == State::Position::IN_HEAP_DUMP ||
            state->position() == State::Position::IN_HEAP_DUMP_SEGMENT);

    u1 tag;
    if (!parse_subrecord_tag(&tag) || !state->reduce_remaining_record_size(sizeof(u1))) {
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    log_trace(heapdump, parser)("Subrecord: tag " UINT8_FORMAT_X_0, tag);

    Result body_res;
    u4 body_size;
    switch (tag) {
      case HPROF_GC_CLASS_DUMP:      body_res = parse_class_dump(     &_out->class_dumps,      &body_size); break;
      case HPROF_GC_INSTANCE_DUMP:   body_res = parse_instance_dump(  &_out->instance_dumps,   &body_size); break;
      case HPROF_GC_OBJ_ARRAY_DUMP:  body_res = parse_obj_array_dump( &_out->obj_array_dumps,  &body_size); break;
      case HPROF_GC_PRIM_ARRAY_DUMP: body_res = parse_prim_array_dump(&_out->prim_array_dumps, &body_size); break;
      default: // Other subrecord types are skipped
        switch (tag) {
          case HPROF_GC_ROOT_UNKNOWN:
          case HPROF_GC_ROOT_STICKY_CLASS:
          case HPROF_GC_ROOT_MONITOR_USED:
            body_size = _id_size;
            break;
          case HPROF_GC_ROOT_JNI_GLOBAL:
            body_size = 2 * _id_size;
            break;
          case HPROF_GC_ROOT_JNI_LOCAL:
          case HPROF_GC_ROOT_JAVA_FRAME:
          case HPROF_GC_ROOT_THREAD_OBJ:
            body_size = _id_size + 2 * sizeof(u4);
            break;
          case HPROF_GC_ROOT_NATIVE_STACK:
          case HPROF_GC_ROOT_THREAD_BLOCK:
            body_size = _id_size + sizeof(u4);
            break;
          default:
            return ERR_UNKNOWN_RECORD_TAG;
        }
        body_res = _reader->skip(body_size) ? Result::OK : Result::FAILED;
        if (body_res == Result::FAILED) {
          log_error(heapdump, parser)("Failed to read past a " UINT8_FORMAT_X_0 " tagged subrecord body (" UINT32_FORMAT " bytes)",
                                     tag, body_size);
        }
    }

    switch (body_res) {
      case Result::OK:
        if (state->reduce_remaining_record_size(body_size)) {
          return nullptr;
        }
        return ERR_INVAL_RECORD_BODY;
      case Result::FAILED:
        return ERR_INVAL_RECORD_BODY;
      case Result::REPEATED_ID:
        return ERR_REPEATED_ID;
      default:
        ShouldNotReachHere();
        return nullptr;
    }
  }

  // (Sub-)record preamble parsing

  struct RecordPreamble {
    bool finish;
    u1 tag;
    u4 body_size;
  };

  bool parse_record_preamble(RecordPreamble *preamble) {
    if (!_reader->read(&preamble->tag)) {
      if (_reader->eos()) {
        preamble->finish = true;
        return true;
      }
      log_error(heapdump, parser)("Failed to read a record tag");
      return false;
    }
    preamble->finish = false;
    if (!_reader->skip(sizeof(u4)) || !_reader->read(&preamble->body_size)) {
      log_error(heapdump, parser)("Failed to parse a record preamble after tag " UINT8_FORMAT_X_0, preamble->tag);
      return false;
    }
    return true;
  }

  bool parse_subrecord_tag(u1 *tag) {
    if (_reader->read(tag)) {
      return true;
    }
    log_error(heapdump, parser)("Failed to read a subrecord tag");
    return false;
  }

  // (Sub-)record body parsing

  enum class Result {
    OK,
    // Parsing failed because the record was ill-formatted.
    FAILED,
    // A record with the same ID has been already parsed before.
    REPEATED_ID
  };

#define ALLOC_NEW_RECORD(hashtable, id, record_group_name)                                                          \
  bool is_new;                                                                                                      \
  auto *record = (hashtable)->put_if_absent(id, &is_new);                                                           \
  if (!is_new) {                                                                                                    \
    log_error(heapdump, parser)("Multiple occurences of ID " UINT64_FORMAT " in %s records", id, record_group_name); \
    return Result::REPEATED_ID;                                                                                     \
  }                                                                                                                 \
  (hashtable)->maybe_grow()

#define READ_INTO_OR_FAIL(ptr, what)                         \
  do {                                                       \
    if (!_reader->read(ptr)) {                               \
      log_error(heapdump, parser)("Failed to read %s", what); \
      return Result::FAILED;                                 \
    }                                                        \
  } while (false)

#define READ_OR_FAIL(type, var, what) \
  type var;                           \
  READ_INTO_OR_FAIL(&(var), what)

#define READ_ID_INTO_OR_FAIL(ptr, what)                      \
  do {                                                       \
    if (!_reader->read_uint(ptr, _id_size)) {                \
      log_error(heapdump, parser)("Failed to read %s", what); \
      return Result::FAILED;                                 \
    }                                                        \
  } while (false)

#define READ_ID_OR_FAIL(var, what) \
  HeapDump::ID var;                     \
  READ_ID_INTO_OR_FAIL(&(var), what)

  Result parse_UTF8(u4 size, decltype(ParsedHeapDump::utf8s) *out) {
    if (size < _id_size) {
      log_error(heapdump, parser)("Too small size specified for HPROF_UTF8");
      return Result::FAILED;
    }

    READ_ID_OR_FAIL(id, "HPROF_UTF8 ID");
    ALLOC_NEW_RECORD(out, id, "HPROF_UTF8");
    record->id = id;

    u4 sym_size = size - _id_size;
    if (sym_size > INT_MAX) {
      // SymbolTable::new_symbol() takes length as an int
      log_error(heapdump, parser)("HPROF_UTF8 symbol is too large for the symbol table: " UINT32_FORMAT " > %i",
                                 sym_size, INT_MAX);
      return Result::FAILED;
    }
    if (sym_size > _sym_buf.size()) {
      _sym_buf.extend(sym_size);
    }

    if (!_reader->read_raw(_sym_buf.mem(), sym_size)) {
      log_error(heapdump, parser)("Failed to read HPROF_UTF8 symbol bytes");
      return Result::FAILED;
    }

    record->sym = TempNewSymbol(SymbolTable::new_symbol(_sym_buf.mem(), static_cast<int>(sym_size)));

    return Result::OK;
  }

  Result parse_load_class(u4 size, decltype(ParsedHeapDump::load_classes) *out) {
    if (size != 2 * (sizeof(u4) + _id_size)) {
      log_error(heapdump, parser)("Too small size specified for HPROF_LOAD_CLASS");
      return Result::FAILED;
    }

    READ_OR_FAIL(u4, serial, "HPROF_LOAD_CLASS serial");
    READ_ID_OR_FAIL(class_id, "HPROF_LOAD_CLASS class ID");

    ALLOC_NEW_RECORD(out, class_id, "HPROF_LOAD_CLASS");
    record->serial = serial;
    record->class_id = class_id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_LOAD_CLASS stack trace serial");
    READ_ID_INTO_OR_FAIL(&record->class_name_id, "HPROF_LOAD_CLASS class name ID");

    return Result::OK;
  }

  bool read_basic_value(u1 type, HeapDump::BasicValue *value_out, u4 *size_out) {
    switch (type) {
      case HPROF_NORMAL_OBJECT:
        if (!_reader->read_uint(&value_out->as_object_id, _id_size)) return false;
        *size_out = _id_size;
        break;
      case HPROF_BOOLEAN:
        if (!_reader->read(&value_out->as_boolean)) return false;
        *size_out = sizeof(value_out->as_boolean);
        break;
      case HPROF_CHAR:
        if (!_reader->read(&value_out->as_char)) return false;
        *size_out = sizeof(value_out->as_char);
        break;
      case HPROF_FLOAT:
        if (!_reader->read(&value_out->as_float)) return false;
        *size_out = sizeof(value_out->as_float);
        break;
      case HPROF_DOUBLE:
        if (!_reader->read(&value_out->as_double)) return false;
        *size_out = sizeof(value_out->as_double);
        break;
      case HPROF_BYTE:
        if (!_reader->read(&value_out->as_byte)) return false;
        *size_out = sizeof(value_out->as_byte);
        break;
      case HPROF_SHORT:
        if (!_reader->read(&value_out->as_short)) return false;
        *size_out = sizeof(value_out->as_short);
        break;
      case HPROF_INT:
        if (!_reader->read(&value_out->as_int)) return false;
        *size_out = sizeof(value_out->as_int);
        break;
      case HPROF_LONG:
        if (!_reader->read(&value_out->as_long)) return false;
        *size_out = sizeof(value_out->as_long);
        break;
      default:
        *size_out = 0;
    }
    return true;
  }

  Result parse_class_dump(decltype(ParsedHeapDump::class_dumps) *out, u4 *record_size) {
    // Array sizes will be added dynamically
    *record_size = 7 * _id_size + 2 * sizeof(u4) + 3 * sizeof(u2);

    READ_ID_OR_FAIL(id, "HPROF_GC_CLASS_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_CLASS_DUMP");
    assert(record->constant_pool.size() == 0 && record->static_fields.size() == 0 && record->instance_field_infos.size() == 0,
           "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_CLASS_DUMP stack trace serial");
    READ_ID_INTO_OR_FAIL(&record->super_id, "HPROF_GC_CLASS_DUMP super ID");
    READ_ID_INTO_OR_FAIL(&record->class_loader_id, "HPROF_GC_CLASS_DUMP class loader ID");
    READ_ID_INTO_OR_FAIL(&record->signers_id, "HPROF_GC_CLASS_DUMP signers ID");
    READ_ID_INTO_OR_FAIL(&record->protection_domain_id, "HPROF_GC_CLASS_DUMP protection domain ID");

    // Reserved
    if (!_reader->skip(2 * _id_size)) {
      log_error(heapdump, parser)("Failed to read past reserved fields of HPROF_GC_CLASS_DUMP");
      return Result::FAILED;
    }

    READ_INTO_OR_FAIL(&record->instance_size, "HPROF_GC_CLASS_DUMP instance size");

    READ_OR_FAIL(u2, constant_pool_size, "HPROF_GC_CLASS_DUMP constant pool size");
    record->constant_pool.extend(constant_pool_size);
    for (u2 i = 0; i < constant_pool_size; i++) {
      auto &constant = record->constant_pool[i];
      READ_INTO_OR_FAIL(&constant.index, "HPROF_GC_CLASS_DUMP constant index");
      READ_INTO_OR_FAIL(&constant.type, "HPROF_GC_CLASS_DUMP constant type");

      u4 value_size;
      if (!read_basic_value(constant.type, &constant.value, &value_size)) {
        log_error(heapdump, parser)("Failed to read a constant's value in HPROF_GC_CLASS_DUMP");
        return Result::FAILED;
      }
      if (value_size == 0) {
        log_error(heapdump, parser)("Unknown constant type in HPROF_GC_CLASS_DUMP: " UINT8_FORMAT_X_0, constant.type);
        return Result::FAILED;
      }
      *record_size += sizeof(u2) + sizeof(u1) + value_size;
    }

    READ_OR_FAIL(u2, static_fields_num, "HPROF_GC_CLASS_DUMP static fields number");
    record->static_fields.extend(static_fields_num);
    for (u2 i = 0; i < static_fields_num; i++) {
      auto &field = record->static_fields[i];
      READ_ID_INTO_OR_FAIL(&field.info.name_id, "HPROF_GC_CLASS_DUMP static field name ID");
      READ_INTO_OR_FAIL(&field.info.type, "HPROF_GC_CLASS_DUMP static field type");

      u4 value_size;
      if (!read_basic_value(field.info.type, &field.value, &value_size)) {
        log_error(heapdump, parser)("Failed to read a static field's value in HPROF_GC_CLASS_DUMP");
        return Result::FAILED;
      }
      if (value_size == 0) {
        log_error(heapdump, parser)("Unknown static field type in HPROF_GC_CLASS_DUMP: " UINT8_FORMAT_X_0, field.info.type);
        return Result::FAILED;
      }
      *record_size += _id_size + sizeof(u1) + value_size;
    }

    READ_OR_FAIL(u2, instance_fields_num, "HPROF_GC_CLASS_DUMP instance fields number");
    record->instance_field_infos.extend(instance_fields_num);
    for (u2 i = 0; i < instance_fields_num; i++) {
      auto &field_info = record->instance_field_infos[i];
      READ_ID_INTO_OR_FAIL(&field_info.name_id, "HPROF_GC_CLASS_DUMP instance field name ID");
      READ_INTO_OR_FAIL(&field_info.type, "HPROF_GC_CLASS_DUMP instance field type");
    }
    *record_size += instance_fields_num * (_id_size + sizeof(u1));

    return Result::OK;
  }

  Result parse_instance_dump(decltype(ParsedHeapDump::instance_dumps) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_INSTANCE_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_INSTANCE_DUMP");
    assert(record->fields_data.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_INSTANCE_DUMP stack trace serial");
    READ_ID_INTO_OR_FAIL(&record->class_id, "HPROF_GC_INSTANCE_DUMP class ID");

    READ_OR_FAIL(u4, fields_data_size, "HPROF_GC_INSTANCE_DUMP fields data size");
    record->fields_data.extend(fields_data_size);
    if (!_reader->read_raw(record->fields_data.mem(), fields_data_size)) {
      log_error(heapdump, parser)("Failed to read HPROF_GC_INSTANCE_DUMP fields data");
      return Result::FAILED;
    }

    *record_size = 2 * _id_size + 2 * sizeof(u4) + fields_data_size;
    return Result::OK;
  }

  Result parse_obj_array_dump(decltype(ParsedHeapDump::obj_array_dumps) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_OBJ_ARRAY_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_OBJ_ARRAY_DUMP");
    assert(record->elem_ids.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_OBJ_ARRAY_DUMP stack trace serial");
    READ_OR_FAIL(u4, elems_num, "HPROF_GC_OBJ_ARRAY_DUMP elements number");
    READ_ID_INTO_OR_FAIL(&record->array_class_id, "HPROF_GC_OBJ_ARRAY_DUMP array class ID");

    record->elem_ids.extend(elems_num);
    for (u4 i = 0; i < elems_num; i++) {
      READ_ID_INTO_OR_FAIL(&record->elem_ids[i], "HPROF_GC_OBJ_ARRAY_DUMP element ID");
    }
    *record_size = 2 * _id_size + 2 * sizeof(u4) + elems_num * _id_size;

    return Result::OK;
  }

  Result parse_prim_array_dump(decltype(ParsedHeapDump::prim_array_dumps) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_PRIM_ARRAY_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_PRIM_ARRAY_DUMP");
    assert(record->elems_data.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_PRIM_ARRAY_DUMP stack trace serial");
    READ_INTO_OR_FAIL(&record->elems_num, "HPROF_GC_PRIM_ARRAY_DUMP elements number");
    READ_INTO_OR_FAIL(&record->elem_type, "HPROF_GC_PRIM_ARRAY_DUMP element type");

    const BasicType elem_type = HeapDump::htype2btype(record->elem_type);
    if (elem_type == T_ILLEGAL) {
      log_error(heapdump, parser)("Unknown element type in HPROF_GC_PRIM_ARRAY_DUMP: " UINT8_FORMAT_X_0, record->elem_type);
      return Result::FAILED;
    }
    if (!is_java_primitive(elem_type)) {
      log_error(heapdump, parser)("Illegal element type in HPROF_GC_PRIM_ARRAY_DUMP: " UINT8_FORMAT_X_0, record->elem_type);
      return Result::FAILED;
    }
    u1 elem_size = type2aelembytes(elem_type);
    u4 elems_data_size = record->elems_num * elem_size;

    record->elems_data.extend(elems_data_size);
    if (!Endian::is_Java_byte_ordering_different() || elem_size == 1) {
      // Can save the data as is
      if (!_reader->read_raw(record->elems_data.mem(), elems_data_size)) {
        log_error(heapdump, parser)("Failed to read HPROF_GC_PRIM_ARRAY_DUMP elements data");
        return Result::FAILED;
      }
    } else {
      // Have to save each value byte-wise backwards
      for (u1 *elem = record->elems_data.mem(); elem < record->elems_data.mem() + elems_data_size; elem += elem_size) {
        for (u1 *byte = elem + elem_size - 1; byte >= elem; byte--) {
          READ_INTO_OR_FAIL(byte, "HPROF_GC_PRIM_ARRAY_DUMP elements data");
        }
      }
    }

    *record_size = _id_size + 2 * sizeof(u4) + sizeof(u1) + elems_data_size;
    return Result::OK;
  }

#undef ALLOC_NEW_RECORD
#undef READ_INTO_OR_FAIL
#undef READ_OR_FAIL
#undef READ_ID_INTO_OR_FAIL
#undef READ_ID_OR_FAIL
};

static HeapDump::Version parse_header(BasicTypeReader *reader) {
  constexpr char HEADER_STR_102[] = "JAVA PROFILE 1.0.2";
  constexpr char HEADER_STR_101[] = "JAVA PROFILE 1.0.1";
  STATIC_ASSERT(sizeof(HEADER_STR_102) == sizeof(HEADER_STR_101));

  char header_str[sizeof(HEADER_STR_102)];
  if (!reader->read_raw(header_str, sizeof(header_str))) {
    log_error(heapdump, parser)("Failed to read header string");
    return HeapDump::Version::UNKNOWN;
  }
  header_str[sizeof(header_str) - 1] = '\0'; // Ensure nul-terminated

  if (strcmp(header_str, HEADER_STR_102) == 0) {
    return HeapDump::Version::V102;
  }
  if (strcmp(header_str, HEADER_STR_101) == 0) {
    return HeapDump::Version::V101;
  }

  log_error(heapdump, parser)("Unknown header string: %s", header_str);
  return HeapDump::Version::UNKNOWN;
}

static const char *parse_id_size(BasicTypeReader *reader, u4 *out) {
  if (!reader->read(out)) {
    log_error(heapdump, parser)("Failed to read ID size");
    return ERR_INVAL_ID_SIZE;
  }
  if (!is_supported_id_size(*out)) {
    log_error(heapdump, parser)("ID size " UINT32_FORMAT " is not supported -- use 1, 2, 4, or 8", *out);
    return ERR_UNSUPPORTED_ID_SIZE;
  }
  return nullptr;
}

const char *HeapDumpParser::parse(const char *path, ParsedHeapDump *out) {
  guarantee(path != nullptr, "cannot parse from null path");
  guarantee(out != nullptr, "cannot save results into null container");

  log_info(heapdump, parser)("Started parsing heap dump %s", path);
  TraceTime timer("Heap dump parsing timer", TRACETIME_LOG(Info, heapdump, parser));

  FileBasicTypeReader reader;
  if (!reader.open(path)) {
    log_error(heapdump, parser)("Failed to open %s: %s", path, os::strerror(errno));
    return os::strerror(errno);
  }

  HeapDump::Version version = parse_header(&reader);
  if (version == HeapDump::Version::UNKNOWN) {
    return ERR_INVAL_HEADER_STR;
  }
  log_debug(heapdump, parser)("HPROF version: %s", hprof_version2str(version));

  u4 id_size;
  const char *err_msg = parse_id_size(&reader, &id_size);
  if (err_msg != nullptr) {
    return err_msg;
  }
  log_debug(heapdump, parser)("ID size: " UINT32_FORMAT, id_size);
  out->id_size = id_size;

  // Skip dump timestamp
  if (!reader.skip(2 * sizeof(u4))) {
    log_error(heapdump, parser)("Failed to read past heap dump timestamp");
    return ERR_INVAL_DUMP_TIMESTAMP;
  }

  err_msg = RecordsParser(&reader, out, version, id_size).parse_records();
  if (err_msg == nullptr) {
    log_info(heapdump, parser)("Successfully parsed %s", path);
  } else {
    log_info(heapdump, parser)("Position in %s after error: %zu", path, reader.pos());
  }
  return err_msg;
}


// Reads from the specified address.
class AddressBasicTypeReader : public BasicTypeReader {
 public:
  AddressBasicTypeReader(const void *from, size_t max_size) : _from(from), _max_size(max_size) {}

  bool read_raw(void *buf, size_t size) override {
    precond(buf != nullptr || size == 0);
    if (size > _max_size) {
      return false;
    }
    memcpy(buf, _from, size);
    return true;
  }

  // Reading from a specific address so this does not make sense.
  bool skip(size_t size) override { ShouldNotCallThis(); return false; }
  size_t pos() const override     { return 0; }
  bool eos() const override       { return _max_size == 0; }

 private:
  const void *_from;
  size_t _max_size;
};

HeapDump::BasicValue HeapDump::InstanceDump::read_field(u4 offset, BasicType type, u4 id_size) const {
  AddressBasicTypeReader reader(&fields_data[offset], fields_data.size() - offset);
  HeapDump::BasicValue val;
  switch (type) {
    case T_OBJECT:
    case T_ARRAY:   guarantee(reader.read_uint(&val.as_object_id, id_size), "out of bounds"); break;
    case T_BOOLEAN: guarantee(reader.read(&val.as_boolean)                , "out of bounds"); break;
    case T_CHAR:    guarantee(reader.read(&val.as_char)                   , "out of bounds"); break;
    case T_FLOAT:   guarantee(reader.read(&val.as_float)                  , "out of bounds"); break;
    case T_DOUBLE:  guarantee(reader.read(&val.as_double)                 , "out of bounds"); break;
    case T_BYTE:    guarantee(reader.read(&val.as_byte)                   , "out of bounds"); break;
    case T_SHORT:   guarantee(reader.read(&val.as_short)                  , "out of bounds"); break;
    case T_INT:     guarantee(reader.read(&val.as_int)                    , "out of bounds"); break;
    case T_LONG:    guarantee(reader.read(&val.as_long)                   , "out of bounds"); break;
    default:        ShouldNotReachHere();
  }
  return val;
}


void DumpedInstanceFieldStream::next() {
  precond(!eos());
  _field_offset += HeapDump::value_size(type(), _heap_dump.id_size);
  _field_index++;
}

bool DumpedInstanceFieldStream::eos() {
  if (_field_index < _current_class_dump->instance_field_infos.size()) {
    return false;
  }
  if (_current_class_dump->super_id == HeapDump::NULL_ID) {
    return true;
  }
  _field_index = 0;
  _current_class_dump = &_heap_dump.get_class_dump(_current_class_dump->super_id);
  return eos(); // Check again to skip the class if it has no non-static fields
}

Symbol *DumpedInstanceFieldStream::name() const {
  precond(_field_index < _current_class_dump->instance_field_infos.size());
  const HeapDump::ID name_id = _current_class_dump->instance_field_infos[_field_index].name_id;
  return _heap_dump.get_symbol(name_id);
}

BasicType DumpedInstanceFieldStream::type() const {
  precond(_field_index < _current_class_dump->instance_field_infos.size());
  const u1 t = _current_class_dump->instance_field_infos[_field_index].type;
  return HeapDump::htype2btype(t);
}

HeapDump::BasicValue DumpedInstanceFieldStream::value() const {
  const BasicType t = type();
  guarantee(_field_offset + HeapDump::value_size(t, _heap_dump.id_size) <= _instance_dump.fields_data.size(),
            "object " HDID_FORMAT " has less non-static fields' data dumped than specified by its direct class and super-classes: "
            "read " UINT32_FORMAT " bytes and expect at least " UINT32_FORMAT " more to read %s value, but only " UINT32_FORMAT " bytes left",
            _instance_dump.id, _field_offset, HeapDump::value_size(t, _heap_dump.id_size), type2name(type()),
            _instance_dump.fields_data.size() - _field_offset);
  return _instance_dump.read_field(_field_offset, t, _heap_dump.id_size);
}
