#ifndef SHARE_UTILITIES_HEAPDUMPCLASSES_HPP
#define SHARE_UTILITIES_HEAPDUMPCLASSES_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/heapDumpParser.hpp"

#define DEFINE_OFFSET_FIELD(klass, name, ...) \
  u4 _##name##_offset;
#define DECLARE_GET_FIELD_METHOD(klass, name, name_sym, basic_type, c_type, type_name) \
  c_type name(const HeapDump::InstanceDump &dump) const;
// "Pointer" fields are internal fields of intptr type: their Java type is
// either int or long depending on the CPU architecture
#define DECLARE_GET_PTR_FIELD_METHOD(klass, name, name_sym) \
  jlong name(const HeapDump::InstanceDump &dump) const;

// Helper classes for parsing HPROF-dumped instance fields of well-known
// classes and their sub-classes.
//
// Classes and fields are to be added on-demand.
struct HeapDumpClasses : public AllStatic {

#define CLASSLOADER_DUMP_FIELDS_DO(macro)                                                             \
  macro(java_lang_ClassLoader, parent, vmSymbols::parent_name(), T_OBJECT, HeapDump::ID, object_id)   \
  macro(java_lang_ClassLoader, name, vmSymbols::name_name(), T_OBJECT, HeapDump::ID, object_id)       \
  macro(java_lang_ClassLoader, nameAndId, "nameAndId", T_OBJECT, HeapDump::ID, object_id)             \
  macro(java_lang_ClassLoader, unnamedModule, "unnamedModule", T_OBJECT, HeapDump::ID, object_id)     \
  macro(java_lang_ClassLoader, parallelLockMap, "parallelLockMap", T_OBJECT, HeapDump::ID, object_id)

  class java_lang_ClassLoader {
   private:
    u4 _id_size = 0;
    CLASSLOADER_DUMP_FIELDS_DO(DEFINE_OFFSET_FIELD)
    DEBUG_ONLY(HeapDump::ID _java_lang_ClassLoader_id = HeapDump::NULL_ID);

   public:
    void ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_ClassLoader_id);
    CLASSLOADER_DUMP_FIELDS_DO(DECLARE_GET_FIELD_METHOD)

   private:
    bool is_initialized() const { return _id_size > 0; }
  };


#define CLASSMIRROR_DUMP_FIELDS_DO(macro)                                                                   \
  macro(java_lang_Class, name, vmSymbols::name_name(), T_OBJECT, HeapDump::ID, object_id)                   \
  macro(java_lang_Class, module, "module", T_OBJECT, HeapDump::ID, object_id)                               \
  macro(java_lang_Class, componentType, vmSymbols::componentType_name(), T_OBJECT, HeapDump::ID, object_id)
#define CLASSMIRROR_DUMP_PTR_FIELDS_DO(macro)                        \
  macro(java_lang_Class, klass, vmSymbols::klass_name())             \
  macro(java_lang_Class, array_klass, vmSymbols::array_klass_name())

  // Requires the heap dump to include injected fields.
  class java_lang_Class {
   private:
    u4 _id_size = 0;
    BasicType _ptr_type = T_ILLEGAL;
    CLASSMIRROR_DUMP_FIELDS_DO(DEFINE_OFFSET_FIELD)
    CLASSMIRROR_DUMP_PTR_FIELDS_DO(DEFINE_OFFSET_FIELD)
    DEBUG_ONLY(HeapDump::ID _java_lang_Class_id = HeapDump::NULL_ID);

   public:
    void ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_Class_id);

    CLASSMIRROR_DUMP_FIELDS_DO(DECLARE_GET_FIELD_METHOD)
    CLASSMIRROR_DUMP_PTR_FIELDS_DO(DECLARE_GET_PTR_FIELD_METHOD)

    enum class Kind { INSTANCE, ARRAY, PRIMITIVE };
    Kind kind(const HeapDump::InstanceDump &dump) const;
    bool is_instance_kind(const HeapDump::InstanceDump &dump) const  { return kind(dump) == Kind::INSTANCE; }
    bool is_array_kind(const HeapDump::InstanceDump &dump) const     { return kind(dump) == Kind::ARRAY; }
    bool is_primitive_kind(const HeapDump::InstanceDump &dump) const { return kind(dump) == Kind::PRIMITIVE; }

    bool mirrors_void(const HeapDump::InstanceDump &dump) const {
      // Void is the only "primitive type" without an array class
      return is_primitive_kind(dump) && array_klass(dump) == 0;
    }

   private:
    bool is_initialized() const { return _id_size > 0; }
  };


