#include "precompiled.hpp"
#include "classfile/vmSymbols.hpp"
#include "oops/symbol.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/heapDumpClasses.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/macros.hpp"
#ifdef ASSERT
#include "memory/resourceArea.hpp"
#include "utilities/resourceHash.hpp"
#endif // ASSERT

static bool symbol_equals(const Symbol *actual, const Symbol *expected) {
  return actual == expected;
}
static bool symbol_equals(const Symbol *actual, const char *expected) {
  return actual->equals(expected);
}

#define NO_DUMP_FIELDS_DO(...)

#define DEFINE_INSTANCE_DUMP_CHECK(class_name)                                                                        \
  bool HeapDumpClasses::is_##class_name##_dump(const ParsedHeapDump &heap_dump, const HeapDump::InstanceDump &dump) { \
    ResourceMark rm;                                                                                                  \
    ResourceHashtable<HeapDump::ID, bool> visited_classes;                                                            \
                                                                                                                      \
    for (const HeapDump::ClassDump *class_dump = &heap_dump.get_class_dump(dump.class_id);                            \
         class_dump->super_id != HeapDump::NULL_ID; class_dump = &heap_dump.get_class_dump(class_dump->super_id)) {   \
      bool is_first_visit;                                                                                            \
      visited_classes.put_if_absent(class_dump->id, &is_first_visit);                                                 \
      assert(is_first_visit, "circularity detected in class hierarchy of " HDID_FORMAT, dump.class_id);               \
                                                                                                                      \
      if (is_##class_name##_class_dump(heap_dump, *class_dump)) {                                                     \
        return true;                                                                                                  \
      }                                                                                                               \
    }                                                                                                                 \
                                                                                                                      \
    return false;                                                                                                     \
  }

STATIC_ASSERT((std::is_same<u4, juint>()));
#define DEFINE_OFFSET_FROM_START_LOCAL(klass, name, ...) u4 name##_offset_from_start = max_juint;

