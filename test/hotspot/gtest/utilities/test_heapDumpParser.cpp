#include "precompiled.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/heapDumpParser.hpp"
#include "unittest.hpp"

#include <functional>

static const char TEST_FILENAME[] = "heap_dump_parsing_test.hprof";

static void fill_test_file(const char *contents, size_t size) {
  FILE *file = os::fopen(TEST_FILENAME, "wb");
  ASSERT_NE(nullptr, file) << "Cannot open " << TEST_FILENAME
                           << " for writing: " << os::strerror(errno);
  EXPECT_EQ(1U, fwrite(contents, size, 1, file)) << "Cannot write test data into "
                                                        << TEST_FILENAME << ": " << os::strerror(errno);
  ASSERT_EQ(0, fclose(file)) << "Cannot close the test file: " << os::strerror(errno);
}

struct RecordAmounts {
  size_t utf8;
  size_t load_class;
  size_t class_dump;
  size_t instance_dump;
  size_t obj_array_dump;
  size_t prim_array_dump;
};

static void check_record_amounts(const RecordAmounts &expected, const ParsedHeapDump &actual) {
  EXPECT_EQ(expected.utf8, static_cast<size_t>(actual.utf8_records.number_of_entries()));
  EXPECT_EQ(expected.load_class, static_cast<size_t>(actual.load_class_records.number_of_entries()));
  EXPECT_EQ(expected.class_dump, static_cast<size_t>(actual.class_dump_records.number_of_entries()));
  EXPECT_EQ(expected.instance_dump, static_cast<size_t>(actual.instance_dump_records.number_of_entries()));
  EXPECT_EQ(expected.obj_array_dump, static_cast<size_t>(actual.obj_array_dump_records.number_of_entries()));
  EXPECT_EQ(expected.prim_array_dump, static_cast<size_t>(actual.prim_array_dump_records.number_of_entries()));
}

template <class ElemT, class SizeT, class Eq>
static void check_array_eq(const HeapDumpFormat::Array<ElemT, SizeT> &l,
                           const HeapDumpFormat::Array<ElemT, SizeT> &r,
                           const char *array_name, const Eq &eq) {
  ASSERT_EQ(l.size(), r.size());
  for (SizeT i = 0; i < l.size(); i++) {
    EXPECT_PRED2(eq, l[i], r[i]) << array_name << " array differs on i = " << i;
  }
}

static bool basic_value_eq(HeapDumpFormat::BasicValue l,
                           HeapDumpFormat::BasicValue r, u1 type) {
  switch (type) {
    case HPROF_BOOLEAN:
      return l.as_boolean == r.as_boolean;
    case HPROF_CHAR:
      return l.as_char == r.as_char;
    case HPROF_FLOAT:
      return l.as_float == r.as_float;
    case HPROF_DOUBLE:
      return l.as_double == r.as_double;
    case HPROF_BYTE:
      return l.as_byte == r.as_byte;
    case HPROF_SHORT:
      return l.as_short == r.as_short;
    case HPROF_INT:
      return l.as_int == r.as_int;
    case HPROF_LONG:
      return l.as_long == r.as_long;
    default:
      EXPECT_TRUE(false) << "Unknown basic value type: " << type;
      ShouldNotReachHere();
  }
}

static const char CONTENTS_UTF8[] =
    "JAVA PROFILE 1.0.1\0"                                  // Header
    "\x00\x00\x00\x04"                                      // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"                      // Dump timestamp

    "\x01"                                                  // HPROF_UTF8 tag
    "\x00\x00\x00\x00"                                      // Record timestamp
    "\x00\x00\x00\x11"                                      // Body size

    "\x07\x5b\xcd\x15"                                      // ID = 123456789
    "\x48\x65\x6c\x6c\x6f\x2c\x20\x77\x6f\x72\x6c\x64\x21"  // "Hello, world!" in UTF-8
    ;