#define RESOLVEDMETHODNAME_DUMP_FIELDS_DO(macro)                                                                      \
  macro(java_lang_invoke_ResolvedMethodName, vmholder, vmSymbols::vmholder_name(), T_OBJECT, HeapDump::ID, object_id) \
  macro(java_lang_invoke_ResolvedMethodName, method_kind, vmSymbols::internal_kind_name(), T_BYTE, jbyte, byte)
#define RESOLVEDMETHODNAME_DUMP_PTR_FIELDS_DO(macro)                                                    \
  macro(java_lang_invoke_ResolvedMethodName, method_name_id, vmSymbols::internal_name_name())           \
  macro(java_lang_invoke_ResolvedMethodName, method_signature_id, vmSymbols::internal_signature_name())

  // Requires the heap dump to include injected fields and 3 additional
  // identification fake-fields for 'vmtarget' field of ResolvedMethodName.
  class java_lang_invoke_ResolvedMethodName {
   private:
    u4 _id_size = 0;
    BasicType _ptr_type = T_ILLEGAL;
    RESOLVEDMETHODNAME_DUMP_FIELDS_DO(DEFINE_OFFSET_FIELD)
    RESOLVEDMETHODNAME_DUMP_PTR_FIELDS_DO(DEFINE_OFFSET_FIELD)
    DEBUG_ONLY(HeapDump::ID _java_lang_invoke_ResolvedMethodName_id = HeapDump::NULL_ID);

   public:
    void ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_invoke_ResolvedMethodName_id);

    RESOLVEDMETHODNAME_DUMP_FIELDS_DO(DECLARE_GET_FIELD_METHOD)
    HeapDump::ID method_name_id(const HeapDump::InstanceDump &dump) const;
    HeapDump::ID method_signature_id(const HeapDump::InstanceDump &dump) const;

   private:
    bool is_initialized() const { return _id_size > 0; }
  };


#define MEMBERNAME_DUMP_FIELDS_DO(macro)                                                                          \
  macro(java_lang_invoke_MemberName, clazz, vmSymbols::clazz_name(), T_OBJECT, HeapDump::ID, object_id)           \
  macro(java_lang_invoke_MemberName, name, vmSymbols::name_name(), T_OBJECT, HeapDump::ID, object_id)             \
  macro(java_lang_invoke_MemberName, type, vmSymbols::type_name(), T_OBJECT, HeapDump::ID, object_id)             \
  macro(java_lang_invoke_MemberName, flags, vmSymbols::flags_name(), T_INT, jint, int)                            \
  macro(java_lang_invoke_MemberName, method, vmSymbols::method_name(), T_OBJECT, HeapDump::ID, object_id)         \
  macro(java_lang_invoke_MemberName, resolution, "resolution", T_OBJECT, HeapDump::ID, object_id)
#define MEMBERNAME_DUMP_PTR_FIELDS_DO(macro)                             \
  macro(java_lang_invoke_MemberName, vmindex, vmSymbols::vmindex_name())

  // Requires the heap dump to include injected fields.
  class java_lang_invoke_MemberName {
   private:
    u4 _id_size = 0;
    BasicType _ptr_type = T_ILLEGAL;
    MEMBERNAME_DUMP_FIELDS_DO(DEFINE_OFFSET_FIELD)
    MEMBERNAME_DUMP_PTR_FIELDS_DO(DEFINE_OFFSET_FIELD)
    DEBUG_ONLY(HeapDump::ID _java_lang_invoke_MemberName_id = HeapDump::NULL_ID);

   public:
    void ensure_initialized(const ParsedHeapDump &heap_dump, HeapDump::ID java_lang_invoke_MemberName_id);

    MEMBERNAME_DUMP_FIELDS_DO(DECLARE_GET_FIELD_METHOD)
    MEMBERNAME_DUMP_PTR_FIELDS_DO(DECLARE_GET_PTR_FIELD_METHOD)

    bool is_field(const HeapDump::InstanceDump &dump) const;
    bool is_resolved(const HeapDump::InstanceDump &dump) const { return resolution(dump) == HeapDump::NULL_ID; }

   private:
    bool is_initialized() const { return _id_size > 0; }
  };
};

#undef DECLARE_GET_FIELD_METHOD
#undef DEFINE_OFFSET_FIELD

#endif // SHARE_UTILITIES_HEAPDUMPCLASSES_HPP