#define CREATE_FIELD_OFFSET_CASE(klass, name, name_sym, basic_type, ...)                                              \
  if (symbol_equals(field_name, name_sym)) {                                                                                   \
    guarantee(name##_offset_from_start == max_juint, "non-static field %s::%s dumped multiple times in " HDID_FORMAT,          \
              vmSymbols::klass()->as_klass_external_name(), field_name->as_C_string(), klass##_dump.id);                       \
    guarantee(field_type == (basic_type), "illegal type of non-static field %s::%s (ID " HDID_FORMAT "): expected %s, got %s", \
              vmSymbols::klass()->as_klass_external_name(), field_name->as_C_string(), klass##_dump.id,                        \
              type2name(basic_type), type2name(field_type));                                                                   \
    name##_offset_from_start = total_offset_from_start;                                                                        \
  } else

#define CREATE_PTR_FIELD_OFFSET_CASE(klass, name, name_sym, ...)                                                           \
  if (symbol_equals(field_name, name_sym)) {                                                                               \
    guarantee(name##_offset_from_start == max_juint, "non-static field %s::%s dumped multiple times in " HDID_FORMAT,      \
              vmSymbols::klass()->as_klass_external_name(), field_name->as_C_string(), klass##_dump.id);                   \
    const u4 field_size = is_java_primitive(field_type) ? type2aelembytes(field_type) : heap_dump.id_size;                 \
    name##_offset_from_start = total_offset_from_start;                                                                    \
    if (_ptr_type == T_ILLEGAL) {                                                                                          \
      guarantee(field_type == T_INT || field_type == T_LONG,                                                               \
                "illegal type of non-static raw pointer field %s::%s (ID " HDID_FORMAT "): expected int or long, got %s",  \
                vmSymbols::klass()->as_klass_external_name(), field_name->as_C_string(), klass##_dump.id,                  \
                type2name(field_type));                                                                                    \
      _ptr_type = field_type;                                                                                              \
    } else {                                                                                                               \
      precond(_ptr_type == T_INT || _ptr_type == T_LONG);                                                                  \
      guarantee(field_type == _ptr_type, "%s object " HDID_FORMAT " has non-static raw pointer fields of different types", \
                vmSymbols::klass()->as_klass_external_name(), klass##_dump.id);                                            \
    }                                                                                                                      \
  } else

#define CHECK_FIELD_FOUND(klass, name, ...) name##_offset_from_start < max_juint &&

#define SET_FIELD_OFFSET(klass, name, ...) _##name##_offset = total_offset_from_start - name##_offset_from_start;

#define INITIALIZE_OFFSETS(klass, FIELDS_DO, PTR_FIELDS_DO)                                                     \
  FIELDS_DO(DEFINE_OFFSET_FROM_START_LOCAL)                                                                     \
  PTR_FIELDS_DO(DEFINE_OFFSET_FROM_START_LOCAL)                                                                 \
  u4 total_offset_from_start = 0;                                                                               \
  for (u2 i = 0; i < klass##_dump.instance_field_infos.size(); i++) {                                           \
    const HeapDump::ClassDump::Field::Info &field_info = klass##_dump.instance_field_infos[i];                  \
    const BasicType field_type = HeapDump::htype2btype(field_info.type);                                        \
    const Symbol *field_name = heap_dump.get_symbol(field_info.name_id);                                        \
    FIELDS_DO(CREATE_FIELD_OFFSET_CASE)                                                                         \
    PTR_FIELDS_DO(CREATE_PTR_FIELD_OFFSET_CASE)                                                                 \
    { /* final else branch */ }                                                                                 \
    total_offset_from_start += is_java_primitive(field_type) ? type2aelembytes(field_type) : heap_dump.id_size; \
  }                                                                                                             \
  guarantee(FIELDS_DO(CHECK_FIELD_FOUND) PTR_FIELDS_DO(CHECK_FIELD_FOUND) /* && */ true,                        \
            "some non-static fields are missing from %s class dump " HDID_FORMAT,                               \
            vmSymbols::klass()->as_klass_external_name(), klass##_dump.id);                                     \
  FIELDS_DO(SET_FIELD_OFFSET)                                                                                   \
  PTR_FIELDS_DO(SET_FIELD_OFFSET)

#define ASSERT_INITIALIZED_WITH_SAME_ID(klass)                                                             \
  precond(_##klass##_id != HeapDump::NULL_ID);                                                             \
  assert(_##klass##_id == klass##_id,                                                                      \
         "%s class dump already found with different ID: old ID = " HDID_FORMAT ", new ID = " HDID_FORMAT, \
         vmSymbols::klass()->as_klass_external_name(), _##klass##_id, klass##_id);

#define DEFINE_GET_FIELD_METHOD(klass, name, name_sym, basic_type, c_type, type_name)                                          \
  c_type HeapDumpClasses::klass::name(const HeapDump::InstanceDump &dump) const {                                              \
    precond(is_initialized() && _##name##_offset >= (is_java_primitive(basic_type) ? type2aelembytes(basic_type) : _id_size)); \
    guarantee(dump.fields_data.size() >= _##name##_offset,                                                                     \
              "%s object " HDID_FORMAT " has not enough non-static field data to store its '" #name "' field",              \
              vmSymbols::klass()->as_klass_external_name(), dump.id);                                                          \
    return dump.read_field(dump.fields_data.size() - _##name##_offset, basic_type, _id_size).as_##type_name;                   \
  }

#define DEFINE_GET_PTR_FIELD_METHOD(klass, name, name_sym)                                                             \
  jlong HeapDumpClasses::java_lang_Class::name(const HeapDump::InstanceDump &dump) const {                             \
    precond(is_initialized() && _##name##_offset >= checked_cast<u4>(type2aelembytes(_ptr_type)));                     \
    guarantee(dump.fields_data.size() >= _##name##_offset,                                                             \
              "%s object " HDID_FORMAT " has not enough non-static field data to store its '" #name "' field",         \
              vmSymbols::klass()->as_klass_external_name(), dump.id);                                                  \
    const HeapDump::BasicValue val = dump.read_field(dump.fields_data.size() - _##name##_offset, _ptr_type, _id_size); \
    switch (_ptr_type) {                                                                                               \
      case T_INT:  return val.as_int;                                                                                  \
      case T_LONG: return val.as_long;                                                                                 \
      default:                                                                                                         \
        ShouldNotReachHere();                                                                                          \
        return 0;                                                                                                      \
    }                                                                                                                  \
  }

// java.lang.ClassLoader

#ifdef ASSERT
static bool is_class_loader_class_dump(const ParsedHeapDump &heap_dump, const HeapDump::ClassDump &dump) {
  const bool has_right_name_and_loader = heap_dump.get_class_name(dump.id) == vmSymbols::java_lang_ClassLoader() &&
                                         dump.class_loader_id == HeapDump::NULL_ID;
  if (!has_right_name_and_loader) {
    return false;
  }

  assert(dump.super_id != HeapDump::NULL_ID, "illegal super in %s dump " HDID_FORMAT ": expected %s, got none",
         vmSymbols::java_lang_ClassLoader()->as_klass_external_name(), dump.id,
         vmSymbols::java_lang_Object()->as_klass_external_name());

  const HeapDump::ClassDump &super_dump = heap_dump.get_class_dump(dump.super_id);
  assert(heap_dump.get_class_name(super_dump.id) == vmSymbols::java_lang_Object() &&
         super_dump.class_loader_id == HeapDump::NULL_ID, "illegal super in %s dump " HDID_FORMAT ": expected %s, got %s",
         vmSymbols::java_lang_ClassLoader()->as_klass_external_name(), dump.id,
         heap_dump.get_class_name(super_dump.id)->as_klass_external_name(),
         vmSymbols::java_lang_Object()->as_klass_external_name());

  return true;
}

DEFINE_INSTANCE_DUMP_CHECK(class_loader)
#endif // ASSERT

void HeapDumpClasses::java_lang_ClassLoader::ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_ClassLoader_id) {
  precond(java_lang_ClassLoader_id != HeapDump::NULL_ID);
  if (!is_initialized()) {
    const HeapDump::ClassDump &java_lang_ClassLoader_dump = heap_dump.get_class_dump(java_lang_ClassLoader_id);
    precond(is_class_loader_class_dump(heap_dump, java_lang_ClassLoader_dump));
    INITIALIZE_OFFSETS(java_lang_ClassLoader, CLASSLOADER_DUMP_FIELDS_DO, NO_DUMP_FIELDS_DO)
    DEBUG_ONLY(_java_lang_ClassLoader_id = java_lang_ClassLoader_id);
    _id_size = heap_dump.id_size;
  } else {
    ASSERT_INITIALIZED_WITH_SAME_ID(java_lang_ClassLoader)
  }
  postcond(is_initialized());
}

CLASSLOADER_DUMP_FIELDS_DO(DEFINE_GET_FIELD_METHOD)


// java.lang.Class

#ifdef ASSERT
static bool is_class_mirror_class_dump(const ParsedHeapDump &heap_dump, const HeapDump::ClassDump &dump) {
  const bool has_right_name_and_loader = heap_dump.get_class_name(dump.id) == vmSymbols::java_lang_Class() &&
                                         dump.class_loader_id == HeapDump::NULL_ID;
  if (!has_right_name_and_loader) {
    return false;
  }

  assert(dump.super_id != HeapDump::NULL_ID, "illegal super in %s dump " HDID_FORMAT ": expected %s, got none",
         vmSymbols::java_lang_Class()->as_klass_external_name(), dump.id,
         vmSymbols::java_lang_Object()->as_klass_external_name());

  const HeapDump::ClassDump &super_dump = heap_dump.get_class_dump(dump.super_id);
  assert(heap_dump.get_class_name(super_dump.id) == vmSymbols::java_lang_Object() &&
         super_dump.class_loader_id == HeapDump::NULL_ID, "illegal super in %s dump " HDID_FORMAT ": expected %s, got %s",
         vmSymbols::java_lang_Class()->as_klass_external_name(), dump.id,
         heap_dump.get_class_name(super_dump.id)->as_klass_external_name(),
         vmSymbols::java_lang_Object()->as_klass_external_name());

  return true;
}

DEFINE_INSTANCE_DUMP_CHECK(class_mirror)
#endif // ASSERT

void HeapDumpClasses::java_lang_Class::ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_Class_id) {
  precond(java_lang_Class_id != HeapDump::NULL_ID);
  if (!is_initialized()) {
    const HeapDump::ClassDump &java_lang_Class_dump = heap_dump.get_class_dump(java_lang_Class_id);
    precond(is_class_mirror_class_dump(heap_dump, java_lang_Class_dump));
    INITIALIZE_OFFSETS(java_lang_Class, CLASSMIRROR_DUMP_FIELDS_DO, CLASSMIRROR_DUMP_PTR_FIELDS_DO)
    DEBUG_ONLY(_java_lang_Class_id = java_lang_Class_id);
    _id_size = heap_dump.id_size;
  } else {
    ASSERT_INITIALIZED_WITH_SAME_ID(java_lang_Class)
  }
  postcond(is_initialized());
}

CLASSMIRROR_DUMP_FIELDS_DO(DEFINE_GET_FIELD_METHOD)
CLASSMIRROR_DUMP_PTR_FIELDS_DO(DEFINE_GET_PTR_FIELD_METHOD)

HeapDumpClasses::java_lang_Class::Kind HeapDumpClasses::java_lang_Class::kind(const HeapDump::InstanceDump &dump) const {
  const bool has_klass = klass(dump) != 0;
  const bool has_component = componentType(dump) != 0;
  if (has_klass) {
    return has_component ? Kind::ARRAY : Kind::INSTANCE;
  }
  guarantee(!has_component, "%s object " HDID_FORMAT " representing a primitive type cannot have a component type",
            vmSymbols::java_lang_Class()->as_klass_external_name(), dump.id);
  return Kind::PRIMITIVE;
}


#undef DEFINE_GET_PTR_FIELD_METHOD
#undef DEFINE_GET_FIELD_METHOD
#undef ASSERT_INITIALIZED_WITH_SAME_ID
#undef INITIALIZE_OFFSETS
#undef SET_FIELD_OFFSET
#undef CHECK_FIELD_FOUND
#undef CREATE_PTR_FIELD_OFFSET_CASE
#undef CREATE_FIELD_OFFSET_CASE
#undef DEFINE_OFFSET_FROM_START_LOCAL
#undef DEFINE_INSTANCE_DUMP_CHECK
#undef NO_DUMP_FIELDS_DO