TEST_VM(HeapDumpParser, single_utf8_record) {
  fill_test_file(CONTENTS_UTF8, sizeof(CONTENTS_UTF8) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({1 /* UTF-8 */, 0, 0, 0, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::id_t expected_id = 123456789;
  const char expected_str[] = "Hello, world!";

  auto *record = heap_dump.utf8_records.get(expected_id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected_id, record->id);
  {
    ResourceMark rm;
    EXPECT_STREQ(expected_str, record->str->as_C_string());
  }
}

static const char CONTENTS_LOAD_CLASS[] =
    "JAVA PROFILE 1.0.1\0"              // Header
    "\x00\x00\x00\x08"                  // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"  // Dump timestamp

    "\x02"                              // HPROF_LOAD_CLASS tag
    "\x00\x00\x00\x00"                  // Record timestamp
    "\x00\x00\x00\x18"                  // Body size

    "\x01\x02\x03\x04"                  // class serial
    "\x00\x00\x00\x06\xc7\x93\x73\xb8"  // class ID
    "\x00\x00\x00\x01"                  // stack trace serial
    "\x00\x00\x7f\xfa\x40\x05\x65\x50"  // class name ID
    ;

TEST_VM(HeapDumpParser, single_load_class_record) {
  fill_test_file(CONTENTS_LOAD_CLASS, sizeof(CONTENTS_LOAD_CLASS) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 1 /* load class */, 0, 0, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::LoadClassRecord expected = {
      0x01020304,          // class serial
      0x00000006c79373b8,  // class ID
      0x00000001,          // stack trace serial
      0x00007ffa40056550   // class name ID
  };

  auto *record = heap_dump.load_class_records.get(expected.class_id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected.serial, record->serial);
  EXPECT_EQ(expected.class_id, record->class_id);
  EXPECT_EQ(expected.stack_trace_serial, record->stack_trace_serial);
  EXPECT_EQ(expected.class_name_id, record->class_name_id);
}

static const char CONTENTS_CLASS_DUMP[] =
    "JAVA PROFILE 1.0.1\0"                // Header
    "\x00\x00\x00\x08"                    // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // Dump timestamp

    "\x0C"                                // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                    // Record timestamp
    "\x00\x00\x00\x7e"                    // Body size

    "\x20"                                // HPROF_GC_CLASS_DUMP tag

    "\x00\x00\x00\x06\xc7\x93\x73\xf8"    // class ID
    "\x12\x34\x56\x78"                    // stack trace serial
    "\x00\x00\x00\x06\xc7\x93\x3a\x58"    // super ID
    "\x00\x00\x00\x06\xc7\x92\x29\x38"    // class loader ID
    "\x00\x00\x00\x06\xc7\x90\x31\x5f"    // signers ID
    "\x00\x00\x00\x06\xc7\x8d\x85\xc0"    // protection domain ID
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // reserved
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // reserved
    "\x00\x00\x00\x18"                    // instance size
    "\x00\x01"                            // constant pool size
      "\x00\x01"                            // index
      "\x09"                                // type = short
      "\x67\x89"                            // value
    "\x00\x02"                            // static fields number
      "\x00\x00\x7f\xfa\x2c\x13\xca\xd0"    // name ID
      "\x04"                                // type = boolean
      "\x01"                                // value
      "\x00\x00\x7f\xfa\x94\x00\x98\x18"    // name ID
      "\x0a"                                // type = int
      "\x12\xab\xcd\xef"                    // value
    "\x00\x03"                            // instance fields number
      "\x00\x00\x7f\xfa\x90\x16\xad\x30"    // name ID
      "\x05"                                // type = char
      "\x00\x00\x7f\xfa\x94\x00\x98\x18"    // name ID
      "\x02"                                // type = object
      "\x00\x00\x7f\xfa\x90\x3c\x5a\xf8"    // name ID
      "\x0b"                                // type = long
    ;

TEST_VM(HeapDumpParser, single_class_dump_subrecord) {
  fill_test_file(CONTENTS_CLASS_DUMP, sizeof(CONTENTS_CLASS_DUMP) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 1 /* class dump */, 0, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::ClassDumpRecord expected = {
      0x00000006c79373f8,  // ID
      0x12345678,          // stack trace serial
      0x00000006c7933a58,  // super ID
      0x00000006c7922938,  // class loader ID
      0x00000006c790315f,  // signers ID
      0x00000006c78d85c0,  // protection domain ID
      0x00000018           // instance size
  };
  expected.constant_pool.extend_to(1);
  expected.constant_pool[0] = {/* index */ 0x01, /* type */ 0x09, /* value */ 0x6789};
  expected.static_fields.extend_to(2);
  expected.static_fields[0] = {{/* name ID */ 0x00007ffa2c13cad0, /* type */ 0x04}, /* value */ 0x01};
  expected.static_fields[1] = {{/* name ID */ 0x00007ffa94009818, /* type */ 0x0a}, /* value */ 0x12abcdef};
  expected.instance_field_infos.extend_to(3);
  expected.instance_field_infos[0] = {/* name ID */ 0x00007ffa9016ad30, /* type */ 0x05};
  expected.instance_field_infos[1] = {/* name ID */ 0x00007ffa94009818, /* type */ 0x02};
  expected.instance_field_infos[2] = {/* name ID */ 0x00007ffa903c5af8, /* type */ 0x0b};

  auto *record = heap_dump.class_dump_records.get(expected.id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected.id, record->id);
  EXPECT_EQ(expected.stack_trace_serial, record->stack_trace_serial);
  EXPECT_EQ(expected.super_id, record->super_id);
  EXPECT_EQ(expected.class_loader_id, record->class_loader_id);
  EXPECT_EQ(expected.signers_id, record->signers_id);
  EXPECT_EQ(expected.protection_domain_id, record->protection_domain_id);
  EXPECT_EQ(expected.instance_size, record->instance_size);
  check_array_eq(expected.constant_pool, record->constant_pool, "Constant pool",
                 [&](const auto &l, const auto &r) {
                   return l.index == r.index && l.type == r.type &&
                          basic_value_eq(l.value, r.value, l.type);
                 });
  check_array_eq(expected.static_fields, record->static_fields, "Static fields",
                 [&](const auto &l, const auto &r) {
                   return l.info.name_id == r.info.name_id &&
                          l.info.type == r.info.type &&
                          basic_value_eq(l.value, r.value, l.info.type);
                 });
  check_array_eq(expected.instance_field_infos, record->instance_field_infos,
                 "Instance field infos", [&](const auto &l, const auto &r) {
                   return l.name_id == r.name_id && l.type == r.type;
                 });
}

static const char CONTENTS_INSTANCE_DUMP[] =
    "JAVA PROFILE 1.0.1\0"              // Header
    "\x00\x00\x00\x08"                  // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"  // Dump timestamp

    "\x0C"                              // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                  // Record timestamp
    "\x00\x00\x00\x1f"                  // Body size

    "\x21"                              // HPROF_GC_INSTANCE_DUMP tag

    "\x00\x00\x00\x06\xc7\x56\x78\x90"  // ID
    "\x87\x65\x43\x21"                  // stack trace serial
    "\x00\x00\x00\x06\xc7\x93\x73\xf8"  // class ID
    "\x00\x00\x00\x06"                  // following field bytes num
      "\x00\x00\x43\x21"                  // int field
      "\x67\x89"                          // short field
    ;

TEST_VM(HeapDumpParser, single_instance_dump_subrecord) {
  fill_test_file(CONTENTS_INSTANCE_DUMP, sizeof(CONTENTS_INSTANCE_DUMP) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 0, 1 /* instance dump */, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::InstanceDumpRecord expected = {
      0x00000006c7567890,  // ID
      0x87654321,          // stack trace serial
      0x00000006c79373f8   // class ID
  };
  expected.fields_data.extend_to(sizeof(jint) + sizeof(jshort));
  memcpy(expected.fields_data.mem(), "\x00\x00\x43\x21\x67\x89", sizeof(jint) + sizeof(jshort));

  auto *record = heap_dump.instance_dump_records.get(expected.id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected.id, record->id);
  EXPECT_EQ(expected.stack_trace_serial, record->stack_trace_serial);
  EXPECT_EQ(expected.class_id, record->class_id);
  check_array_eq(expected.fields_data, record->fields_data, "Fields data",
                 std::equal_to<u1>());
}

static const char CONTENTS_OBJ_ARRAY_DUMP[] =
    "JAVA PROFILE 1.0.1\0"              // Header
    "\x00\x00\x00\x04"                  // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"  // Dump timestamp

    "\x0C"                              // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                  // Record timestamp
    "\x00\x00\x00\x1d"                  // Body size

    "\x22"                              // HPROF_GC_OBJ_ARRAY_DUMP tag

    "\xc7\x89\x91\x24"                  // ID
    "\x13\x24\x35\x46"                  // stack trace serial
    "\x00\x00\x00\x03"                  // elements num
    "\xc7\x43\xab\xd8"                  // array class ID
      "\x12\x34\x56\x78"                  // elem 0 ID
      "\x9a\xbc\xde\xf4"                  // elem 1 ID
      "\x32\x10\xff\x60"                  // elem 2 ID
    ;

TEST_VM(HeapDumpParser, single_obj_array_dump_subrecord) {
  fill_test_file(CONTENTS_OBJ_ARRAY_DUMP, sizeof(CONTENTS_OBJ_ARRAY_DUMP) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 0, 0, 1 /* obj array dump */, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::ObjArrayDumpRecord expected = {
      0xc7899124,  // ID
      0x13243546,  // stack trace serial
      0xc743abd8   // array class ID
  };
  expected.elem_ids.extend_to(3);
  expected.elem_ids[0] = 0x12345678;
  expected.elem_ids[1] = 0x9abcdef4;
  expected.elem_ids[2] = 0x3210ff60;

  auto *record = heap_dump.obj_array_dump_records.get(expected.id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected.id, record->id);
  EXPECT_EQ(expected.stack_trace_serial, record->stack_trace_serial);
  EXPECT_EQ(expected.array_class_id, record->array_class_id);
  check_array_eq(expected.elem_ids, record->elem_ids, "Element IDs",
                 std::equal_to<HeapDumpFormat::id_t>());
}

static const char CONTENTS_PRIM_ARRAY_DUMP[] =
    "JAVA PROFILE 1.0.1\0"              // Header
    "\x00\x00\x00\x08"                  // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"  // Dump timestamp

    "\x0C"                              // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                  // Record timestamp
    "\x00\x00\x00\x16"                  // Body size

    "\x23"                              // HPROF_GC_PRIM_ARRAY_DUMP tag

    "\xfa\xbc\xde\xf0\x12\x34\x56\x78"  // ID
    "\x13\x24\x35\x46"                  // stack trace serial
    "\x00\x00\x00\x02"                  // elements num
    "\x09"                              // element type = short
      "\x12\x34"                          // elem 0
      "\xff\xff"                          // elem 1
    ;

TEST_VM(HeapDumpParser, single_prim_array_dump_subrecord) {
  fill_test_file(CONTENTS_PRIM_ARRAY_DUMP, sizeof(CONTENTS_PRIM_ARRAY_DUMP) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 0, 0, 0, 1 /* prim array dump */}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  HeapDumpFormat::PrimArrayDumpRecord expected = {
      0xfabcdef012345678,  // ID
      0x13243546,          // stack trace serial
      0x00000002,          // elements num
      0x09                 // element type
  };
  expected.elems_data.extend_to(expected.elems_num * sizeof(jshort));
  memcpy(expected.elems_data.mem(), "\x12\x34\xff\xff", expected.elems_num * sizeof(jshort));

  auto *record = heap_dump.prim_array_dump_records.get(expected.id);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  EXPECT_EQ(expected.id, record->id);
  EXPECT_EQ(expected.stack_trace_serial, record->stack_trace_serial);
  EXPECT_EQ(expected.elems_num, record->elems_num);
  EXPECT_EQ(expected.elem_type, record->elem_type);
  check_array_eq(expected.elems_data, record->elems_data, "Elements data",
                 std::equal_to<u1>());
}

static const char CONTENTS_BASIC_VALUES[] =
    "JAVA PROFILE 1.0.1\0"                // Header
    "\x00\x00\x00\x08"                    // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // Dump timestamp

    "\x0C"                                // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                    // Record timestamp
    "\x00\x00\x00\x88"                    // Body size

    "\x20"                                // HPROF_GC_CLASS_DUMP tag

    "\x00\x00\x00\x00\x00\x00\x00\x00"    // class ID
    "\x00\x00\x00\x00"                    // stack trace serial
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // super ID
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // class loader ID
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // signers ID
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // protection domain ID
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // reserved
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // reserved
    "\x00\x00\x00\x25"                    // instance size
    "\x00\x09"                            // constant pool size
      "\x00\x01"                            // index
      "\x02"                                // type = object
      "\x00\x00\x00\x06\xc7\x92\x53\x98"    // value

      "\x00\x02"                            // index
      "\x04"                                // type = boolean
      "\x01"                                // value = true

      "\x00\x03"                            // index
      "\x05"                                // type = char
      "\x00\x4a"                            // value = 'J'

      "\x00\x04"                            // index
      "\x06"                                // type = float
      "\x43\x40\x91\x80"                    // value = 192.568359375 (exact)

      "\x00\x05"                            // index
      "\x07"                                // type = double
      "\x43\x11\x8b\x54\xf2\x2a\xeb\x01"    // value = 1234567890123456.25 (exact)

      "\x00\x06"                            // index
      "\x08"                                // type = byte
      "\x79"                                // value = 121

      "\x00\x07"                            // index
      "\x09"                                // type = short
      "\x2f\x59"                            // value = 12121

      "\x00\x08"                            // index
      "\x0a"                                // type = int
      "\x07\x39\x8c\xd9"                    // value = 121212121

      "\x00\x09"                            // index
      "\x0b"                                // type = long
      "\x7f\xff\xff\xff\xff\xff\xff\xff"    // value = 9223372036854775807
    "\x00\x00"                            // static fields number
    "\x00\x00"                            // instance fields number
    ;

TEST_VM(HeapDumpParser, basic_values_get_right_values) {
  fill_test_file(CONTENTS_BASIC_VALUES, sizeof(CONTENTS_BASIC_VALUES) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 1 /* class dump */, 0, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  auto *record = heap_dump.class_dump_records.get(0);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  const auto &basic_values = record->constant_pool;

  EXPECT_EQ(static_cast<HeapDumpFormat::id_t>(0x00000006c7925398),  basic_values[0].value.as_object_id);
  EXPECT_EQ(static_cast<jboolean>            (true),                basic_values[1].value.as_boolean);
  EXPECT_EQ(static_cast<jchar>               ('J'),                 basic_values[2].value.as_char);
  EXPECT_EQ(static_cast<jfloat>              (192.568359375F),      basic_values[3].value.as_float);
  EXPECT_EQ(static_cast<jdouble>             (1234567890123456.25), basic_values[4].value.as_double);
  EXPECT_EQ(static_cast<jbyte>               (121),                 basic_values[5].value.as_byte);
  EXPECT_EQ(static_cast<jshort>              (12121),               basic_values[6].value.as_short);
  EXPECT_EQ(static_cast<jint>                (121212121),           basic_values[7].value.as_int);
  EXPECT_EQ(static_cast<jlong>               (9223372036854775807), basic_values[8].value.as_long);
}

static const char CONTENTS_SPECIAL_FLOATS[] =
    "JAVA PROFILE 1.0.1\0"              // Header
    "\x00\x00\x00\x04"                  // ID size
    "\x00\x00\x00\x00\x00\x00\x00\x00"  // Dump timestamp

    "\x0C"                              // HPROF_HEAP_DUMP tag
    "\x00\x00\x00\x00"                  // Record timestamp
    "\x00\x00\x00\x6A"                  // Body size

    "\x20"                              // HPROF_GC_CLASS_DUMP tag

    "\x00\x00\x00\x00"                  // class ID
    "\x00\x00\x00\x00"                  // stack trace serial
    "\x00\x00\x00\x00"                  // super ID
    "\x00\x00\x00\x00"                  // class loader ID
    "\x00\x00\x00\x00"                  // signers ID
    "\x00\x00\x00\x00"                  // protection domain ID
    "\x00\x00\x00\x00"                  // reserved
    "\x00\x00\x00\x00"                  // reserved
    "\x00\x00\x00\x24"                  // instance size
    "\x00\x09"                          // constant pool size 39
      "\x00\x01"                          // index
      "\x06"                              // type = float
      "\x43\x00\x00\x00"                  // value = 128 -- normal value

      "\x00\x02"                          // index
      "\x06"                              // type = float
      "\xc3\x00\x00\x00"                  // value = -128 -- normal value, sign bit set

      "\x00\x03"                          // index
      "\x06"                              // type = float
      "\x00\x00\x00\x00"                  // value = +0

      "\x00\x04"                          // index
      "\x06"                              // type = float
      "\x80\x00\x00\x00"                  // value = -0

      "\x00\x05"                          // index
      "\x06"                              // type = float
      "\x7f\x80\x00\x00"                  // value = +inf

      "\x00\x06"                          // index
      "\x06"                              // type = float
      "\xff\x80\x00\x00"                  // value = -inf

      "\x00\x07"                          // index
      "\x06"                              // type = float
      "\x7f\xff\xff\xff"                  // value = nan

      "\x00\x08"                          // index
      "\x06"                              // type = float
      "\xff\x80\x00\x01"                  // value = nan (another variant)

      "\x00\x09"                          // index
      "\x06"                              // type = float
      "\x7f\xc0\x00\x00"                  // value = nan (another variant)
    "\x00\x00"                          // static fields number
    "\x00\x00"                          // instance fields number
    ;

TEST_VM(HeapDumpParser, parsing_special_float_values) {
  fill_test_file(CONTENTS_SPECIAL_FLOATS, sizeof(CONTENTS_SPECIAL_FLOATS) - 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure());

  ParsedHeapDump heap_dump;
  const char *err_msg = HeapDumpParser::parse(TEST_FILENAME, &heap_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  check_record_amounts({0, 0, 1 /* class dump */, 0, 0, 0}, heap_dump);
  ASSERT_FALSE(::testing::Test::HasNonfatalFailure()) << "Unexpected amounts of records parsed";

  auto *record = heap_dump.class_dump_records.get(0);
  ASSERT_NE(nullptr, record) << "Record not found under the expected ID";

  const auto &floats = record->constant_pool;

  STATIC_ASSERT(std::numeric_limits<jfloat>::is_iec559);

  EXPECT_EQ(128.0F,                                       floats[0].value.as_float);
  EXPECT_EQ(-128.0F,                                      floats[1].value.as_float);
  EXPECT_EQ(0.0F,                                         floats[2].value.as_float);
  EXPECT_EQ(-0.0F,                                        floats[3].value.as_float);
  EXPECT_EQ(std::numeric_limits<jfloat>::infinity(),      floats[4].value.as_float);
  EXPECT_EQ(-std::numeric_limits<jfloat>::infinity(),     floats[5].value.as_float);
  EXPECT_PRED1(static_cast<bool (*)(jfloat)>(std::isnan), floats[6].value.as_float);
  EXPECT_PRED1(static_cast<bool (*)(jfloat)>(std::isnan), floats[7].value.as_float);
  EXPECT_PRED1(static_cast<bool (*)(jfloat)>(std::isnan), floats[8].value.as_float);
}
