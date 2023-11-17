#include "precompiled.hpp"
#include "memory/resourceArea.hpp"
#include "unittest.hpp"
#include "utilities/stackDumpParser.hpp"
#include "utilities/stackDumper.hpp"

constexpr char TEST_FILENAME[] = "stackDumpParser_test.hprof";

static void fill_test_file(const char *contents, size_t size) {
  FILE *file = os::fopen(TEST_FILENAME, "wb");
  ASSERT_NE(nullptr, file) << "Cannot open " << TEST_FILENAME << " for writing: " << os::strerror(errno);
  EXPECT_EQ(1U, fwrite(contents, size, 1, file)) << "Cannot write test data into " << TEST_FILENAME << ": " << os::strerror(errno);
  ASSERT_EQ(0, fclose(file)) << "Cannot close the test file: " << os::strerror(errno);
}

static void check_stack_values(const ExtendableArray<StackTrace::Frame::Value, u2> &expected_values,
                               const ExtendableArray<StackTrace::Frame::Value, u2> &actual_values) {
  ASSERT_EQ(expected_values.size(), actual_values.size());
  for (u2 i = 0; i < expected_values.size(); i++) {
    const StackTrace::Frame::Value &expected = expected_values[i];
    const StackTrace::Frame::Value &actual = actual_values[i];
    EXPECT_EQ(expected.type, actual.type) << "Wrong type of value #" << i;
    if (expected.type == DumpedStackValueType::REFERENCE) {
      EXPECT_EQ(expected.obj_id, actual.obj_id) << "Wrong obj ref #" << i;
    } else {
      EXPECT_EQ(expected.prim, actual.prim) << "Wrong primitive #" << i;
    }
  }
}

static void check_stack_frames(const StackTrace &expected_trace,
                               const StackTrace &actual_trace) {
  EXPECT_EQ(expected_trace.thread_id(), actual_trace.thread_id());
  ASSERT_EQ(expected_trace.frames_num(), expected_trace.frames_num());

  for (u4 i = 0; i < expected_trace.frames_num(); i++) {
    const StackTrace::Frame &expected_frame = expected_trace.frames(i);
    const StackTrace::Frame &actual_frame = actual_trace.frames(i);

    EXPECT_EQ(expected_frame.method_name_id, actual_frame.method_name_id);
    EXPECT_EQ(expected_frame.method_sig_id,  actual_frame.method_sig_id);
    EXPECT_EQ(expected_frame.class_id,       actual_frame.class_id);
    EXPECT_EQ(expected_frame.bci,            actual_frame.bci);

    check_stack_values(expected_frame.locals,   actual_frame.locals);
    EXPECT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong locals parsing in frame " << i;
    check_stack_values(expected_frame.operands, actual_frame.operands);
    EXPECT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong operands parsing in frame " << i;
    // TODO check monitors after they are added
  }
}

static constexpr char CONTENTS_NO_TRACES[] =
    "JAVA STACK DUMP 0.1\0" // Header
    "\x00\x04"              // ID size
    ;

