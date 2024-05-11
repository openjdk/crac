#include "precompiled.hpp"
#include "unittest.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/methodKind.hpp"

constexpr char TEST_FILENAME[] = "stackDumpParser_test.hprof";

static void fill_test_file(const char *contents, size_t size) {
  FILE *file = os::fopen(TEST_FILENAME, "wb");
  ASSERT_NE(nullptr, file) << "Cannot open " << TEST_FILENAME << " for writing: " << os::strerror(errno);
  EXPECT_EQ(1U, fwrite(contents, size, 1, file)) << "Cannot write test data into " << TEST_FILENAME << ": " << os::strerror(errno);
  ASSERT_EQ(0, fclose(file)) << "Cannot close the test file: " << os::strerror(errno);
}

static void check_stack_values(const GrowableArrayView<CracStackTrace::Frame::Value> &expected_values,
                               const GrowableArrayView<CracStackTrace::Frame::Value> &actual_values) {
  ASSERT_EQ(expected_values.length(), actual_values.length());
  for (int i = 0; i < expected_values.length(); i++) {
    const CracStackTrace::Frame::Value &expected = expected_values.at(i);
    const CracStackTrace::Frame::Value &actual = actual_values.at(i);
    ASSERT_NE(expected.type(), CracStackTrace::Frame::Value::Type::EMPTY); // Sanity check
    ASSERT_NE(expected.type(), CracStackTrace::Frame::Value::Type::OBJ);   // Sanity check
    EXPECT_EQ(expected.type(), actual.type()) << "Wrong type of value #" << i;
    if (expected.type() == CracStackTrace::Frame::Value::Type::REF) {
      EXPECT_EQ(expected.as_obj_id(), actual.as_obj_id()) << "Wrong obj ref #" << i;
    } else {
      EXPECT_EQ(expected.as_primitive(), actual.as_primitive()) << "Wrong primitive #" << i;
    }
  }
}

static void check_stack_frames(const CracStackTrace &expected_trace,
                               const CracStackTrace &actual_trace) {
  EXPECT_EQ(expected_trace.thread_id(), actual_trace.thread_id());
  ASSERT_EQ(expected_trace.frames_num(), expected_trace.frames_num());

  for (u4 i = 0; i < expected_trace.frames_num(); i++) {
    const CracStackTrace::Frame &expected_frame = expected_trace.frame(i);
    const CracStackTrace::Frame &actual_frame = actual_trace.frame(i);

    EXPECT_EQ(expected_frame.method_name_id(),   actual_frame.method_name_id());
    EXPECT_EQ(expected_frame.method_sig_id(),    actual_frame.method_sig_id());
    EXPECT_EQ(expected_frame.method_kind(),      actual_frame.method_kind());
    EXPECT_EQ(expected_frame.method_holder_id(), actual_frame.method_holder_id());
    EXPECT_EQ(expected_frame.bci(),              actual_frame.bci());

    check_stack_values(expected_frame.locals(),   actual_frame.locals());
    EXPECT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong locals parsing in frame " << i;
    check_stack_values(expected_frame.operands(), actual_frame.operands());
    EXPECT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong operands parsing in frame " << i;
    check_stack_values(expected_frame.monitor_owners(), actual_frame.monitor_owners());
    EXPECT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong monitors parsing in frame " << i;
  }
}

static constexpr char CONTENTS_NO_TRACES[] =
    "CRAC STACK DUMP 0.1\0" // Header
    "\x00\x04"              // Word size
    ;

