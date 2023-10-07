#include "precompiled.hpp"
#include "classfile/symbolTable.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/timerTrace.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/bytes.hpp"
#include "utilities/debug.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/hprofTag.hpp"

#include <limits>

using hdf = HeapDumpFormat;

static bool is_supported_id_size(u4 size) {
  return size == sizeof(u8) || size == sizeof(u4) || size == sizeof(u2) ||
         size == sizeof(u1);
}

// Abstarct class for reading bytes as Java types.
class AbstractReader : public StackObj {
 public:
  // Reads size bytes into buf. Returns true on success.
  virtual bool read_raw(void *buf, size_t size) = 0;

  template <class T>
  bool read(T *out) {
    if (!read_raw(out, sizeof(T))) {
      return false;
    }
    *out = Bytes::get_Java<T>(static_cast<address>(static_cast<void *>(out)));
    return true;
  }

  bool read(jfloat *out) {
    u4 tmp;
    if (!read(&tmp)) {
      return false;
    }
    *out = bit_cast<jfloat>(tmp);
    return true;
  }

  bool read(jdouble *out) {
    u8 tmp;
    if (!read(&tmp)) {
      return false;
    }
    *out = bit_cast<jdouble>(tmp);
    return true;
  }

  bool read_id(hdf::id_t *out, size_t id_size) {
    switch (id_size) {
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
    }
  }
};

// Opens a file and reads from it.
//
// Class name "FileReader" is already occupied by the ELF parsing code.
class BinaryFileReader : public AbstractReader {
 public:
  ~BinaryFileReader() {
    if (_file != nullptr && fclose(_file) != 0) {
      warning("failed to close a heap dump file");
    }
  }

  bool open(const char *path) {
    assert(path != nullptr, "cannot read from null path");

    errno = 0;

    if (_file != nullptr && fclose(_file) != 0) {
      warning("failed to close a heap dump file");
      guarantee(errno != 0, "fclose should set errno on error");
      return false;
    }
    guarantee(errno == 0, "fclose shouldn't set errno on success");

    FILE *file = os::fopen(path, "rb");
    if (file == nullptr) {
      guarantee(errno != 0, "fopen should set errno on error");
      return false;
    }

    _file = file;
    postcond(_file != nullptr);
    return true;
  }

  bool read_raw(void *buf, size_t size) override {
    assert(_file != nullptr, "file must be opened before reading");
    precond(buf != nullptr || size == 0);
    return size == 0 || fread(buf, size, 1, _file) == 1;
  }

  bool skip(size_t size) {
    precond(_file != nullptr);
    assert(size <= std::numeric_limits<long>::max(), "must fit into fseek's offset of type long");
    return fseek(_file, static_cast<long>(size), SEEK_CUR) == 0;
  }

  bool eof() const {
    precond(_file != nullptr);
    return feof(_file) != 0;
  }

  long pos() const { return ftell(_file); }

 private:
  FILE *_file = nullptr;
  size_t _buf_size = 0;
};

// Reads from the specified address.
class AddressReader : public AbstractReader {
 public:
  AddressReader(const void *from, size_t max_size) : _from(from), _max_size(max_size) {}

  bool read_raw(void *buf, size_t size) override {
    precond(buf != nullptr || size == 0);
    if (size > _max_size) {
      return false;
    }
    memcpy(buf, _from, size);
    return true;
  }

 private:
  const void *_from;
  size_t _max_size;
};

static constexpr char ERR_INVAL_HEADER[] = "invalid header";
static constexpr char ERR_INVAL_ID_SIZE[] = "invalid ID size format";
static constexpr char ERR_UNSUPPORTED_ID_SIZE[] = "unsupported ID size";
static constexpr char ERR_INVAL_DUMP_TIMESTAMP[] = "invalid dump timestamp format";

static constexpr char ERR_INVAL_RECORD_PREAMBLE[] = "invalid (sub-)record preamble";
static constexpr char ERR_INVAL_RECORD_BODY[] = "invalid (sub-)record body";
static constexpr char ERR_INVAL_RECORD_TAG_POS[] = "illegal position of a (sub-)record tag";
static constexpr char ERR_UNKNOWN_RECORD_TAG[] = "unknown (sub-)record tag";