TEST_VM(StackDumpParser, no_stack_traces) {
  fill_test_file(CONTENTS_NO_TRACES, sizeof(CONTENTS_NO_TRACES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.id_size());
  EXPECT_EQ(0, stack_dump.stack_traces().length());
}

static constexpr char CONTENTS_EMPTY_TRACE[] =
    "JAVA STACK DUMP 0.1\0" // Header
    "\x00\x04"              // ID size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x00"      // Number of frames
    ;

TEST_VM(StackDumpParser, empty_stack_trace) {
  fill_test_file(CONTENTS_EMPTY_TRACE, sizeof(CONTENTS_EMPTY_TRACE) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.id_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  StackTrace expected_trace(/* thread ID */ 0xabcdef95, /* frames num */ 0);

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_NO_STACK_VALUES[] =
    "JAVA STACK DUMP 0.1\0" // Header
    "\x00\x04"              // ID size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x01"      // Number of frames
      "\x12\x34\x56\x78"      // Method name ID
      "\x87\x65\x43\x21"      // Method signature ID
      "\x87\x65\x43\x22"      // Class ID
      "\x12\x34"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x00"              // Operands num
      "\x00\x00"              // Monitors num
    ;

TEST_VM(StackDumpParser, stack_frame_with_no_stack_values) {
  fill_test_file(CONTENTS_NO_STACK_VALUES, sizeof(CONTENTS_NO_STACK_VALUES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.id_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  StackTrace expected_trace(/* thread ID */ 0xabcdef95, /* frames num */ 1);

  auto &expected_frame = expected_trace.frames(0);
  expected_frame.method_name_id = 0x12345678;
  expected_frame.method_sig_id = 0x87654321;
  expected_frame.class_id = 0x87654322;
  expected_frame.bci = 0x1234;

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_CORRECT_STACK_VALUES[] =
    "JAVA STACK DUMP 0.1\0"              // Header
    "\x00\x08"                           // ID size

    "\xab\xcd\xef\x95\xba\xdc\xfe\x96"   // Thread ID
    "\x00\x00\x00\x01"                   // Number of frames
      "\x12\x34\x56\x78\x01\x23\x45\x67"   // Method name ID
      "\x87\x65\x12\x34\x56\x78\x43\x21"   // Method signature ID
      "\x87\x65\x43\x12\x34\x56\x78\x22"   // Class ID
      "\x12\x34"                           // BCI
      "\x00\x03"                           // Locals num
        "\x00"                               // Type = 4-byte primitive
        "\xab\xcd\xef\xab"                   // Value
        "\x01"                               // Type = half of an 8-byte primitive
        "\x01\x23\x45\x67"                   // Value
        "\x01"                               // Type = half of an 8-byte primitive
        "\x89\xab\xcd\xef"                   // Value
      "\x00\x02"                           // Operands num
        "\x02"                               // Type = object reference
        "\x00\x00\x7f\xfa\x40\x05\x65\x50"   // Value
        "\x00"                               // Type = 4-byte primitive
        "\x56\x78\x90\xab"                   // Value
      "\x00\x00"                           // Monitors num
    ;

TEST_VM(StackDumpParser, stack_frame_with_correct_stack_values) {
  fill_test_file(CONTENTS_CORRECT_STACK_VALUES, sizeof(CONTENTS_CORRECT_STACK_VALUES) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(8U, stack_dump.id_size());
  ASSERT_EQ(1, stack_dump.stack_traces().length());

  StackTrace expected_trace(/* thread ID */ 0xabcdef95badcfe96, /* frames num */ 1);

  auto &expected_frame = expected_trace.frames(0);
  expected_frame.method_name_id = 0x1234567801234567;
  expected_frame.method_sig_id = 0x8765123456784321;
  expected_frame.class_id = 0x8765431234567822;
  expected_frame.bci = 0x1234;
  expected_frame.locals.extend(3);
  expected_frame.locals[0] = {DumpedStackValueType::PRIMITIVE, {0xabcdefab}};
  expected_frame.locals[1] = {DumpedStackValueType::PRIMITIVE_HALF, {0x01234567}};
  expected_frame.locals[2] = {DumpedStackValueType::PRIMITIVE_HALF, {0x89abcdef}};
  expected_frame.operands.extend(2);
  expected_frame.operands[0].type = DumpedStackValueType::REFERENCE;
  expected_frame.operands[0].obj_id = 0x00007ffa40056550; // Cannot set via braced init
  expected_frame.operands[1] = {DumpedStackValueType::PRIMITIVE, {0x567890ab}};

  check_stack_frames(expected_trace, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());
}

static constexpr char CONTENTS_UNMATCHED_PRIM_HALF[] =
    "JAVA STACK DUMP 0.1\0" // Header
    "\x00\x04"              // ID size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x01"      // Number of frames
      "\x12\x34\x56\x78"      // Method name ID
      "\x87\x65\x43\x21"      // Method signature ID
      "\x87\x65\x43\x22"      // Class ID
      "\x12\x34"              // BCI
      "\x00\x02"              // Locals num
        "\x01"                  // Type = half of an 8-byte primitive
        "\x01\x23\x45\x67"      // Value
        "\x00"                  // Type = 4-byte primitive
        "\xab\xcd\xef\xab"      // Value
      "\x00\x00"              // Operands num
      "\x00\x00"              // Monitors num
    ;

TEST_VM(StackDumpParser, stack_frame_with_unmatched_primitive_half_local) {
  fill_test_file(CONTENTS_UNMATCHED_PRIM_HALF, sizeof(CONTENTS_UNMATCHED_PRIM_HALF) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_NE(nullptr, err_msg) << "Parsing was expected to fail but didn't";
}

static constexpr char CONTENTS_MULTIPLE_STACKS[] =
    "JAVA STACK DUMP 0.1\0" // Header
    "\x00\x04"              // ID size

    "\xab\xcd\xef\x95"      // Thread ID
    "\x00\x00\x00\x02"      // Number of frames
      "\xab\xac\xab\xaa"      // Method name ID
      "\xba\xba\xfe\xda"      // Method signature ID
      "\x87\x65\x43\x21"      // Class ID
      "\x00\x05"              // BCI
      "\x00\x01"              // Locals num
        "\x00"                  // Type = 4-byte primitive
        "\xab\xcd\xef\xab"      // Value
      "\x00\x00"              // Operands num
      "\x00\x00"              // Monitors num

      "\xba\xca\xba\xca"      // Method name ID
      "\xcc\xdd\xbb\xaf"      // Method signature ID
      "\x01\x23\x78\x32"      // Class ID
      "\x00\x10"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x00"              // Operands num
      "\x00\x00"              // Monitors num

    "\x00\x11\x32\x09"      // Thread ID
    "\x00\x00\x00\x01"      // Number of frames
      "\xfe\xfe\xca\xca"      // Method name ID
      "\x34\x43\x78\x22"      // Method signature ID
      "\x21\x21\x74\x55"      // Class ID
      "\x00\xfa"              // BCI
      "\x00\x00"              // Locals num
      "\x00\x02"              // Operands num
        "\x01"                  // Type = half of an 8-byte primitive
        "\x01\x23\x45\x67"      // Value
        "\x01"                  // Type = half of an 8-byte primitive
        "\x89\xab\xcd\xef"      // Value
      "\x00\x00"              // Monitors num
    ;

TEST_VM(StackDumpParser, multiple_stacks_dumped) {
  fill_test_file(CONTENTS_MULTIPLE_STACKS, sizeof(CONTENTS_MULTIPLE_STACKS) - 1);
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure());

  ResourceMark rm;
  ParsedStackDump stack_dump;
  const char *err_msg = StackDumpParser::parse(TEST_FILENAME, &stack_dump);
  ASSERT_EQ(nullptr, err_msg) << "Parsing error: " << err_msg;

  EXPECT_EQ(4U, stack_dump.id_size());
  ASSERT_EQ(2, stack_dump.stack_traces().length());

  StackTrace expected_trace_1(/* thread ID */ 0xabcdef95, /* frames num */ 2);

  expected_trace_1.frames(0).method_name_id = 0xabacabaa;
  expected_trace_1.frames(0).method_sig_id = 0xbabafeda;
  expected_trace_1.frames(0).class_id = 0x87654321;
  expected_trace_1.frames(0).bci = 5;
  expected_trace_1.frames(0).locals.extend(1);
  expected_trace_1.frames(0).locals[0] = {DumpedStackValueType::PRIMITIVE, {0xabcdefab}};

  expected_trace_1.frames(1).method_name_id = 0xbacabaca;
  expected_trace_1.frames(1).method_sig_id = 0xccddbbaf;
  expected_trace_1.frames(1).class_id = 0x01237832;
  expected_trace_1.frames(1).bci = 0x10;

  check_stack_frames(expected_trace_1, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong parsing of trace #1";

  StackTrace expected_trace_2(/* thread ID */ 0x00113209, /* frames num */ 1);

  expected_trace_2.frames(0).method_name_id = 0xfefecaca;
  expected_trace_2.frames(0).method_sig_id = 0x34437822;
  expected_trace_2.frames(0).class_id = 0x21217455;
  expected_trace_2.frames(0).bci = 0xfa;
  expected_trace_2.frames(0).operands.extend(2);
  expected_trace_2.frames(0).operands[0] = {DumpedStackValueType::PRIMITIVE_HALF, {0x01234567}};
  expected_trace_2.frames(0).operands[1] = {DumpedStackValueType::PRIMITIVE_HALF, {0x89abcdef}};

  check_stack_frames(expected_trace_1, *stack_dump.stack_traces().at(0));
  ASSERT_FALSE(testing::Test::HasFatalFailure() || testing::Test::HasNonfatalFailure()) << "Wrong parsing of trace #2";
}