TEST(CracStackDumpParser, no_stack_traces) {
  fill_test_file(CONTENTS_NO_TRACES, sizeof(CONTENTS_NO_TRACES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ParsedCracStackDump stack_dump;
  const char *err_msg = CracStackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.word_size());
  EXPECT_EQ(0, stack_dump.stack_traces().length());
}

static constexpr char CONTENTS_EMPTY_TRACE[] =
    "CRAC STACK DUMP 0.1\0" // Header
    "\x00\x04"              // Word size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x00"      // Number of frames
    ;

TEST(CracStackDumpParser, empty_stack_trace) {
  fill_test_file(CONTENTS_EMPTY_TRACE, sizeof(CONTENTS_EMPTY_TRACE) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ParsedCracStackDump stack_dump;
  const char *err_msg = CracStackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.word_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  CracStackTrace expected_trace(/* thread ID */ 0xabcdef95, /* frames num */ 0);

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_NO_STACK_VALUES[] =
    "CRAC STACK DUMP 0.1\0" // Header
    "\x00\x04"              // Word size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x01"      // Number of frames
      "\x12\x34\x56\x78"      // Method name ID
      "\x87\x65\x43\x21"      // Method signature ID
      "\x00"                  // Method kind - static
      "\x87\x65\x43\x22"      // Class ID
      "\x12\x34"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x00"              // Operands num
      "\x00\x00\x00\x00"      // Monitors num
    ;

TEST(CracStackDumpParser, stack_frame_with_no_stack_values) {
  fill_test_file(CONTENTS_NO_STACK_VALUES, sizeof(CONTENTS_NO_STACK_VALUES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ParsedCracStackDump stack_dump;
  const char *err_msg = CracStackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.word_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  CracStackTrace expected_trace(/* thread ID */ 0xabcdef95, /* frames num */ 1);

  auto &expected_frame = expected_trace.frame(0);
  expected_frame.set_method_name_id(0x12345678);
  expected_frame.set_method_sig_id(0x87654321);
  expected_frame.set_method_kind(MethodKind::Enum::STATIC);
  expected_frame.set_method_holder_id(0x87654322);
  expected_frame.set_bci(0x1234);

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_CORRECT_STACK_VALUES[] =
    "CRAC STACK DUMP 0.1\0"              // Header
    "\x00\x08"                           // Word size

    "\xab\xcd\xef\x95\xba\xdc\xfe\x96"   // Thread ID
    "\x00\x00\x00\x01"                   // Number of frames
      "\x12\x34\x56\x78\x01\x23\x45\x67"   // Method name ID
      "\x87\x65\x12\x34\x56\x78\x43\x21"   // Method signature ID
      "\x01"                               // Method kind = instance
      "\x87\x65\x43\x12\x34\x56\x78\x22"   // Class ID
      "\x12\x34"                           // BCI
      "\x00\x03"                           // Locals num
        "\x00"                               // Type = primitive
        "\x00\x00\x00\x00\xab\xcd\xef\xab"   // Value
        "\x00"                               // Type = primitive
        "\xde\xad\xde\xaf\x00\x00\x00\x00"   // Value
        "\x00"                               // Type = primitive
        "\x01\x23\x45\x67\x89\xab\xcd\xef"   // Value
      "\x00\x02"                           // Operands num
        "\x01"                               // Type = object reference
        "\x00\x00\x7f\xfa\x40\x05\x65\x50"   // Value
        "\x00"                               // Type = primitive
        "\x00\x00\x00\x00\x56\x78\x90\xab"   // Value
      "\x00\x00\x00\x01"                   // Monitors num
        "\x00\x00\x7f\xfa\x40\x05\x65\x50"   // Monitor owner ID
    ;

TEST(CracStackDumpParser, stack_frame_with_correct_stack_values) {
  fill_test_file(CONTENTS_CORRECT_STACK_VALUES, sizeof(CONTENTS_CORRECT_STACK_VALUES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ParsedCracStackDump stack_dump;
  const char *err_msg = CracStackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(8U, stack_dump.word_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  CracStackTrace expected_trace(/* thread ID */ 0xabcdef95badcfe96, /* frames num */ 1);

  auto &expected_frame = expected_trace.frame(0);
  expected_frame.set_method_name_id(0x1234567801234567);
  expected_frame.set_method_sig_id(0x8765123456784321);
  expected_frame.set_method_kind(MethodKind::Enum::INSTANCE);
  expected_frame.set_method_holder_id(0x8765431234567822);
  expected_frame.set_bci(0x1234);
  expected_frame.locals().append(CracStackTrace::Frame::Value::of_primitive(0x00000000abcdefab));
  expected_frame.locals().append(CracStackTrace::Frame::Value::of_primitive(0xdeaddeaf00000000));
  expected_frame.locals().append(CracStackTrace::Frame::Value::of_primitive(0x0123456789abcdef));
  expected_frame.operands().append(CracStackTrace::Frame::Value::of_obj_id(0x00007ffa40056550));
  expected_frame.operands().append(CracStackTrace::Frame::Value::of_primitive(0x00000000567890ab));
  expected_frame.monitor_owners().append(CracStackTrace::Frame::Value::of_obj_id(0x00007ffa40056550));

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_MULTIPLE_STACKS[] =
    "CRAC STACK DUMP 0.1\0" // Header
    "\x00\x04"              // Word size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x02"      // Number of frames
      "\xab\xac\xab\xaa"      // Method name ID
      "\xba\xba\xfe\xda"      // Method signature ID
      "\x00"                  // Method kind = static
      "\x87\x65\x43\x21"      // Class ID
      "\x00\x05"              // BCI
      "\x00\x01"              // Locals num
        "\x00"                  // Type = 4-byte primitive
        "\xab\xcd\xef\xab"      // Value
      "\x00\x00"              // Operands num
      "\x00\x00\x00\x03"      // Monitors num
        "\x7f\xab\xcd\x35"      // Monitor owner ID
        "\x7f\xcd\x01\x23"      // Monitor owner ID
        "\x7f\xef\x45\x67"      // Monitor owner ID

      "\xba\xca\xba\xca"      // Method name ID
      "\xcc\xdd\xbb\xaf"      // Method signature ID
      "\x01"                  // Method kind = instance
      "\x01\x23\x78\x32"      // Class ID
      "\x00\x10"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x00"              // Operands num
      "\x00\x00\x00\x00"      // Monitors num

    "\x00\x11\x32\x09"      // Thread ID
    "\x00\x00\x00\x01"      // Number of frames
      "\xfe\xfe\xca\xca"      // Method name ID
      "\x34\x43\x78\x22"      // Method signature ID
      "\x02"                  // Method kind = overpass
      "\x21\x21\x74\x55"      // Class ID
      "\x00\xfa"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x02"              // Operands num
        "\x01"                  // Type = primitive
        "\x01\x23\x45\x67"      // Value
        "\x01"                  // Type = primitive
        "\x89\xab\xcd\xef"      // Value
      "\x00\x00\x00\x00"      // Monitors num
    ;

TEST(CracStackDumpParser, multiple_stacks_dumped) {
  fill_test_file(CONTENTS_MULTIPLE_STACKS, sizeof(CONTENTS_MULTIPLE_STACKS) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ParsedCracStackDump stack_dump;
  const char *err_msg = CracStackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.word_size());
  ASSERT_EQ(2, stack_dump.stack_traces().length());

  // Frames are dumped from youngest to oldest but stored in reverse (so that
  // the youngest is on top), so the frame indices are reversed here
  CracStackTrace expected_trace_1(/* thread ID */ 0xabcdef95, /* frames num */ 2);
  // First in the dump, last in the parsed array
  expected_trace_1.frame(1).set_method_name_id(0xabacabaa);
  expected_trace_1.frame(1).set_method_sig_id(0xbabafeda);
  expected_trace_1.frame(1).set_method_kind(MethodKind::Enum::STATIC);
  expected_trace_1.frame(1).set_method_holder_id(0x87654321);
  expected_trace_1.frame(1).set_bci(5);
  expected_trace_1.frame(1).locals().append(CracStackTrace::Frame::Value::of_primitive(0xabcdefab));
  expected_trace_1.frame(1).monitor_owners().append(CracStackTrace::Frame::Value::of_obj_id(0x7fabcd35));
  expected_trace_1.frame(1).monitor_owners().append(CracStackTrace::Frame::Value::of_obj_id(0x7fcd0123));
  expected_trace_1.frame(1).monitor_owners().append(CracStackTrace::Frame::Value::of_obj_id(0x7fef4567));
  // Last in the dump, first in the parsed array
  expected_trace_1.frame(0).set_method_name_id(0xbacabaca);
  expected_trace_1.frame(0).set_method_sig_id(0xccddbbaf);
  expected_trace_1.frame(0).set_method_kind(MethodKind::Enum::INSTANCE);
  expected_trace_1.frame(0).set_method_holder_id(0x01237832);
  expected_trace_1.frame(0).set_bci(0x10);

  check_stack_frames(expected_trace_1, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong parsing of trace #1";

  CracStackTrace expected_trace_2(/* thread ID */ 0x00113209, /* frames num */ 1);

  expected_trace_2.frame(0).set_method_name_id(0xfefecaca);
  expected_trace_2.frame(0).set_method_sig_id(0x34437822);
  expected_trace_2.frame(0).set_method_kind(MethodKind::Enum::OVERPASS);
  expected_trace_2.frame(0).set_method_holder_id(0x21217455);
  expected_trace_2.frame(0).set_bci(0xfa);
  expected_trace_2.frame(0).operands().append(CracStackTrace::Frame::Value::of_primitive(0x01234567));
  expected_trace_2.frame(0).operands().append(CracStackTrace::Frame::Value::of_primitive(0x89abcdef));

  check_stack_frames(expected_trace_1, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong parsing of trace #2";
}