static constexpr char ERR_REPEATED_ID[] = "found a repeated ID";

// For logging.
const char *hprof_version2str(hdf::Version version) {
  switch (version) {
    case hdf::Version::V102:
      return "v1.0.2";
    case hdf::Version::V101:
      return "v1.0.1";
    case hdf::Version::UNKNOWN:
      return "<unknown version>";
    default:
      ShouldNotReachHere();
  }
}

class RecordsParser : public StackObj {
 public:
  RecordsParser(BinaryFileReader *reader, ParsedHeapDump *out, hdf::Version version, u4 id_size)
      : _reader(reader), _out(out), _version(version), _id_size(id_size) {
    precond(_reader != nullptr && _out != nullptr &&
            _version != hdf::Version::UNKNOWN && is_supported_id_size(id_size));
  }

  const char *parse_records() {
    State state;
    const char *err_msg = nullptr;

    log_debug(heapdumpparsing)("Parsing records");

    while (!_reader->eof() && err_msg == nullptr) {
      switch (state.position()) {
        case State::Position::TOPLEVEL:
          err_msg = step_toplevel(&state);
          break;
        case State::Position::AMONG_HEAP_DUMP_SEGMENTS:
          err_msg = step_heap_segments(&state);
          break;
        case State::Position::IN_HEAP_DUMP_SEGMENT:
          precond(_version >= hdf::Version::V102);
        case State::Position::IN_HEAP_DUMP:
          err_msg = step_heap_dump(&state);
      }
    }

    return err_msg;
  }

 private:
  BinaryFileReader *_reader;
  ParsedHeapDump *_out;

  hdf::Version _version;
  u4 _id_size;

  class SymbolBuffer : public StackObj {
   public:
    explicit SymbolBuffer(size_t size = 1 * M) : _size(size) {
      _buf = static_cast<char *>(os::malloc(size, mtInternal));
      if (_buf == nullptr) {
        vm_exit_out_of_memory(size, OOM_MALLOC_ERROR,
                              "construction of a heap dump parsing symbol buffer");
      }
    }

    ~SymbolBuffer() { FreeHeap(_buf); }

    NONCOPYABLE(SymbolBuffer);

    void ensure_fits(size_t size) {
      if (size <= _size) {
        return;
      }
      _buf = static_cast<char *>(os::realloc(buf(), size, mtInternal));
      if (buf() == nullptr) {
        vm_exit_out_of_memory(size, OOM_MALLOC_ERROR,
                              "extension of a heap dump parsing symbol buffer");
      }
      _size = size;
    }

    char *buf() { return _buf; }

   private:
    size_t _size;
    char *_buf;
  };

  SymbolBuffer _sym_buf;

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
        log_error(heapdumpparsing)("Illegal position transition: %s -> %s",
                                   position2str(position()),
                                   position2str(Position::IN_HEAP_DUMP));
        return false;
      }
      precond(_remaining_record_size == 0);

      if (size > 0) {
        log_debug(heapdumpparsing)("Position transition: %s -> %s (size " UINT32_FORMAT ")",
                                   position2str(position()),
                                   position2str(Position::IN_HEAP_DUMP),
                                   size);
        _position = Position::IN_HEAP_DUMP;
        _remaining_record_size = size;
      } else {
        log_debug(heapdumpparsing)("Got HPROF_HEAP_DUMP of size 0 -- no position transition");
      }

      return true;
    }

    // When found a HPROF_HEAP_DUMP_SEGMENT.
    bool enter_heap_dump_segment(u4 size) {
      if (position() != Position::AMONG_HEAP_DUMP_SEGMENTS &&
          position() != Position::TOPLEVEL) {
        log_error(heapdumpparsing)("Illegal position transition: %s -> %s",
                                   position2str(position()),
                                   position2str(Position::IN_HEAP_DUMP_SEGMENT));
        return false;
      }
      precond(_remaining_record_size == 0);

      if (size > 0) {
        log_debug(heapdumpparsing)("Position transition: %s -> %s (size " UINT32_FORMAT ")",
                                   position2str(position()),
                                   position2str(Position::IN_HEAP_DUMP_SEGMENT),
                                   size);
        _position = Position::IN_HEAP_DUMP_SEGMENT;
        _remaining_record_size = size;
      } else {
        log_debug(heapdumpparsing)(
            "Got HPROF_HEAP_DUMP_SEGMENT of size 0 -- position transition: %s -> %s",
            position2str(position()),
            position2str(Position::AMONG_HEAP_DUMP_SEGMENTS));
        _position = Position::AMONG_HEAP_DUMP_SEGMENTS;
      }

      return true;
    }

    // When found a HPROF_HEAP_DUMP_END.
    bool exit_heap_dump_segments() {
      // Allow top-level position for sequences of zero segments
      if (position() != Position::AMONG_HEAP_DUMP_SEGMENTS &&
          position() != Position::TOPLEVEL) {
        log_error(heapdumpparsing)("Illegal position transition: %s -> %s",
                                   position2str(position()),
                                   position2str(Position::TOPLEVEL));
        return false;
      }
      assert(_remaining_record_size == 0, "must be 0 outside a record");
      log_debug(heapdumpparsing)("Position transition: %s -> %s",
                                 position2str(position()),
                                 position2str(Position::TOPLEVEL));
      _position = Position::TOPLEVEL;
      return true;
    }

    // When parsed the specified portion of the current record.
    bool reduce_remaining_record_size(u4 amount) {
      assert(position() != Position::TOPLEVEL &&
                 position() != Position::AMONG_HEAP_DUMP_SEGMENTS,
             "should be inside a record");
      assert(_remaining_record_size > 0, "must be > 0 inside a record");

      if (_remaining_record_size < amount) {
        log_error(heapdumpparsing)("Tried to read " UINT32_FORMAT " bytes "
                                   "from a subrecord with " UINT32_FORMAT " bytes left",
                                   amount, _remaining_record_size);
        return false;
      }

      _remaining_record_size -= amount;

      if (_remaining_record_size == 0) {
        if (position() == Position::IN_HEAP_DUMP) {
          log_debug(heapdumpparsing)("Position transition: %s -> %s",
                                     position2str(position()),
                                     position2str(Position::TOPLEVEL));
          _position = Position::TOPLEVEL;
        } else if (position() == Position::IN_HEAP_DUMP_SEGMENT) {
          log_debug(heapdumpparsing)("Position transition: %s -> %s",
                                     position2str(position()),
                                     position2str(Position::AMONG_HEAP_DUMP_SEGMENTS));
          _position = Position::AMONG_HEAP_DUMP_SEGMENTS;
        } else {
          ShouldNotReachHere();  // We should be inside a record
        }
      }

      return true;
    }

   private:
    Position _position = Position::TOPLEVEL;
    u4 _remaining_record_size = 0;

    // For logging.
    static const char *position2str(Position position) {
      switch (position) {
        case Position::TOPLEVEL:
          return "TOPLEVEL";
        case Position::IN_HEAP_DUMP:
          return "IN_HEAP_DUMP";
        case Position::IN_HEAP_DUMP_SEGMENT:
          return "IN_HEAP_DUMP_SEGMENT";
        case Position::AMONG_HEAP_DUMP_SEGMENTS:
          return "AMONG_HEAP_DUMP_SEGMENTS";
        default:
          ShouldNotReachHere();
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
    log_trace(heapdumpparsing)("Record (toplevel): tag " UINT8_FORMAT_X_0
                               ", size " UINT32_FORMAT,
                               preamble.tag, preamble.body_size);

    Result body_res = Result::OK;
    switch (preamble.tag) {
      case HPROF_UTF8:
        body_res = parse_UTF8(preamble.body_size, &_out->utf8_records);
        break;
      case HPROF_LOAD_CLASS:
        body_res =
            parse_load_class(preamble.body_size, &_out->load_class_records);
        break;
      case HPROF_HEAP_DUMP:
        if (!state->enter_heap_dump(preamble.body_size)) {
          return ERR_INVAL_RECORD_TAG_POS;
        }
        break;
      case HPROF_HEAP_DUMP_SEGMENT:
        if (_version < hdf::Version::V102) {
          log_error(heapdumpparsing)("HPROF_HEAP_DUMP_SEGMENT is not allowed in HPROF %s",
                                     hprof_version2str(_version));
          return ERR_UNKNOWN_RECORD_TAG;
        }
        if (!state->enter_heap_dump_segment(preamble.body_size)) {
          return ERR_INVAL_RECORD_TAG_POS;
        }
        break;
      case HPROF_HEAP_DUMP_END:
        if (_version < hdf::Version::V102) {
          log_error(heapdumpparsing)("HPROF_HEAP_DUMP_END is not allowed in HPROF %s",
                                     hprof_version2str(_version));
          return ERR_UNKNOWN_RECORD_TAG;
        }
        if (preamble.body_size != 0) {
          log_error(heapdumpparsing)(
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
          log_error(heapdumpparsing)("Failed to read past a " UINT8_FORMAT_X_0
                                     " tagged record body (" UINT32_FORMAT " bytes)",
                                     preamble.tag, preamble.body_size);
          body_res = Result::FAILED;
        }
        break;
      default:
        log_error(heapdumpparsing)("Unknown record tag: " UINT8_FORMAT_X_0, preamble.tag);
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
    }
  }

  const char *step_heap_segments(State *state) {
    precond(state->position() == State::Position::AMONG_HEAP_DUMP_SEGMENTS);
    precond(_version >= hdf::Version::V102);

    RecordPreamble preamble;
    if (!parse_record_preamble(&preamble)) {
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    if (preamble.finish) {
      log_error(heapdumpparsing)("Reached EOF, but HPROF_HEAP_DUMP_END was expected");
      return ERR_INVAL_RECORD_PREAMBLE;
    }
    log_trace(heapdumpparsing)("Record (heap segments): tag " UINT8_FORMAT_X_0
                               ", size " UINT32_FORMAT,
                               preamble.tag, preamble.body_size);

    switch (preamble.tag) {
      case HPROF_HEAP_DUMP_SEGMENT:
        if (state->enter_heap_dump_segment(preamble.body_size)) {
          return nullptr;
        }
        return ERR_INVAL_RECORD_TAG_POS;
      case HPROF_HEAP_DUMP_END:
        if (preamble.body_size != 0) {
          log_error(heapdumpparsing)(
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
        log_error(heapdumpparsing)("Record tag " UINT8_FORMAT_X_0
                                   " is not allowed among heap dump segments",
                                   preamble.tag);
        return ERR_INVAL_RECORD_TAG_POS;
      default:
        log_error(heapdumpparsing)("Unknown record tag: " UINT8_FORMAT_X_0, preamble.tag);
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
    log_trace(heapdumpparsing)("Subrecord: tag " UINT8_FORMAT_X_0, tag);

    Result body_res;
    u4 body_size;
    switch (tag) {
      case HPROF_GC_CLASS_DUMP:
        body_res = parse_class_dump(&_out->class_dump_records, &body_size);
        break;
      case HPROF_GC_INSTANCE_DUMP:
        body_res = parse_instance_dump(&_out->instance_dump_records, &body_size);
        break;
      case HPROF_GC_OBJ_ARRAY_DUMP:
        body_res = parse_obj_array_dump(&_out->obj_array_dump_records, &body_size);
        break;
      case HPROF_GC_PRIM_ARRAY_DUMP:
        body_res = parse_prim_array_dump(&_out->prim_array_dump_records, &body_size);
        break;
      default:  // Other subrecord types are skipped
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
          log_error(heapdumpparsing)("Failed to read past a " UINT8_FORMAT_X_0
                                     " tagged subrecord body (" UINT32_FORMAT " bytes)",
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
      if (_reader->eof()) {
        preamble->finish = true;
        return true;
      }
      log_error(heapdumpparsing)("Failed to read a record tag");
      return false;
    }
    preamble->finish = false;
    if (!_reader->skip(sizeof(u4)) || !_reader->read(&preamble->body_size)) {
      log_error(heapdumpparsing)("Failed to parse a record preamble after tag " UINT8_FORMAT_X_0,
                                 preamble->tag);
      return false;
    }
    return true;
  }

  bool parse_subrecord_tag(u1 *tag) {
    if (_reader->read(tag)) {
      return true;
    }
    log_error(heapdumpparsing)("Failed to read a subrecord tag");
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

#define ALLOC_NEW_RECORD(hashtable, id, record_group_name)                                  \
  bool is_new;                                                                              \
  auto *record = (hashtable)->put_if_absent(id, &is_new);                                   \
  if (!is_new) {                                                                            \
    log_error(heapdumpparsing)("Multiple occurences of ID " UINT64_FORMAT " in %s records", \
                               id, record_group_name);                                      \
    return Result::REPEATED_ID;                                                             \
  }                                                                                         \
  (hashtable)->maybe_grow()

#define READ_INTO_OR_FAIL(ptr, what)                         \
  do {                                                       \
    if (!_reader->read(ptr)) {                               \
      log_error(heapdumpparsing)("Failed to read %s", what); \
      return Result::FAILED;                                 \
    }                                                        \
  } while (false)

#define READ_OR_FAIL(type, var, what) \
  type var;                           \
  READ_INTO_OR_FAIL(&(var), what)

#define READ_ID_INTO_OR_FAIL(ptr, what)                      \
  do {                                                       \
    if (!_reader->read_id(ptr, _id_size)) {                  \
      log_error(heapdumpparsing)("Failed to read %s", what); \
      return Result::FAILED;                                 \
    }                                                        \
  } while (false)

#define READ_ID_OR_FAIL(var, what) \
  hdf::id_t var;                   \
  READ_ID_INTO_OR_FAIL(&(var), what)

  Result parse_UTF8(u4 size, decltype(ParsedHeapDump::utf8_records) *out) {
    if (size < _id_size) {
      log_error(heapdumpparsing)("Too small size specified for HPROF_UTF8");
      return Result::FAILED;
    }

    READ_ID_OR_FAIL(id, "HPROF_UTF8 ID");
    ALLOC_NEW_RECORD(out, id, "HPROF_UTF8");
    record->id = id;

    u4 sym_size = size - _id_size;
    if (sym_size > std::numeric_limits<int>::max()) {
      // SymbolTable::new_symbol() takes length as an int
      log_error(heapdumpparsing)("HPROF_UTF8 symbol is too large for the symbol table: " UINT32_FORMAT " > %i",
                                 sym_size,  std::numeric_limits<int>::max());
      return Result::FAILED;
    }
    _sym_buf.ensure_fits(sym_size);

    if (!_reader->read_raw(_sym_buf.buf(), sym_size)) {
      log_error(heapdumpparsing)("Failed to read HPROF_UTF8 symbol bytes");
      return Result::FAILED;
    }

    record->sym = TempNewSymbol(SymbolTable::new_symbol(_sym_buf.buf(), static_cast<int>(sym_size)));

    return Result::OK;
  }

  Result parse_load_class(u4 size, decltype(ParsedHeapDump::load_class_records) *out) {
    if (size != 2 * (sizeof(u4) + _id_size)) {
      log_error(heapdumpparsing)("Too small size specified for HPROF_LOAD_CLASS");
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

  bool read_basic_value(u1 type, hdf::BasicValue *value_out, size_t *size_out) {
    switch (type) {
      case HPROF_NORMAL_OBJECT:
        if (!_reader->read_id(&value_out->as_object_id, _id_size)) return false;
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

  Result parse_class_dump(decltype(ParsedHeapDump::class_dump_records) *out, u4 *record_size) {
    // Array sizes will be added dynamically
    *record_size = 7 * _id_size + 2 * sizeof(u4) + 3 * sizeof(u2);

    READ_ID_OR_FAIL(id, "HPROF_GC_CLASS_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_CLASS_DUMP");
    assert(record->constant_pool.size() == 0 &&
               record->static_fields.size() == 0 &&
               record->instance_field_infos.size() == 0,
           "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_CLASS_DUMP stack trace serial");
    READ_ID_INTO_OR_FAIL(&record->super_id, "HPROF_GC_CLASS_DUMP super ID");
    READ_ID_INTO_OR_FAIL(&record->class_loader_id, "HPROF_GC_CLASS_DUMP class loader ID");
    READ_ID_INTO_OR_FAIL(&record->signers_id, "HPROF_GC_CLASS_DUMP signers ID");
    READ_ID_INTO_OR_FAIL(&record->protection_domain_id, "HPROF_GC_CLASS_DUMP protection domain ID");

    // Reserved
    if (!_reader->skip(2 * _id_size)) {
      log_error(heapdumpparsing)("Failed to read past reserved fields of HPROF_GC_CLASS_DUMP");
      return Result::FAILED;
    }

    READ_INTO_OR_FAIL(&record->instance_size, "HPROF_GC_CLASS_DUMP instance size");

    READ_OR_FAIL(u2, constant_pool_size, "HPROF_GC_CLASS_DUMP constant pool size");
    record->constant_pool.extend_to(constant_pool_size);
    for (u2 i = 0; i < constant_pool_size; i++) {
      auto &constant = record->constant_pool[i];
      READ_INTO_OR_FAIL(&constant.index, "HPROF_GC_CLASS_DUMP constant index");
      READ_INTO_OR_FAIL(&constant.type, "HPROF_GC_CLASS_DUMP constant type");

      size_t value_size;
      if (!read_basic_value(constant.type, &constant.value, &value_size)) {
        log_error(heapdumpparsing)("Failed to read a constant's value in HPROF_GC_CLASS_DUMP");
        return Result::FAILED;
      }
      if (value_size == 0) {
        log_error(heapdumpparsing)("Unknown constant type in "
                                   "HPROF_GC_CLASS_DUMP: " UINT8_FORMAT_X_0,
                                   constant.type);
        return Result::FAILED;
      }
      *record_size += sizeof(u2) + sizeof(u1) + value_size;
    }

    READ_OR_FAIL(u2, static_fields_num, "HPROF_GC_CLASS_DUMP static fields number");
    record->static_fields.extend_to(static_fields_num);
    for (u2 i = 0; i < static_fields_num; i++) {
      auto &field = record->static_fields[i];
      READ_ID_INTO_OR_FAIL(&field.info.name_id, "HPROF_GC_CLASS_DUMP static field name ID");
      READ_INTO_OR_FAIL(&field.info.type, "HPROF_GC_CLASS_DUMP static field type");

      size_t value_size;
      if (!read_basic_value(field.info.type, &field.value, &value_size)) {
        log_error(heapdumpparsing)("Failed to read a static field's value in HPROF_GC_CLASS_DUMP");
        return Result::FAILED;
      }
      if (value_size == 0) {
        log_error(heapdumpparsing)("Unknown static field type in "
                                   "HPROF_GC_CLASS_DUMP: " UINT8_FORMAT_X_0,
                                   field.info.type);
        return Result::FAILED;
      }
      *record_size += _id_size + sizeof(u1) + value_size;
    }

    READ_OR_FAIL(u2, instance_fields_num, "HPROF_GC_CLASS_DUMP instance fields number");
    record->instance_field_infos.extend_to(instance_fields_num);
    for (u2 i = 0; i < instance_fields_num; i++) {
      auto &field_info = record->instance_field_infos[i];
      READ_ID_INTO_OR_FAIL(&field_info.name_id, "HPROF_GC_CLASS_DUMP instance field name ID");
      READ_INTO_OR_FAIL(&field_info.type, "HPROF_GC_CLASS_DUMP instance field type");
    }
    *record_size += instance_fields_num * (_id_size + sizeof(u1));

    return Result::OK;
  }

  Result parse_instance_dump(decltype(ParsedHeapDump::instance_dump_records) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_INSTANCE_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_INSTANCE_DUMP");
    assert(record->fields_data.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_INSTANCE_DUMP stack trace serial");
    READ_ID_INTO_OR_FAIL(&record->class_id, "HPROF_GC_INSTANCE_DUMP class ID");

    READ_OR_FAIL(u4, fields_data_size, "HPROF_GC_INSTANCE_DUMP fields data size");
    record->fields_data.extend_to(fields_data_size);
    if (!_reader->read_raw(record->fields_data.mem(), fields_data_size)) {
      log_error(heapdumpparsing)("Failed to read HPROF_GC_INSTANCE_DUMP fields data");
      return Result::FAILED;
    }

    *record_size = 2 * _id_size + 2 * sizeof(u4) + fields_data_size;
    return Result::OK;
  }

  Result parse_obj_array_dump(decltype(ParsedHeapDump::obj_array_dump_records) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_OBJ_ARRAY_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_OBJ_ARRAY_DUMP");
    assert(record->elem_ids.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_OBJ_ARRAY_DUMP stack trace serial");
    READ_OR_FAIL(u4, elems_num, "HPROF_GC_OBJ_ARRAY_DUMP elements number");
    READ_ID_INTO_OR_FAIL(&record->array_class_id, "HPROF_GC_OBJ_ARRAY_DUMP array class ID");

    record->elem_ids.extend_to(elems_num);
    for (u4 i = 0; i < elems_num; i++) {
      READ_ID_INTO_OR_FAIL(&record->elem_ids[i], "HPROF_GC_OBJ_ARRAY_DUMP element ID");
    }
    *record_size = 2 * _id_size + 2 * sizeof(u4) + elems_num * _id_size;

    return Result::OK;
  }

  Result parse_prim_array_dump(decltype(ParsedHeapDump::prim_array_dump_records) *out, u4 *record_size) {
    READ_ID_OR_FAIL(id, "HPROF_GC_PRIM_ARRAY_DUMP ID");

    ALLOC_NEW_RECORD(out, id, "HPROF_GC_PRIM_ARRAY_DUMP");
    assert(record->elems_data.size() == 0, "newly allocated record must be empty");
    record->id = id;

    READ_INTO_OR_FAIL(&record->stack_trace_serial, "HPROF_GC_PRIM_ARRAY_DUMP stack trace serial");
    READ_INTO_OR_FAIL(&record->elems_num, "HPROF_GC_PRIM_ARRAY_DUMP elements number");
    READ_INTO_OR_FAIL(&record->elem_type, "HPROF_GC_PRIM_ARRAY_DUMP element type");

    size_t elem_size = hdf::prim2size(record->elem_type);
    if (elem_size == 0) {
      log_error(heapdumpparsing)(
          "Unknown element type in HPROF_GC_PRIM_ARRAY_DUMP: " UINT8_FORMAT_X_0,
          record->elem_type);
      return Result::FAILED;
    }
    size_t elems_data_size = record->elems_num * elem_size;

    record->elems_data.extend_to(elems_data_size);
    if (!Endian::is_Java_byte_ordering_different() || elem_size == 1) {
      // Can save the data as is
      if (!_reader->read_raw(record->elems_data.mem(), elems_data_size)) {
        log_error(heapdumpparsing)("Failed to read HPROF_GC_PRIM_ARRAY_DUMP elements data");
        return Result::FAILED;
      }
    } else {
      // Have to save each value byte-wise backwards
      for (u1 *elem = record->elems_data.mem();
           elem < record->elems_data.mem() + elems_data_size;
           elem += elem_size) {
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

static hdf::Version parse_header(BinaryFileReader *reader) {
  static constexpr char HEADER_102[] = "JAVA PROFILE 1.0.2";
  static constexpr char HEADER_101[] = "JAVA PROFILE 1.0.1";
  STATIC_ASSERT(sizeof(HEADER_102) == sizeof(HEADER_101));

  char header[sizeof(HEADER_102)];
  if (!reader->read_raw(header, sizeof(header))) {
    log_error(heapdumpparsing)("Failed to read header");
    return hdf::Version::UNKNOWN;
  }
  header[sizeof(header) - 1] = '\0';

  if (strcmp(header, HEADER_102) == 0) {
    return hdf::Version::V102;
  }
  if (strcmp(header, HEADER_101) == 0) {
    return hdf::Version::V101;
  }

  log_error(heapdumpparsing)("Unknown header: %s", header);
  return hdf::Version::UNKNOWN;
}

static const char *parse_id_size(BinaryFileReader *reader, u4 *out) {
  u4 id_size;
  if (!reader->read(&id_size)) {
    log_error(heapdumpparsing)("Failed to read ID size");
    return ERR_INVAL_ID_SIZE;
  }
  if (!is_supported_id_size(id_size)) {
    log_error(heapdumpparsing)("ID size " UINT32_FORMAT " is not supported -- use 1, 2, 4, or 8",
                               id_size);
    return ERR_UNSUPPORTED_ID_SIZE;
  }
  *out = id_size;
  return nullptr;
}

const char *HeapDumpParser::parse(const char *path, ParsedHeapDump *out) {
  guarantee(path != nullptr, "cannot parse from null path");
  guarantee(out != nullptr, "cannot save results into null container");

  log_info(heapdumpparsing)("Started parsing %s", path);
  TraceTime timer("Heap dump parsing timer", TRACETIME_LOG(Info, heapdumpparsing));

  BinaryFileReader reader;
  if (!reader.open(path)) {
    log_error(heapdumpparsing)("Failed to open %s: %s", path, os::strerror(errno));
    return os::strerror(errno);
  }

  hdf::Version version = parse_header(&reader);
  if (version == hdf::Version::UNKNOWN) {
    return ERR_INVAL_HEADER;
  }
  log_debug(heapdumpparsing)("HPROF version: %s", hprof_version2str(version));

  u4 id_size;
  const char *err_msg = parse_id_size(&reader, &id_size);
  if (err_msg != nullptr) {
    return err_msg;
  }
  log_debug(heapdumpparsing)("ID size: " UINT32_FORMAT, id_size);
  out->id_size = id_size;

  // Skip dump timestamp
  if (!reader.skip(2 * sizeof(u4))) {
    log_error(heapdumpparsing)("Failed to read past heap dump timestamp");
    return ERR_INVAL_DUMP_TIMESTAMP;
  }

  err_msg = RecordsParser(&reader, out, version, id_size).parse_records();

  if (err_msg == nullptr) {
    log_info(heapdumpparsing)("Successfully parsed %s", path);
  } else {
    log_info(heapdumpparsing)("Position in %s after error: %li", path, reader.pos());
  }
  return err_msg;
}

size_t hdf::InstanceDumpRecord::read_field(u4 offset, char sig, u4 id_size, hdf::BasicValue *out) const {
  AddressReader reader(&fields_data[offset], fields_data.size() - offset);
  switch (sig) {
    case JVM_SIGNATURE_CLASS:
    case JVM_SIGNATURE_ARRAY:   return reader.read_id(&out->as_object_id, id_size) ? sizeof(out->as_object_id) : 0;
    case JVM_SIGNATURE_BOOLEAN: return reader.read(&out->as_boolean) ? sizeof(out->as_boolean) : 0;
    case JVM_SIGNATURE_CHAR:    return reader.read(&out->as_char) ? sizeof(out->as_char) : 0;
    case JVM_SIGNATURE_FLOAT:   return reader.read(&out->as_float) ? sizeof(out->as_float) : 0;
    case JVM_SIGNATURE_DOUBLE:  return reader.read(&out->as_double) ? sizeof(out->as_double) : 0;
    case JVM_SIGNATURE_BYTE:    return reader.read(&out->as_byte) ? sizeof(out->as_byte) : 0;
    case JVM_SIGNATURE_SHORT:   return reader.read(&out->as_short) ? sizeof(out->as_short) : 0;
    case JVM_SIGNATURE_INT:     return reader.read(&out->as_int) ? sizeof(out->as_int) : 0;
    case JVM_SIGNATURE_LONG:    return reader.read(&out->as_long) ? sizeof(out->as_long) : 0;
    default:                    return 0;
  }
}
