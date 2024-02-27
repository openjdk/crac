#include "precompiled.hpp"
#include "classfile/classFileParser.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/fieldLayoutBuilder.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/resolutionErrors.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "classfile/vmSymbols.hpp"
#include "classfile_constants.h"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/bytecodes.hpp"
#include "interpreter/linkResolver.hpp"
#include "jni.h"
#include "jvm_constants.h"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "metaprogramming/enableIf.hpp"
#include "oops/annotations.hpp"
#include "oops/array.hpp"
#include "oops/constMethod.hpp"
#include "oops/constMethodFlags.hpp"
#include "oops/constantPool.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/cpCache.hpp"
#include "oops/fieldInfo.hpp"
#include "oops/fieldInfo.inline.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceKlassFlags.hpp"
#include "oops/klassVtable.hpp"
#include "oops/method.hpp"
#include "oops/method.inline.hpp"
#include "oops/methodFlags.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/recordComponent.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/arguments.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/cracHeapRestorer.hpp"
#include "runtime/globals.hpp"
#include "runtime/handles.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/signature.hpp"
#include "runtime/thread.hpp"
#include "services/classLoadingService.hpp"
#include "utilities/accessFlags.hpp"
#include "utilities/basicTypeReader.hpp"
#include "utilities/bytes.hpp"
#include "utilities/constantTag.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/macros.hpp"
#include "utilities/pair.hpp"
#include "utilities/tribool.hpp"
#include <type_traits>
#if INCLUDE_JFR
#include "jfr/support/jfrTraceIdExtension.hpp"
#endif // INCLUDE_JFR

static constexpr bool IS_ZERO = ZERO_ONLY(true) NOT_ZERO(false);
static constexpr bool HAVE_JVMTI = JVMTI_ONLY(true) NOT_JVMTI(false);

void ClassDumpReader::set_id_size(u2 value, TRAPS) {
  if (!is_supported_id_size(value)) {
    THROW_MSG(vmSymbols::java_lang_UnsupportedOperationException(),
              err_msg("ID size %i is not supported: should be 1, 2, 4 or 8", value));
  }
  _id_size = value;
}

void ClassDumpReader::read_raw(void *buf, size_t size, TRAPS) {
  if (!_reader->read_raw(buf, size)) {
    log_error(crac, class, parser)("Raw reading error (position %zu, size %zu): %s", _reader->pos(), size, os::strerror(errno));
    THROW_MSG(vmSymbols::java_io_IOException(), "Truncated dump");
  }
}

template <class T>
T ClassDumpReader::read(TRAPS) {
  T result;
  if (!_reader->read(&result)) {
    log_error(crac, class, parser)("Basic type reading error (position %zu, size %zu): %s", _reader->pos(), sizeof(T), os::strerror(errno));
    THROW_MSG_(vmSymbols::java_io_IOException(), "Truncated dump", {});
  }
  return result;
}

bool ClassDumpReader::read_bool(TRAPS) {
  const auto byte = read<u1>(CHECK_false);
  guarantee(byte <= 1, "not a boolean: expected 0 or 1, got %i", byte);
  return byte == 1;
}

HeapDump::ID ClassDumpReader::read_id(bool can_be_null, TRAPS) {
  precond(_id_size > 0);
  HeapDump::ID result;
  if (!_reader->read_uint(&result, _id_size)) {
    log_error(crac, class, parser)("ID reading error (position %zu, size %i): %s", _reader->pos(), _id_size, os::strerror(errno));
    THROW_MSG_0(vmSymbols::java_io_IOException(), "Truncated dump");
  }
  guarantee(can_be_null || result != HeapDump::NULL_ID, "unexpected null ID");
  return result;
}

void ClassDumpReader::skip(size_t size, TRAPS) {
  if (!_reader->skip(size)) {
    log_error(crac, class, parser)("Reading error (position %zu, size %zu): %s", _reader->pos(), size, os::strerror(errno));
    THROW_MSG(vmSymbols::java_io_IOException(), "Truncated dump");
  }
}

// Parses a particular class in a class dump and creates it.
class CracInstanceClassDumpParser : public StackObj /* constructor allocates resources */,
                                    public ClassDumpReader {
 private:
  const ParsedHeapDump &_heap_dump;                      // Heap dump accompanying the class dump
  const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &_created_classes; // Classes already created (super and interfaces should be here)
  const HeapDump::ClassDump &_class_dump;
  ClassLoaderData *const _loader_data;

  bool _finished = false;
  InstanceKlass *_ik = nullptr;
  InstanceKlass::ClassState _class_state;
  HeapDump::ID _class_initialization_error_id;
  InterclassRefs _interclass_refs;

 public:
  CracInstanceClassDumpParser(u2 id_size, BasicTypeReader *reader, const ParsedHeapDump &heap_dump,
                              const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &created_classes,
                              const HeapDump::ClassDump &class_dump, ClassLoaderData *loader_data, TRAPS) :
      ClassDumpReader(reader, id_size), _heap_dump(heap_dump),
      _created_classes(created_classes), _class_dump(class_dump),
      _loader_data(loader_data) {
    precond(reader != nullptr);
    precond(loader_data != nullptr);
    log_trace(crac, class, parser)("Parsing instance class " HDID_FORMAT, class_dump.id);
    parse_class(CHECK);
    create_class(CHECK);
    _finished = true;
    postcond(_ik != nullptr);
    if (log_is_enabled(Debug, crac, class, parser)) {
      ResourceMark rm;
      log_debug(crac, class, parser)("Parsed and created instance class " HDID_FORMAT " (%s)", class_dump.id, _ik->external_name());
    }
  }

  InstanceKlass *created_class()     const           { precond(_finished); return _ik; }
  // Returned arrays are resource-allocated in the parser's constructor. The
  // caller must ensure there is no resource mark boundaries between the call to
  // the constructor and the usage of the returned arrays.
  InterclassRefs interclass_references() const       { precond(_finished); return _interclass_refs; }
  InstanceKlass::ClassState class_state() const      { precond(_finished); return _class_state; }
  HeapDump::ID class_initialization_error_id() const { precond(_finished); return _class_initialization_error_id; }

  ~CracInstanceClassDumpParser() {
    if (_finished) {
      // The data has been transfered to the created class which is now
      // responsible for deallocation
      return;
    }

    if (_ik != nullptr) {
      // Do what ClassFileParser does
      _loader_data->add_to_deallocate_list(_ik);
    }

    if (_nest_members != Universe::the_empty_short_array()) {
      MetadataFactory::free_array(_loader_data, _nest_members);
    }
    if (_inner_classes != Universe::the_empty_short_array()) {
      MetadataFactory::free_array(_loader_data, _inner_classes);
    }
    if (_permitted_subclasses != Universe::the_empty_short_array()) {
      MetadataFactory::free_array(_loader_data, _inner_classes);
    }
    if (_source_debug_extension != nullptr) {
      FREE_C_HEAP_ARRAY(char, _source_debug_extension);
    }
    MetadataFactory::free_array(_loader_data, _bsm_operands);
    MetadataFactory::free_array(_loader_data, _class_annotations);
    MetadataFactory::free_array(_loader_data, _class_type_annotations);
    InstanceKlass::deallocate_record_components(_loader_data, _record_components);

    SystemDictionary::delete_resolution_error(_cp);
    MetadataFactory::free_metadata(_loader_data, _cp);

    InstanceKlass::deallocate_interfaces(_loader_data, _super, _local_interfaces, _transitive_interfaces);

    if (_original_method_ordering != Universe::the_empty_int_array()) {
      MetadataFactory::free_array(_loader_data, _original_method_ordering);
    }
    InstanceKlass::deallocate_methods(_loader_data, _methods);
    if (_default_methods != Universe::the_empty_method_array()) {
      MetadataFactory::free_array(_loader_data, _default_methods);
    }

    MetadataFactory::free_array(_loader_data, _field_info_stream);
    MetadataFactory::free_array(_loader_data, _field_statuses);
    Annotations::free_contents(_loader_data, _field_annotations);
    Annotations::free_contents(_loader_data, _field_type_annotations);

    JVMTI_ONLY(os::free(_cached_class_file)); // Handles nullptr
  };

 private:
  // First the parsed data is put into these fields, then when there is enough
  // data to allocate the class, the ownership of this data istransfered to it

  u2 _minor_version;                                      // Class file's minor version
  u2 _major_version;                                      // Class file's major version
  JVMTI_ONLY(jint _redefinition_version);                 // Class redefinition version

  AccessFlags _class_access_flags;                        // Class access flags from class file + internal flags from Klass
  bool _is_value_based;                                   // Whether the class is marked value-based in the dump
  InstanceKlassFlags _ik_flags;                           // Internal flags and statuses from InstanceKlass

  u2 _source_file_name_index;                             // SourceFile class attribute
  u2 _generic_signature_index;                            // Signature class attribute
  u2 _nest_host_index;                                    // NestHost class attribute
  Array<u2> *_nest_members = nullptr;                     // NestMembers class attribute
  Array<u2> *_inner_classes = nullptr;                    // InnerClasses and EnclosingMethod class attributes
  char *_source_debug_extension = nullptr;                // SourceDebugExtension class attribute (nul-terminated, heap-allocated)
  Array<u2> *_bsm_operands = nullptr;                     // BootstrapMethods class attribute (gets moved into the ConstantPool as soon as it's ready)
  Array<RecordComponent *> *_record_components = nullptr; // Record class attribute
  Array<u2> *_permitted_subclasses = nullptr;             // PermittedSubclasses class attribute
  AnnotationArray *_class_annotations = nullptr;          // Runtime(In)VisibleAnnotations
  AnnotationArray *_class_type_annotations = nullptr;     // Runtime(In)VisibleTypeAnnotations class attribute

  ConstantPool *_cp = nullptr;

  u2 _this_class_index;
  InstanceKlass *_super = nullptr;
  Array<InstanceKlass *> *_local_interfaces = nullptr;
  Array<InstanceKlass *> *_transitive_interfaces = nullptr;

  u2 _java_fields_num;
  u2 _injected_fields_num;
  u2 _static_oop_fields_num;
  GrowableArrayCHeap<FieldInfo, mtInternal> _field_infos;
  Array<u1> *_field_info_stream = nullptr;
  Array<FieldStatus> *_field_statuses = nullptr;
  Array<AnnotationArray *> *_field_annotations = nullptr;
  Array<AnnotationArray *> *_field_type_annotations = nullptr;

  Array<int> *_original_method_ordering = nullptr;
  Array<Method *> *_methods = nullptr;
  Array<Method *> *_default_methods = nullptr;

  JVMTI_ONLY(JvmtiCachedClassFileData *_cached_class_file = nullptr);

  // ###########################################################################
  // Parsing helpers
  // ###########################################################################

  template <class UINT_T, ENABLE_IF((std::is_same<UINT_T, u1>::value || std::is_same<UINT_T, u2>::value ||
                                     std::is_same<UINT_T, u4>::value || std::is_same<UINT_T, u8>::value))>
  void read_uint_array_data(UINT_T *buf, size_t length, TRAPS) {
    if (Endian::is_Java_byte_ordering_different()) { // Have to convert
      for (size_t i = 0; i < length; i++) {
        buf[i] = read<UINT_T>(CHECK);
      }
    } else { // Can read directly
      read_raw(buf, length * sizeof(UINT_T), CHECK);
    }
  }

  template <class UINT_T, ENABLE_IF((std::is_same<UINT_T, u1>::value || std::is_same<UINT_T, u2>::value ||
                                     std::is_same<UINT_T, u4>::value || std::is_same<UINT_T, u8>::value))>
  Array<UINT_T> *read_uint_array(Array<UINT_T> *if_none, TRAPS) {
    precond(_loader_data != nullptr);

    const auto len = read<u4>(CHECK_NULL);
    if (len == CracClassDump::NO_ARRAY_SENTINEL) {
      return if_none;
    }
    guarantee(len <= INT_MAX, "metadata array length too large: " UINT32_FORMAT " > %i", len, INT_MAX);

    Array<UINT_T> *arr = MetadataFactory::new_array<UINT_T>(_loader_data, len, CHECK_NULL);
    read_uint_array_data(arr->data(), arr->length(), THREAD);
    if (HAS_PENDING_EXCEPTION) {
      MetadataFactory::free_array(_loader_data, arr);
    }
    return arr;
  }

  Pair<HeapDump::ID, InterclassRefs::MethodDescription> read_method_identification(TRAPS) {
    const HeapDump::ID holder_id = read_id(false, CHECK_({}));
    const HeapDump::ID name_id = read_id(false, CHECK_({}));
    const HeapDump::ID sig_id = read_id(false, CHECK_({}));
    const auto kind_raw = read<u1>(CHECK_({}));
    guarantee(CracClassDump::is_method_kind(kind_raw), "unrecognized method kind: %i", kind_raw);
    return {holder_id, {name_id, sig_id, static_cast<CracClassDump::MethodKind>(kind_raw)}};
  }

  // ###########################################################################
  // Parsing
  // ###########################################################################

  void parse_class_state(TRAPS) {
    const auto raw_state = read<u1>(CHECK);
    guarantee(raw_state == InstanceKlass::loaded || raw_state == InstanceKlass::linked ||
              raw_state == InstanceKlass::fully_initialized || raw_state == InstanceKlass::initialization_error,
              "illegal class state: %i", raw_state);
    _class_state = static_cast<InstanceKlass::ClassState>(raw_state);

    if (raw_state == InstanceKlass::initialization_error) {
      _class_initialization_error_id = read_id(true, CHECK);
    } else {
      _class_initialization_error_id = HeapDump::NULL_ID;
    }

    log_trace(crac, class, parser)("  Parsed class state");
  }

  void parse_class_versions(TRAPS) {
    _minor_version = read<u2>(CHECK);
    _major_version = read<u2>(CHECK);

    const auto redefinition_version = read<jint>(CHECK);
#if INCLUDE_JVMTI
    _redefinition_version = redefinition_version;
#else  // INCLUDE_JVMTI
    // Note: the fact that this verion is 0 doesn't mean the class hasn't been
    // redefined (overflow is allowed), so we'll also check the corresponding
    // internal flag later.
    //
    // Also, not sure this being not 0 will cause any problems when JVM TI isn't
    // included, but under the normal circumstances such situation cannot
    // happen, so abort just to be safe
    guarantee(redefinition_version == 0,
              "class has been redefined by a JVM TI agent (has a non-zero redefinition version), "
              "so this dump can only be restored on VMs that have JVM TI included");
#endif // INCLUDE_JVMTI

    log_trace(crac, class, parser)("  Parsed class versions");
  }

  void parse_class_flags(TRAPS) {
    u4 raw_access_flags = read<u4>(CHECK);
    guarantee((raw_access_flags & JVM_ACC_WRITTEN_FLAGS & ~JVM_RECOGNIZED_CLASS_MODIFIERS) == 0,
              "illegal class file flags " UINT32_FORMAT, raw_access_flags & JVM_ACC_WRITTEN_FLAGS);
    guarantee((raw_access_flags & ~JVM_ACC_WRITTEN_FLAGS &
               ~(JVM_ACC_HAS_FINALIZER | JVM_ACC_IS_CLONEABLE_FAST | JVM_ACC_IS_HIDDEN_CLASS | JVM_ACC_IS_VALUE_BASED_CLASS)) == 0,
              "unrecognized internal class flags: " UINT32_FORMAT, raw_access_flags & ~JVM_ACC_WRITTEN_FLAGS);
    // Update flags that depend on VM-options:
    raw_access_flags &= ~JVM_ACC_HAS_FINALIZER; // Will recompute by ourselves
    _is_value_based = (raw_access_flags & JVM_ACC_IS_VALUE_BASED_CLASS) != 0; // Remember for CDS flags
    if (DiagnoseSyncOnValueBasedClasses == 0) {
      raw_access_flags &= ~JVM_ACC_IS_VALUE_BASED_CLASS;
    }
    // Don't use set_flags -- it will drop internal Klass flags
    const AccessFlags access_flags(checked_cast<int>(raw_access_flags));
    guarantee(!access_flags.is_cloneable_fast() || vmClasses::Cloneable_klass_loaded(), // Must implement clonable, so we should've created it as interface
              "either dump order is incorrect or internal class flags are inconsistent with the implemented interfaces");

    const u2 internal_flags = read<u2>(CHECK);
    const u1 internal_status = read<u1>(CHECK);
    InstanceKlassFlags ik_flags(internal_flags, internal_status);
    guarantee(!ik_flags.shared_loading_failed() && ik_flags.is_shared_unregistered_class(),
              "illegal internal instance class flags");
    guarantee(!ik_flags.is_being_redefined() && !ik_flags.is_scratch_class() && !ik_flags.is_marked_dependent() && !ik_flags.is_being_restored(),
              "illegal internal instance class statuses");
    guarantee(!ik_flags.declares_nonstatic_concrete_methods() || ik_flags.has_nonstatic_concrete_methods(),
              "inconsistent internal instance class flags");
    guarantee(!ik_flags.declares_nonstatic_concrete_methods() || access_flags.is_interface(),
              "internal instance class flags are not consistent with class access flags");
    guarantee(_class_state < InstanceKlass::ClassState::linked || ik_flags.rewritten(),
              "internal instance class statuses are not consistent with class initialization state");
    if (!HAVE_JVMTI && ik_flags.has_been_redefined()) {
      // At the moment there shouldn't be any problems with just the fact that
      // this flag is set when JVM TI isn't included, but under the normal
      // circumstances such situation cannot happen, so abort just to be safe
      THROW_MSG(vmSymbols::java_lang_UnsupportedOperationException(),
                "class has been redefined by a JVM TI agent (has the corresponding flag set), "
                "making this dump restorable only on VMs that have JVM TI included");
    }
    ik_flags.set_is_being_restored(true);

    _class_access_flags = access_flags;
    _ik_flags = ik_flags;

    log_trace(crac, class, parser)("  Parsed class flags");
  }

  void parse_nest_host_attr(TRAPS) {
    _nest_host_index = read<u2>(CHECK);
    _interclass_refs.dynamic_nest_host = read_id(true, CHECK);
    guarantee(_interclass_refs.dynamic_nest_host == HeapDump::NULL_ID || _class_access_flags.is_hidden_class(),
              "only hidden classes can have a dynamic nest host");
  }

  void parse_source_debug_extension_attr(TRAPS) {
    const bool has_sde = read_bool(CHECK);
    if (!has_sde) {
      return; // No SourceDebugExtension attribute
    }

    const auto len = read<jint>(CHECK);
    // Will use int adding 1 for the trailing nul. ClassFileParser doesn't
    // validate this for some reason (there is only an assert).
    guarantee(len <= INT_MAX - 1, "SourceDebugExtension length is too large: %i > %i", len, INT_MAX - 1);

    if (!JvmtiExport::can_get_source_debug_extension()) {
      // Skip if SourceDebugExtension won't be retrieved (just as ClassFileParser does)
      skip(len, CHECK);
      return;
    }

    _source_debug_extension = NEW_C_HEAP_ARRAY(char, len + 1, mtClass);
    read_raw(_source_debug_extension, len, CHECK);
    _source_debug_extension[len] = '\0';
  }

  void parse_record_attr(TRAPS) {
    const auto has_record = read_bool(CHECK);
    if (!has_record) {
      return;
    }

    const auto components_num = read<u2>(CHECK);
    // Pre-fill with nulls so that deallocation works correctly if an error occures before the array is filled
    _record_components = MetadataFactory::new_array<RecordComponent *>(_loader_data, components_num, nullptr, CHECK);
    for (u2 i = 0; i < components_num; i++) {
      const auto name_index = read<u2>(CHECK);
      const auto descriptor_index = read<u2>(CHECK);
      const auto attributes_count = read<u2>(CHECK);
      const auto generic_signature_index = read<u2>(CHECK);
      AnnotationArray *annotations = read_uint_array<u1>(nullptr, CHECK);
      AnnotationArray *type_annotations = read_uint_array<u1>(nullptr, CHECK);

      auto *component = RecordComponent::allocate(_loader_data, name_index, descriptor_index,
                                                  attributes_count, generic_signature_index,
                                                  annotations, type_annotations, CHECK);
      _record_components->at_put(i, component);
    }
  }

  void parse_class_attrs(TRAPS) {
    _source_file_name_index = read<u2>(CHECK);
    _generic_signature_index = read<u2>(CHECK);
    parse_nest_host_attr(CHECK);
    _nest_members = read_uint_array(Universe::the_empty_short_array(), CHECK);
    _inner_classes = read_uint_array(Universe::the_empty_short_array(), CHECK);
    parse_source_debug_extension_attr(CHECK);
    _bsm_operands = read_uint_array<u2>(nullptr, CHECK);
    parse_record_attr(CHECK);
    _permitted_subclasses = read_uint_array(Universe::the_empty_short_array(), CHECK);
    _class_annotations = read_uint_array<u1>(nullptr, CHECK);
    _class_type_annotations = read_uint_array<u1>(nullptr, CHECK);
    log_trace(crac, class, parser)("  Parsed class attributes");
  }

  void parse_resolution_error_symbols(int err_table_index, TRAPS) {
    const HeapDump::ID error_sym_id = read_id(true, CHECK);
    Symbol *error_sym = error_sym_id == HeapDump::NULL_ID ? nullptr : _heap_dump.get_symbol(error_sym_id);
    const HeapDump::ID msg_sym_id = read_id(true, CHECK);
    Symbol *msg_sym = msg_sym_id == HeapDump::NULL_ID ? nullptr : _heap_dump.get_symbol(msg_sym_id);
    const HeapDump::ID cause_sym_id = read_id(true, CHECK);
    Symbol *cause_sym = cause_sym_id == HeapDump::NULL_ID ? nullptr : _heap_dump.get_symbol(cause_sym_id);
    const HeapDump::ID cause_msg_sym_id = cause_sym_id == HeapDump::NULL_ID ? HeapDump::NULL_ID : read_id(true, CHECK);
    Symbol *cause_msg_sym = cause_msg_sym_id == HeapDump::NULL_ID ? nullptr : _heap_dump.get_symbol(cause_msg_sym_id);

    char *nest_host_err_msg = nullptr;
    if (err_table_index == _nest_host_index) {
      const auto nest_host_err_len = read<u4>(CHECK);
      nest_host_err_msg = NEW_C_HEAP_ARRAY(char, nest_host_err_len + 1, mtInternal);
      read_raw(nest_host_err_msg, nest_host_err_len, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        FREE_C_HEAP_ARRAY(char, nest_host_err_msg);
      }
      nest_host_err_msg[nest_host_err_len] = '\0';
    }

    const constantPoolHandle cph(Thread::current(), _cp);
#ifdef ASSERT
    {
      MutexLocker ml(Thread::current(), SystemDictionary_lock); // ResolutionErrorTable requires this to be locked
      assert(ResolutionErrorTable::find_entry(cph, err_table_index) == nullptr, "duplicated resolution error");
    }
#endif // ASSERT
    SystemDictionary::add_resolution_error(cph, err_table_index, error_sym, msg_sym, cause_sym, cause_msg_sym);
    if (nest_host_err_msg != nullptr) {
      SystemDictionary::add_nest_host_error(cph, err_table_index, nest_host_err_msg);
    }
  }

  void parse_constant_pool(TRAPS) {
    const auto pool_len = read<u2>(CHECK);
    _cp = ConstantPool::allocate(_loader_data, pool_len, CHECK);
    postcond(_cp->length() == pool_len);

    const auto classes_num = read<u2>(CHECK);
    _cp->allocate_resolved_klasses(_loader_data, classes_num, CHECK);

    u2 current_class_i = 0; // Resolved classes array indexing
    log_trace(crac, class, parser)("  Parsing %i constant pool slots", pool_len);
    for (u2 pool_i = 1 /* index 0 is unused */; pool_i < pool_len; pool_i++) {
      const auto tag = read<u1>(CHECK);
      switch (tag) {
        case JVM_CONSTANT_Utf8: {
          const HeapDump::ID sym_id = read_id(false, CHECK);
          Symbol *const sym = _heap_dump.get_symbol(sym_id);
          sym->increment_refcount(); // Ensures it won't be destroyed together with the heap dump
          _cp->symbol_at_put(pool_i, sym);
          break;
        }
        case JVM_CONSTANT_NameAndType: {
          const auto name_index = read<u2>(CHECK);
          const auto type_index = read<u2>(CHECK);
          _cp->name_and_type_at_put(pool_i, name_index, type_index);
          break;
        }

        case JVM_CONSTANT_Integer: {
          const auto n = read<jint>(CHECK);
          _cp->int_at_put(pool_i, n);
          break;
        }
        case JVM_CONSTANT_Float: {
          const auto n = read<jfloat>(CHECK);
          _cp->float_at_put(pool_i, n);
          break;
        }
        case JVM_CONSTANT_Long: {
          const auto n = read<jlong>(CHECK);
          _cp->long_at_put(pool_i, n);
          guarantee(++pool_i != pool_len, "long occupies two constant pool slots and thus cannot start on the last slot");
          break;
        }
        case JVM_CONSTANT_Double: {
          const auto n = read<jdouble>(CHECK);
          _cp->double_at_put(pool_i, n);
          guarantee(++pool_i != pool_len, "double occupies two constant pool slots and thus cannot start on the last slot");
          break;
        }
        case JVM_CONSTANT_String: {
          const HeapDump::ID sym_id = read_id(false, CHECK);
          Symbol *const sym = _heap_dump.get_symbol(sym_id);
          _cp->unresolved_string_at_put(pool_i, sym);
          // Resolved String objects will be restored as part of cache restoration
          break;
        }

        case JVM_CONSTANT_Class:
        case JVM_CONSTANT_UnresolvedClass:
        case JVM_CONSTANT_UnresolvedClassInError: {
          guarantee(current_class_i < classes_num, "more classes in constant pool than specified");
          const auto class_name_index = read<u2>(CHECK);
          _cp->unresolved_klass_at_put(pool_i, class_name_index, current_class_i++);

          if (tag == JVM_CONSTANT_Class) {
            const HeapDump::ID class_id = read_id(false, CHECK);
            _interclass_refs.cp_class_refs->append({pool_i, class_id});
            if (pool_i == _nest_host_index) {
              const bool has_nest_host_res_error = read_bool(CHECK);
              if (has_nest_host_res_error) {
                parse_resolution_error_symbols(pool_i, CHECK);
              }
            }
          } else if (tag == JVM_CONSTANT_UnresolvedClassInError) {
            parse_resolution_error_symbols(pool_i, CHECK);
            _cp->tag_at_put(pool_i, JVM_CONSTANT_UnresolvedClassInError);
          }
          break;
        }
        case JVM_CONSTANT_Fieldref:
        case JVM_CONSTANT_Methodref:
        case JVM_CONSTANT_InterfaceMethodref: {
          const auto class_index = read<u2>(CHECK);
          const auto name_and_type_index = read<u2>(CHECK);
          if (tag == JVM_CONSTANT_Fieldref) {
            _cp->field_at_put(pool_i, class_index, name_and_type_index);
          } else if (tag == JVM_CONSTANT_Methodref) {
            _cp->method_at_put(pool_i, class_index, name_and_type_index);
          } else {
            _cp->interface_method_at_put(pool_i, class_index, name_and_type_index);
          }
          break;
        }
        case JVM_CONSTANT_MethodType:
        case JVM_CONSTANT_MethodTypeInError: {
          const auto mt_index = read<u2>(CHECK);
          _cp->method_type_index_at_put(pool_i, mt_index);
          if (tag == JVM_CONSTANT_MethodTypeInError) {
            parse_resolution_error_symbols(pool_i, CHECK);
            _cp->tag_at_put(pool_i, JVM_CONSTANT_MethodTypeInError);
          }
          break;
        }
        case JVM_CONSTANT_MethodHandle:
        case JVM_CONSTANT_MethodHandleInError: {
          const auto mh_kind = read<u1>(CHECK);
          const auto mh_index = read<u2>(CHECK);
          _cp->method_handle_index_at_put(pool_i, mh_kind, mh_index);
          if (tag == JVM_CONSTANT_MethodHandleInError) {
            parse_resolution_error_symbols(pool_i, CHECK);
            _cp->tag_at_put(pool_i, JVM_CONSTANT_MethodHandleInError);
          }
          break;
        }
        case JVM_CONSTANT_Dynamic:
        case JVM_CONSTANT_DynamicInError:
        case JVM_CONSTANT_InvokeDynamic: {
          const auto bsm_attr_index = read<u2>(CHECK);
          const auto name_and_type_index = read<u2>(CHECK);
          if (tag == JVM_CONSTANT_InvokeDynamic) {
            _cp->invoke_dynamic_at_put(pool_i, bsm_attr_index, name_and_type_index);
          } else {
            _cp->dynamic_constant_at_put(pool_i, bsm_attr_index, name_and_type_index);
            _cp->set_has_dynamic_constant();
            if (tag == JVM_CONSTANT_DynamicInError) {
              parse_resolution_error_symbols(pool_i, CHECK);
              _cp->tag_at_put(pool_i, JVM_CONSTANT_DynamicInError);
            }
          }
          break;
        }

        default:
          guarantee(false, "illegal tag %i at constant pool slot %i", tag, pool_i);
      }
    }
    guarantee(current_class_i == classes_num, "less classes in constant pool than specified: %i < %i", current_class_i, classes_num);

    log_trace(crac, class, parser)("  Parsed constant pool");
  }

  static int prepare_resolved_method_flags(u1 raw_flags) {
    using crac_shifts = CracClassDump::ResolvedMethodEntryFlagShift;
    using cache_shifts = ConstantPoolCacheEntry;
    return static_cast<int>(is_set_nth_bit(raw_flags, crac_shifts::has_local_signature_shift)) << cache_shifts::has_local_signature_shift |
           static_cast<int>(is_set_nth_bit(raw_flags, crac_shifts::has_appendix_shift))        << cache_shifts::has_appendix_shift        |
           static_cast<int>(is_set_nth_bit(raw_flags, crac_shifts::is_forced_virtual_shift))   << cache_shifts::is_forced_virtual_shift   |
           static_cast<int>(is_set_nth_bit(raw_flags, crac_shifts::is_final_shift))            << cache_shifts::is_final_shift            |
           static_cast<int>(is_set_nth_bit(raw_flags, crac_shifts::is_vfinal_shift))           << cache_shifts::is_vfinal_shift;
  }

  void parse_constant_pool_cache(TRAPS) {
    precond(_cp != nullptr);

    const auto field_entries_len = read<u2>(CHECK);
    const auto method_entries_len = read<jint>(CHECK); // AKA cache length
    const auto indy_entries_len = read<jint>(CHECK);
    guarantee(method_entries_len >= 0, "amount of resolved methods cannot be negative");
    guarantee(indy_entries_len >= 0, "amount of resolved invokedynamic instructions cannot be negative");

    ConstantPoolCache *const cp_cache =
      ConstantPoolCache::allocate_uninitialized(_loader_data, method_entries_len, indy_entries_len, field_entries_len, CHECK);
    _cp->set_cache(cp_cache); // Make constant pool responsible for cache deallocation
    cp_cache->set_constant_pool(_cp);

    for (u2 field_i = 0; field_i < field_entries_len; field_i++) {
      const auto cp_index = read<u2>(CHECK);
      guarantee(cp_index > 0, "resolved field entry %i is uninitialized", field_i);
      ResolvedFieldEntry field_entry(cp_index);

      const auto get_code = read<u1>(CHECK);
      const auto put_code = read<u1>(CHECK);
      if (get_code != 0 && get_code != Bytecodes::_getfield && get_code != Bytecodes::_getstatic) {
        const char *code_name = Bytecodes::is_defined(get_code) ? Bytecodes::name(Bytecodes::cast(get_code)) :
                                                                  static_cast<const char *>(err_msg("%i", get_code));
        guarantee(false, "not a get* bytecode: %s", code_name);
      }
      if (put_code != 0 && put_code != Bytecodes::_putfield && put_code != Bytecodes::_putstatic) {
        const char *code_name = Bytecodes::is_defined(put_code) ? Bytecodes::name(Bytecodes::cast(put_code)) :
                                                                  static_cast<const char *>(err_msg("%i", put_code));
        guarantee(false, "not a put* bytecode: %s", code_name);
      }
      guarantee(get_code != 0 || put_code == 0, "field entry cannot be resolved for put* bytecodes only");

      if (get_code != 0) {
        const HeapDump::ID holder_id = read_id(false, CHECK);
        _interclass_refs.field_refs->append({field_i, holder_id}); // Save to resolve later

        const auto field_index = read<u2>(CHECK);
        const auto tos_state = read<u1>(CHECK);
        guarantee(tos_state < TosState::number_of_states, "illegal resolved field entry ToS state: %i", tos_state);
        const auto flags = read<u1>(CHECK);
        field_entry.fill_in_portable(field_index, tos_state, flags, get_code, put_code);
      }

      *cp_cache->resolved_field_entry_at(field_i) = field_entry;
    }

    for (int cache_i = 0; cache_i < method_entries_len; cache_i++) {
      const auto cp_index = read<u2>(CHECK);
      guarantee(cp_index > 0, "resolved method entry %i is uninitialized", cache_i);
      ConstantPoolCacheEntry cache_entry;
      cache_entry.initialize_entry(cp_index);

      const auto raw_bytecode1 = read<u1>(CHECK);
      const auto raw_bytecode2 = read<u1>(CHECK);
      guarantee(Bytecodes::is_defined(raw_bytecode1), "undefined method resolution bytecode 1: %i", raw_bytecode1);
      guarantee(Bytecodes::is_defined(raw_bytecode2), "undefined method resolution bytecode 2: %i", raw_bytecode2);

      if (raw_bytecode1 > 0 || raw_bytecode2 > 0) { // If resolved
        const auto flags = read<u1>(CHECK);
        guarantee(CracClassDump::is_resolved_method_entry_flags(flags), "unrecognized resolved method entry flags: " UINT8_FORMAT_X_0, flags);
        const auto tos_state = read<u1>(CHECK);
        guarantee(tos_state < TosState::number_of_states, "illegal resolved method entry ToS state: %i", tos_state);
        const auto params_num = read<u1>(CHECK);
        cache_entry.set_method_flags(checked_cast<TosState>(tos_state), prepare_resolved_method_flags(flags), params_num);
        postcond(cache_entry.is_method_entry());
        postcond(cache_entry.flag_state() == tos_state);
        postcond(cache_entry.parameter_size() == params_num);

        // Not readable from the entry until f1 is set
        const bool has_appendix = is_set_nth_bit(flags, CracClassDump::has_appendix_shift);

        // f1
        bool f1_is_method = false;
        HeapDump::ID f1_class_id = HeapDump::NULL_ID;
        InterclassRefs::MethodDescription f1_method_desc;
        const Bytecodes::Code bytecode1 = Bytecodes::cast(raw_bytecode1);
        switch (bytecode1) {
          case Bytecodes::_invokestatic:
          case Bytecodes::_invokespecial:
          case Bytecodes::_invokehandle: {
            f1_is_method = true;
            const auto method_identification = read_method_identification(CHECK);
            f1_class_id = method_identification.first;
            f1_method_desc = method_identification.second;
            break;
          }
          case Bytecodes::_invokeinterface:
            if (!cache_entry.is_forced_virtual()) {
              f1_class_id = read_id(false, CHECK);
            }
            break;
          case 0: // bytecode1 is not set
            break;
          default:
            guarantee(false, "illegal method resolution bytecode 1: %s", Bytecodes::name(bytecode1));
        }
        if (bytecode1 != 0) {
          cache_entry.set_bytecode_1(bytecode1);
          postcond(cache_entry.is_resolved(bytecode1));
        }

        // f2
        HeapDump::ID f2_class_id = HeapDump::NULL_ID;
        InterclassRefs::MethodDescription f2_method_desc;
        bool cleared_virtual_call = false;
        const Bytecodes::Code bytecode2 = Bytecodes::cast(raw_bytecode2);
        guarantee(bytecode2 == 0 || bytecode2 == Bytecodes::_invokevirtual, "illegal method resolution bytecode 2: %s", Bytecodes::name(bytecode2));
        if (cache_entry.is_vfinal() || (bytecode1 == Bytecodes::_invokeinterface && !cache_entry.is_forced_virtual())) {
          guarantee((bytecode1 == Bytecodes::_invokeinterface) != /*XOR*/ (bytecode2 == Bytecodes::_invokevirtual) &&
                    bytecode1 != Bytecodes::_invokestatic && bytecode1 != Bytecodes::_invokehandle,
                    "illegal resolved method data: b1 = %s, b2 = %s, is_vfinal = %s, is_forced_virtual = %s",
                    Bytecodes::name(bytecode1), Bytecodes::name(bytecode2),
                    BOOL_TO_STR(cache_entry.is_vfinal()), BOOL_TO_STR(cache_entry.is_forced_virtual()));
          const auto method_identification = read_method_identification(CHECK);
          f2_class_id = method_identification.first;
          f2_method_desc = method_identification.second;
        } else if (bytecode1 == Bytecodes::_invokehandle) {
          guarantee(bytecode1 != Bytecodes::_invokestatic && bytecode1 != Bytecodes::_invokevirtual && bytecode1 != Bytecodes::_invokeinterface,
                    "illegal resolved method data: b1 = %s, b2 = %s, is_vfinal = %s, is_forced_virtual = %s",
                    Bytecodes::name(bytecode1), Bytecodes::name(bytecode2),
                    BOOL_TO_STR(cache_entry.is_vfinal()), BOOL_TO_STR(cache_entry.is_forced_virtual()));
          const auto appendix_i = read<jint>(CHECK);
          guarantee(appendix_i >= 0, "index into resolved references array cannot be negative");
          cache_entry.set_f2(appendix_i);
        } else if (bytecode2 == Bytecodes::_invokevirtual) {
          precond(!cache_entry.is_vfinal());
          guarantee(bytecode1 != Bytecodes::_invokestatic && bytecode1 != Bytecodes::_invokehandle && bytecode1 != Bytecodes::_invokeinterface,
                    "illegal resolved method data: b1 = %s, b2 = %s, is_vfinal = %s, is_forced_virtual = %s",
                    Bytecodes::name(bytecode1), Bytecodes::name(bytecode2),
                    BOOL_TO_STR(cache_entry.is_vfinal()), BOOL_TO_STR(cache_entry.is_forced_virtual()));
          // f2 was a vtable index which is not portable because vtable depends
          // on method ordering and that depends on Symbol table's memory layout,
          // so clear the entry so it is re-resolved with the new vtable index
          // TODO instead of clearing, find a way to update the vtable index
          if (bytecode1 == 0) {
            // Should clear the whole thing
            cache_entry.initialize_entry(cp_index);
          } else {
            // Clear the only flag that might have been set when resolving the virtual call
            intx flags = cache_entry.flags_ord();
            clear_nth_bit(flags, ConstantPoolCacheEntry::is_forced_virtual_shift);
            cache_entry.set_flags(flags);
            postcond(!cache_entry.is_forced_virtual());
            postcond(cache_entry.is_method_entry() && cache_entry.flag_state() == tos_state && cache_entry.parameter_size() == params_num);
          }
          cleared_virtual_call = true;
        }
        if (bytecode2 != 0 && !cleared_virtual_call) {
          cache_entry.set_bytecode_2(bytecode2);
          postcond(cache_entry.is_resolved(bytecode2));
        }

        if (f1_class_id != HeapDump::NULL_ID || f2_class_id != HeapDump::NULL_ID) { // Save to resolve later
          _interclass_refs.method_refs->append({cache_i, f1_is_method, f1_class_id, f1_method_desc, f2_class_id, f2_method_desc});
        }
      } else {
        const bool is_f2_set = read_bool(CHECK);
        if (is_f2_set) {
          const auto appendix_i = read<jint>(CHECK);
          guarantee(appendix_i >= 0, "index into resolved references array cannot be negative");
          cache_entry.set_f2(appendix_i);
        }
      }
      postcond(cache_entry.bytecode_1() != 0 || cache_entry.bytecode_2() != 0 || // Either resolved...
               (cache_entry.is_f1_null() && cache_entry.flags_ord() == 0));      // ...or clean (except maybe f2)

      *cp_cache->entry_at(cache_i) = cache_entry;
    }

    for (int indy_i = 0; indy_i < indy_entries_len; indy_i++) {
      const auto cp_index = read<u2>(CHECK);
      guarantee(cp_index > 0, "resolved invokedynamic entry %i is uninitialized", indy_i);
      const auto resolved_references_index = read<u2>(CHECK);
      ResolvedIndyEntry indy_entry(resolved_references_index, cp_index);

      const auto extended_flags = read<u1>(CHECK);
      guarantee(extended_flags >> (ResolvedIndyEntry::num_flags + 1) == 0,
                "unrecognized resolved invokedynamic entry flags: " UINT8_FORMAT_X_0, extended_flags);
      const bool is_resolution_failed = is_set_nth_bit(extended_flags, 0); // TODO define the shift in ResolvedIndyEntry
      const bool has_appendix         = is_set_nth_bit(extended_flags, ResolvedIndyEntry::has_appendix_shift);
      const bool is_resolved          = is_set_nth_bit(extended_flags, ResolvedIndyEntry::num_flags);
      guarantee((is_resolved && !is_resolution_failed) || (!is_resolved && !has_appendix),
                "illegal invokedynamic entry flag combination: " UINT8_FORMAT_X_0, extended_flags);

      if (is_resolved) {
        const auto adapter_identification = read_method_identification(CHECK);
        const HeapDump::ID adapter_holder_id = adapter_identification.first;
        const InterclassRefs::MethodDescription adapter_desc = adapter_identification.second;
        _interclass_refs.indy_refs->append({indy_i, adapter_holder_id, adapter_desc}); // Save to resolve later

        const auto adapter_num_params = read<u2>(CHECK);
        const auto adapter_ret_type = read<u1>(CHECK);
        indy_entry.fill_in_partial(adapter_num_params, adapter_ret_type, has_appendix);
      } else if (is_resolution_failed) {
        const int indy_res_err_i = ResolutionErrorTable::encode_cpcache_index(ConstantPool::encode_invokedynamic_index(indy_i));
        parse_resolution_error_symbols(indy_res_err_i, CHECK);
        indy_entry.set_resolution_failed();
      }

      *cp_cache->resolved_indy_entry_at(indy_i) = indy_entry;
    }

    // Mapping from the first part of resolved references back to constant pool
    Array<u2> *const reference_map = read_uint_array<u2>(nullptr, CHECK);
    cp_cache->set_reference_map(reference_map);

    log_trace(crac, class, parser)("  Parsed constant pool cache");
  }

  void parse_this_class_index(TRAPS) {
    const auto this_class_index = read<u2>(CHECK);
    guarantee(this_class_index > 0 && this_class_index < _cp->length(),
              "this class index %i is out of constant pool bounds", this_class_index);
    // Would be nice to assert this points to a resolved class for hidden
    // classes (ClassFileParser performs the resolution in such cases), but we
    // postpone restoring the class references for later
    _this_class_index = this_class_index;
    log_trace(crac, class, parser)("  Parsed this class index");
  }

  void find_super(TRAPS) {
    if (_class_dump.super_id == HeapDump::NULL_ID) {
      log_trace(crac, class, parser)("  No super");
      return;
    }

    InstanceKlass **const super_ptr = _created_classes.get(_class_dump.super_id);
    guarantee(super_ptr != nullptr,
              "invalid dump order: class " HDID_FORMAT " is dumped ahead of its super class " HDID_FORMAT,
              _class_dump.id, _class_dump.super_id);

    InstanceKlass *const super = *super_ptr;
    precond(super->is_loaded());
    guarantee(!super->is_interface(), "class %s (ID " HDID_FORMAT ") cannot be extended by " HDID_FORMAT " because it is an interface",
              super->external_name(), _class_dump.super_id, _class_dump.id);
    guarantee(!super->has_nonstatic_concrete_methods() || _ik_flags.has_nonstatic_concrete_methods(),
              "internal class flags are not consistent with those of the super class");

    _super = super;
    if (log_is_enabled(Trace, crac, class, parser)) {
      ResourceMark rm;
      log_trace(crac, class, parser)("  Found super: %s", super->external_name());
    }
  }

  void parse_interfaces(TRAPS) {
    const auto interfaces_num = read<u2>(CHECK);
    if (interfaces_num == 0) {
      _local_interfaces = Universe::the_empty_instance_klass_array();
      log_trace(crac, class, parser)("  No local interfaces");
      return;
    }

    _local_interfaces = MetadataFactory::new_array<InstanceKlass *>(_loader_data, interfaces_num, CHECK);
    for (u2 i = 0; i < interfaces_num; i++) {
      const HeapDump::ID interface_id = read_id(false, CHECK);
      InstanceKlass **const interface_ptr = _created_classes.get(interface_id);
      guarantee(interface_ptr != nullptr,
                "invalid dump order: class " HDID_FORMAT " is dumped ahead of its interface " HDID_FORMAT,
                _class_dump.id, interface_id);

      InstanceKlass *const interface = *interface_ptr;
      precond(interface->is_loaded());
      guarantee(interface->is_interface(),
                "class %s (ID " HDID_FORMAT ") cannot be implemented by " HDID_FORMAT " because it is not an interface",
                interface->external_name(), interface_id, _class_dump.id);
      guarantee(!interface->has_nonstatic_concrete_methods() || _ik_flags.has_nonstatic_concrete_methods(),
                "internal class flags are not consistent with those of implemented interfaces");

      _local_interfaces->at_put(i, interface);
    }

    log_trace(crac, class, parser)("  Parsed %i local interfaces", interfaces_num);
  }

  void parse_field_annotations(int field_index, int java_fields_num, Array<AnnotationArray *> **const annotations_collection, TRAPS) {
    precond(field_index < java_fields_num);
    precond(annotations_collection != nullptr);
    AnnotationArray *annotations = read_uint_array<u1>(nullptr, CHECK);
    if (annotations == nullptr) {
      return;
    }
    if (*annotations_collection == nullptr) {
      // Pre-fill with nulls since some slots may remain unfilled (fields without annotations)
      *annotations_collection = MetadataFactory::new_array<AnnotationArray *>(_loader_data, java_fields_num, nullptr, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        MetadataFactory::free_array(_loader_data, annotations);
        return;
      }
    }
    (*annotations_collection)->at_put(field_index, annotations);
  }

  void parse_fields(TRAPS) {
    const auto java_fields_num = read<u2>(CHECK);
    const auto injected_fields_num = read<u2>(CHECK);
    const u2 total_fields_num = java_fields_num + injected_fields_num;

    _field_infos.reserve(total_fields_num);
    _field_statuses = MetadataFactory::new_array<FieldStatus>(_loader_data, total_fields_num, CHECK);

    _static_oop_fields_num = 0;
    for (u2 i = 0; i < total_fields_num; i++) {
      const auto name_index = read<u2>(CHECK);
      const auto signature_index = read<u2>(CHECK);
      const auto raw_access_flags = read<jshort>(CHECK);
      guarantee((raw_access_flags & JVM_RECOGNIZED_FIELD_MODIFIERS) == raw_access_flags,
                "unrecognized field access flags: " INT16_FORMAT_X_0, raw_access_flags);
      const auto raw_field_flags = read<u1>(CHECK);
      const auto initializer_index = read<u2>(CHECK);
      const auto generic_signature_index = read<u2>(CHECK);
      const auto contention_group = read<u2>(CHECK);

      {
        const AccessFlags access_flags(raw_access_flags);
        // Check this to skip interfaces when restoring non-static fields. Omit
        // the rest of field flag validation for simplicity.
        guarantee(!_class_access_flags.is_interface() || (access_flags.is_public() && access_flags.is_static() && access_flags.is_final()),
                  "interface fields must be public, static and final");
        const FieldInfo::FieldFlags field_flags(raw_field_flags);
        guarantee(field_flags.is_injected() == (i >= java_fields_num), "injected fields go last");
        guarantee(!field_flags.is_injected() || raw_access_flags == 0,
                  "injected fields don't have any access flags set");
        guarantee(!field_flags.is_contended() || _ik_flags.has_contended_annotations(),
                  "class having contended fields not marked as having contended annotations");

        FieldInfo field_info(access_flags, name_index, signature_index, initializer_index, field_flags);
        field_info.set_generic_signature_index(generic_signature_index);
        if (field_flags.is_contended()) { // Must check or it will be set by set_contended_group()
          field_info.set_contended_group(contention_group);
        }
        _field_infos.append(field_info);

        if (field_flags.is_injected()) {
          _injected_fields_num++;
        }
        // Use FieldInfo::signature() and not the raw signature_index to account for injected fields
        if (access_flags.is_static() && is_reference_type(Signature::basic_type(field_info.signature(_cp)))) {
          _static_oop_fields_num++;
        }
      }

      const auto raw_field_status = read<u1>(CHECK);
      _field_statuses->at_put(i, FieldStatus(raw_field_status));

      if (i < java_fields_num) { // Only non-injected fields have annotations
        parse_field_annotations(i, java_fields_num, &_field_annotations, CHECK);
        parse_field_annotations(i, java_fields_num, &_field_type_annotations, CHECK);
      }
    }

    _java_fields_num = java_fields_num;
    _injected_fields_num = injected_fields_num;
    log_trace(crac, class, parser)("  Parsed fields: %i normal, %i injected", java_fields_num, injected_fields_num);
  }

  InlineTableSizes parse_method_inline_table_sizes(const ConstMethodFlags &flags, TRAPS) {
    const u2 exception_table_length = !flags.has_exception_table() ? 0 : read<u2>(CHECK_({}));
    guarantee(!flags.has_exception_table() || exception_table_length > 0, "existing exception table cannot be empty");

    const jint compressed_linenumber_size = !flags.has_linenumber_table() ? 0 : read<jint>(CHECK_({}));
    guarantee(!flags.has_linenumber_table() || compressed_linenumber_size > 0, "existing line number table cannot be empty");

    const u2 localvariable_table_length = !flags.has_localvariable_table() ? 0 : read<u2>(CHECK_({}));
    guarantee(!flags.has_localvariable_table() || localvariable_table_length > 0, "existing local variable table cannot be empty");

    const u2 checked_exceptions_length = !flags.has_checked_exceptions() ? 0 : read<u2>(CHECK_({}));
    guarantee(!flags.has_checked_exceptions() || checked_exceptions_length > 0, "existing checked exceptions list cannot be empty");

    const int method_parameters_length = !flags.has_method_parameters() ? -1 : read<u1>(CHECK_({})) /* can be zero */;

    const u2 generic_signature_index = !flags.has_generic_signature() ? 0 : read<u2>(CHECK_({}));
    guarantee(!flags.has_generic_signature() || (generic_signature_index > 0 && generic_signature_index < _cp->length()),
              "method's signature index %i is out of constant pool bounds", generic_signature_index);

    const jint method_annotations_length = !flags.has_method_annotations() ? 0 : read<jint>(CHECK_({}));
    guarantee(!flags.has_method_annotations() || method_annotations_length > 0, "existing method annotations cannot be empty");

    const jint parameter_annotations_length = !flags.has_parameter_annotations() ? 0 : read<jint>(CHECK_({}));
    guarantee(!flags.has_parameter_annotations() || parameter_annotations_length > 0, "existing method parameter annotations cannot be empty");

    const jint type_annotations_length = !flags.has_type_annotations() ? 0 : read<jint>(CHECK_({}));
    guarantee(!flags.has_type_annotations() || type_annotations_length > 0, "existing method type annotations cannot be empty");

    const jint default_annotations_length = !flags.has_default_annotations() ? 0 : read<jint>(CHECK_({}));
    guarantee(!flags.has_default_annotations() || default_annotations_length > 0, "existing method default annotations cannot be empty");

#define INLINE_TABLE_NAME_PARAM(name) name,
    return {
      INLINE_TABLES_DO(INLINE_TABLE_NAME_PARAM)
      0 /* end of iteration */
    };
#undef INLINE_TABLE_NAME_PARAM
  }

  static InlineTableSizes update_method_inline_table_sizes(const InlineTableSizes &orig) {
    return {
      orig.localvariable_table_length(),
      orig.compressed_linenumber_size(),
      orig.exception_table_length(),
      orig.checked_exceptions_length(),
      // TODO ClassFIleParser does this, but why? What if j.l.r.Parameter gets loaded later?
      vmClasses::Parameter_klass_loaded() ? orig.method_parameters_length() : -1,
      orig.generic_signature_index(),
      orig.method_annotations_length(),
      orig.parameter_annotations_length(),
      orig.type_annotations_length(),
      orig.default_annotations_length(),
      0
    };
  }

  static void fixup_bytecodes(Method *method) {
    RawBytecodeStream stream(methodHandle(Thread::current(), method));
    for (Bytecodes::Code code = stream.raw_next(); !stream.is_last_bytecode(); code = stream.raw_next()) {
      guarantee((Bytecodes::is_java_code(code) && code != Bytecodes::_lookupswitch) ||
                 code == Bytecodes::_invokehandle ||
                 code == Bytecodes::_fast_aldc || code == Bytecodes::_fast_aldc_w ||
                 code == Bytecodes::_fast_linearswitch || code == Bytecodes::_fast_binaryswitch ||
                 code == Bytecodes::_return_register_finalizer, "illegal bytecode: %s", Bytecodes::name(code));

      if (Endian::is_Java_byte_ordering_different()) {
        const address param_bcp = stream.bcp() + 1;
        if (Bytecodes::is_field_code(code) || Bytecodes::is_invoke(code) || code == Bytecodes::_invokehandle) {
          if (code == Bytecodes::_invokedynamic) {
            Bytes::put_native_u4(param_bcp, Bytes::get_Java_u4(param_bcp));
          } else {
            Bytes::put_native_u2(param_bcp, Bytes::get_Java_u2(param_bcp));
          }
          continue;
        }
        if (code == Bytecodes::_fast_aldc_w) {
          Bytes::put_native_u2(param_bcp, Bytes::get_Java_u2(param_bcp));
          continue;
        }
        postcond(!Bytecodes::native_byte_order(code));
      }

      if (IS_ZERO && code == Bytecodes::_fast_linearswitch && code == Bytecodes::_fast_binaryswitch) {
        (*stream.bcp()) = Bytecodes::_lookupswitch;
      }
    }
  }

  void parse_code_attr(Method *method, int compressed_linenumber_table_size, TRAPS) {
    precond(method->code_size() > 0);

    {
      const auto max_stack = read<u2>(CHECK);
      const auto max_locals = read<u2>(CHECK);
      method->set_max_stack(max_stack);
      method->set_max_locals(max_locals);
    }

    read_raw(method->code_base(), method->code_size(), CHECK);
    if (_ik_flags.rewritten() && (Endian::is_Java_byte_ordering_different() || IS_ZERO)) {
      fixup_bytecodes(method);
    }

    if (method->has_exception_handler()) {
      STATIC_ASSERT(sizeof(ExceptionTableElement) == 4 * sizeof(u2)); // Check no padding
      const size_t len = method->exception_table_length() * sizeof(ExceptionTableElement) / sizeof(u2);
      read_uint_array_data(reinterpret_cast<u2 *>(method->exception_table_start()), len, CHECK);
    }

    if (method->has_linenumber_table()) {
      assert(compressed_linenumber_table_size > 0, "checked when parsing");
      read_raw(method->compressed_linenumber_table(), compressed_linenumber_table_size, CHECK);
    }
    if (method->has_localvariable_table()) {
      STATIC_ASSERT(sizeof(LocalVariableTableElement) == 6 * sizeof(u2)); // Check no padding
      const size_t len = method->localvariable_table_length() * sizeof(LocalVariableTableElement) / sizeof(u2);
      read_uint_array_data(reinterpret_cast<u2 *>(method->localvariable_table_start()), len, CHECK);
    }
    Array<u1> *const stackmap_table_data = read_uint_array<u1>(nullptr, CHECK);
    if (stackmap_table_data != nullptr) {
      guarantee(!stackmap_table_data->is_empty(), "existing stack map table cannot be empty");
      method->set_stackmap_data(stackmap_table_data);
    }
  }

  static void set_method_flags(Method *method, const ConstMethodFlags &flags) {
    // Check flags that are set based on the legths/sizes we passed
    postcond(method->is_overpass() == flags.is_overpass());
    postcond(method->has_linenumber_table() == flags.has_linenumber_table());
    postcond(method->constMethod()->has_checked_exceptions() == flags.has_checked_exceptions());
    postcond(method->has_localvariable_table() == flags.has_localvariable_table());
    postcond(method->has_exception_handler() == flags.has_exception_table());
    postcond(method->constMethod()->has_generic_signature() == flags.has_generic_signature());
    postcond(method->has_method_parameters() == flags.has_method_parameters());
    postcond(method->constMethod()->has_method_annotations() == flags.has_method_annotations());
    postcond(method->constMethod()->has_parameter_annotations() == flags.has_parameter_annotations());
    postcond(method->constMethod()->has_type_annotations() == flags.has_type_annotations());
    postcond(method->constMethod()->has_default_annotations() == flags.has_default_annotations());
    // Set the rest of the flags
    if (flags.caller_sensitive())       method->set_caller_sensitive();
    if (flags.is_hidden())              method->set_is_hidden();
    if (flags.has_injected_profile())   method->set_has_injected_profile();
    if (flags.reserved_stack_access())  method->set_has_reserved_stack_access();
    if (flags.is_scoped())              method->set_scoped();
    if (flags.changes_current_thread()) method->set_changes_current_thread();
    if (flags.jvmti_mount_transition()) method->set_jvmti_mount_transition();
    if (flags.intrinsic_candidate()) {
      guarantee(!method->is_synthetic(), "synthetic method cannot be an intrinsic candidate");
      method->set_intrinsic_candidate();
    }
  }

  void parse_method(Method **method_out, TRAPS) {
    precond(method_out != nullptr);

    const auto raw_access_flags = read<u2>(CHECK);
    guarantee((raw_access_flags & JVM_RECOGNIZED_METHOD_MODIFIERS) == raw_access_flags,
              "unrecognized method access flags: " UINT16_FORMAT_X_0, raw_access_flags);
    const AccessFlags access_flags(raw_access_flags);
    guarantee(!access_flags.is_final() || _ik_flags.has_final_method(), "class with a final method not marked as such");

    const auto raw_flags = read<jint>(CHECK);
    const ConstMethodFlags flags(raw_flags);
    guarantee(!_class_access_flags.is_hidden_class() || flags.is_hidden(), "methods of hidden class must be marked hidden");
    guarantee(!flags.has_localvariable_table() || _ik_flags.has_localvariable_table(), "class with methods with a local variable table not marked as such");
    guarantee(!_class_access_flags.is_interface() || access_flags.is_static() || access_flags.is_abstract() ||
              flags.is_overpass() /* overpasses don't exist in class files and thus don't count as declared */ ||
              _ik_flags.declares_nonstatic_concrete_methods(),
              "interface with a declared non-static non-abstract method not marked as such");

    const auto raw_statuses = read<jint>(CHECK);
    const MethodFlags statuses(raw_statuses);
    guarantee(!statuses.queued_for_compilation() && !statuses.is_not_c1_compilable() &&
              !statuses.is_not_c2_compilable() && !statuses.is_not_c2_osr_compilable(),
              "illegal internal method statuses: " INT32_FORMAT_X_0, raw_statuses);

    const auto name_index = read<u2>(CHECK);
    guarantee(name_index > 0 && name_index < _cp->length(), "method name index %i is out of constant pool bounds", name_index);
    const auto signature_index = read<u2>(CHECK);
    guarantee(signature_index > 0 && signature_index < _cp->length(), "method descriptor index %i is out of constant pool bounds", signature_index);
    Symbol *const name = _cp->symbol_at(name_index);
    Symbol *const signature = _cp->symbol_at(signature_index);

    const auto code_size = read<u2>(CHECK);
    guarantee(code_size != 0 || (!flags.has_exception_table() && !flags.has_linenumber_table() && !flags.has_localvariable_table()),
              "method cannot have Code attribute's contents in the absence of Code attribute");
    const InlineTableSizes orig_inline_sizes = parse_method_inline_table_sizes(flags, CHECK);

    const auto method_type = flags.is_overpass() ? ConstMethod::MethodType::OVERPASS : ConstMethod::MethodType::NORMAL;
    InlineTableSizes updated_inline_sizes = update_method_inline_table_sizes(orig_inline_sizes);
    Method *const method = Method::allocate(_loader_data, code_size, access_flags, &updated_inline_sizes, method_type, name, CHECK);
    *method_out = method; // Save eagerly to get it deallocated in case of a error

    ClassLoadingService::add_class_method_size(method->size() * wordSize); // ClassFileParser does this, so we do too

    method->set_constants(_cp);
    method->set_name_index(name_index);
    method->set_signature_index(signature_index);
    method->constMethod()->compute_from_signature(signature, access_flags.is_static());

    if (code_size > 0) {
      parse_code_attr(method, orig_inline_sizes.compressed_linenumber_size(), CHECK);
    }
    if (flags.has_checked_exceptions()) {
      STATIC_ASSERT(sizeof(CheckedExceptionElement) == sizeof(u2)); // Check no padding
      const size_t len = method->checked_exceptions_length() * sizeof(CheckedExceptionElement) / sizeof(u2);
      read_uint_array_data(reinterpret_cast<u2 *>(method->checked_exceptions_start()), len, CHECK);
    }
    if (flags.has_method_parameters()) {
      STATIC_ASSERT(sizeof(MethodParametersElement) == 2 * sizeof(u2)); // Check no padding
      const size_t size = orig_inline_sizes.method_parameters_length() * sizeof(MethodParametersElement);
      if (vmClasses::Parameter_klass_loaded()) {
        const size_t len = size / sizeof(u2);
        read_uint_array_data(reinterpret_cast<u2 *>(method->method_parameters_start()), len, CHECK);
      } else {
        skip(size, CHECK);
      }
    }
    if (flags.has_method_annotations()) {
      AnnotationArray *const annots = MetadataFactory::new_array<u1>(_loader_data, orig_inline_sizes.method_annotations_length(), CHECK);
      method->constMethod()->set_method_annotations(annots);
      read_uint_array_data(annots->data(), annots->length(), CHECK);
    }
    if (flags.has_parameter_annotations()) {
      AnnotationArray *const annots = MetadataFactory::new_array<u1>(_loader_data, orig_inline_sizes.parameter_annotations_length(), CHECK);
      method->constMethod()->set_parameter_annotations(annots);
      read_uint_array_data(annots->data(), annots->length(), CHECK);
    }
    if (flags.has_type_annotations()) {
      AnnotationArray *const annots = MetadataFactory::new_array<u1>(_loader_data, orig_inline_sizes.type_annotations_length(), CHECK);
      method->constMethod()->set_type_annotations(annots);
      read_uint_array_data(annots->data(), annots->length(), CHECK);
    }
    if (flags.has_default_annotations()) {
      AnnotationArray *const annots = MetadataFactory::new_array<u1>(_loader_data, orig_inline_sizes.default_annotations_length(), CHECK);
      method->constMethod()->set_default_annotations(annots);
      read_uint_array_data(annots->data(), annots->length(), CHECK);
    }

    set_method_flags(method, flags);

    const bool is_compiled_lambda_form = read_bool(CHECK);
    if (is_compiled_lambda_form) {
      precond(method->intrinsic_id() == vmIntrinsics::_none);
      method->set_intrinsic_id(vmIntrinsics::_compiledLambdaForm);
      postcond(method->is_compiled_lambda_form());
    }

    method->set_statuses(statuses);

    // Not a guarantee because is_vanilla_constructor() call may take some time
    assert(!(_super == nullptr ||
             (_super->has_vanilla_constructor() &&
              name == vmSymbols::object_initializer_name() &&
              signature == vmSymbols::void_method_signature() &&
              method->is_vanilla_constructor())) ||
           _ik_flags.has_vanilla_constructor(),
           "class with a vanilla constructor not marked as such");

    NOT_PRODUCT(method->verify());
  }

  NOT_DEBUG(static) TriBool is_finalizer(const Method &method) {
    if (!InstanceKlass::is_finalization_enabled()) {
      assert(!_class_access_flags.has_finalizer(), "must have been unset");
      return {};    // Not a finalizer
    }
    if (method.name() != vmSymbols::finalize_method_name() || method.signature() != vmSymbols::void_method_signature()) {
      return {};    // Not a finalizer
    }
    if (method.is_empty_method()) {
      return false; // Empty finalizer
    }
    return true;    // Non-empty finalizer
  }

  void parse_methods(TRAPS) {
    const auto methods_num = read<u2>(CHECK);
    if (methods_num > 0) {
      // Pre-fill with nulls so that deallocation works correctly if an error occures before the array is filled
      _methods = MetadataFactory::new_array<Method *>(_loader_data, methods_num, nullptr, CHECK);
    } else {
      _methods = Universe::the_empty_method_array();
    }
    if (JvmtiExport::can_maintain_original_method_order() || Arguments::is_dumping_archive()) {
      _original_method_ordering = MetadataFactory::new_array<int>(_loader_data, methods_num, CHECK);
    } else {
      _original_method_ordering = Universe::the_empty_int_array();
    }

    TriBool has_finalizer; // Default - no finalizer, false - empty finalizer, true - non-empty finalizer
    for (u2 i = 0; i < methods_num; i++) {
      const auto orig_i = read<u2>(CHECK);
      guarantee(orig_i < methods_num, "original method index %i exceeds the number of methods %i", orig_i, methods_num);
      if (_original_method_ordering != Universe::the_empty_int_array()) {
        _original_method_ordering->at_put(i, orig_i);
      }

      parse_method(_methods->adr_at(i), CHECK);

      const TriBool is_fin = is_finalizer(*_methods->at(i));
      if (!is_fin.is_default()) continue; // Not a finalizer
      guarantee(has_finalizer.is_default(), "class defines multiple finalizers");
      has_finalizer = is_fin;             // Set the finalizer info
    }
    log_trace(crac, class, parser)("  Parsed %i methods", methods_num);

    if (has_finalizer || (has_finalizer.is_default() && _super != nullptr && _super->has_finalizer())) {
      assert(!InstanceKlass::is_finalization_enabled(), "has_finalizer should not be set");
      _class_access_flags.set_has_finalizer();
    } else {
      assert(!_class_access_flags.has_finalizer(), "must have been unset");
    }

    const u2 default_methods_num = read<u2>(CHECK);
    if (default_methods_num > 0) {
      guarantee(_ik_flags.has_nonstatic_concrete_methods(),
                "class without default methods in its hierarchy should not have default methods");

      // Pre-fill with nulls so that deallocation works correctly if an error occures before the array is filled
      _default_methods = MetadataFactory::new_array<Method *>(_loader_data, default_methods_num, nullptr, CHECK);
      for (u2 i = 0; i < default_methods_num; i++) {
        const auto method_identification = read_method_identification(CHECK);

        const HeapDump::ID holder_id = method_identification.first;
        InstanceKlass **const holder_ptr = _created_classes.get(holder_id);
        // Implemented interfaces have been parsed and found as loaded, so if
        // it was one of them we would have found it
        guarantee(holder_ptr != nullptr, "default method %i belongs to a class not implemented by this class", i);
        InstanceKlass &holder = **holder_ptr;
        // Would be great to check that the holder is among transitive
        // interfaces, but it requires iterating over them

        const InterclassRefs::MethodDescription method_desc = method_identification.second;
        Symbol *const name = _heap_dump.get_symbol(method_desc.name_id);
        Symbol *const sig = _heap_dump.get_symbol(method_desc.sig_id);
        Method *const method = CracClassDumpParser::find_method(&holder, name, sig, method_desc.kind, false, CHECK);
        guarantee(method != nullptr, "default method #%i cannot be found as %s method %s",
                  i, CracClassDump::method_kind_name(method_desc.kind), Method::name_and_sig_as_C_string(*holder_ptr, name, sig));
        guarantee(method->is_default_method(), "default method %i resolved to a non-default %s", i, method->external_name());
        _default_methods->at_put(i, method);
      }
    }
    log_trace(crac, class, parser)("  Parsed %i default methods", default_methods_num);
  }

  void parse_cached_class_file(TRAPS) {
    const auto len = read<jint>(CHECK);
    if (len == CracClassDump::NO_CACHED_CLASS_FILE_SENTINEL) {
      log_trace(crac, class, parser)("  No cached class file");
      return;
    }

#if INCLUDE_JVMTI
    _cached_class_file = reinterpret_cast<JvmtiCachedClassFileData *>(
      AllocateHeap(offset_of(JvmtiCachedClassFileData, data) + len, mtInternal));
    postcond(_cached_class_file != nullptr);

    _cached_class_file->length = len;
    read_raw(_cached_class_file->data, len, CHECK);
#else // INCLUDE_JVMTI
    THROW_MSG(vmSymbols::java_lang_UnsupportedOperationException(),
              "class file has been modified by a JVM TI agent, "
              "making this dump restorable only on VMs that have JVM TI included");
#endif // INCLUDE_JVMTI

    log_trace(crac, class, parser)("  Parsed cached class file");
  }

  // Parses the class dump. Roughly equivalent to ClassFileParser's constructor.
  void parse_class(TRAPS) {
    parse_class_state(CHECK);
    parse_class_versions(CHECK);
    parse_class_flags(CHECK);
    parse_class_attrs(CHECK);
    parse_constant_pool(CHECK);
    if (_ik_flags.rewritten()) {
      parse_constant_pool_cache(CHECK);
    }
    parse_this_class_index(CHECK);
    find_super(CHECK);
    parse_interfaces(CHECK);
    parse_fields(CHECK);
    parse_methods(CHECK);
    parse_cached_class_file(CHECK);
    log_trace(crac, class, parser)("  Instance class dump parsing completed");
  }

  // ###########################################################################
  // Class creation
  // ###########################################################################

  int compute_vtable_size(const Symbol *class_name DEBUG_ONLY(COMMA TRAPS)) {
    precond(_transitive_interfaces != nullptr);

    ResourceMark rm;
    int vtable_size;
    int num_mirandas; GrowableArray<Method *> all_mirandas; // Filled but shouldn't be used (see the comments below)
    const Handle loader_h(Thread::current(), _loader_data->class_loader());
    klassVtable::compute_vtable_size_and_num_mirandas(&vtable_size, &num_mirandas, &all_mirandas, _super, _methods,
                                                      _class_access_flags, _major_version, loader_h, class_name, _local_interfaces);

#ifdef ASSERT
    // The parsed methods already include overpass methods which are normally
    // only generated after computing the mirandas above. Because some of the
    // overpasses can be ex-mirandas the mirandas list computed above may be
    // incomplete, so recompute without the overpasses to do the asserts below.
    GrowableArray<Method *> methods_no_overpasses_tmp(_methods->length());
    for (int i = 0; i < _methods->length(); i++) {
      Method *const m = _methods->at(i);
      if (!m->is_overpass()) {
        methods_no_overpasses_tmp.append(m);
      }
    }
    log_trace(crac, class, parser)("  Class has %i overpass methods", _methods->length() - methods_no_overpasses_tmp.length());

    Array<Method *> *const methods_no_overpasses = MetadataFactory::new_array<Method *>(_loader_data, methods_no_overpasses_tmp.length(), CHECK_0);
    if (!methods_no_overpasses->is_empty()) {
      memcpy(methods_no_overpasses->data(), methods_no_overpasses_tmp.adr_at(0), methods_no_overpasses->length() * sizeof(Method *));
    }

    int vtable_size_debug;
    int num_mirandas_debug; GrowableArray<Method *> all_mirandas_debug; // These will be the right values
    klassVtable::compute_vtable_size_and_num_mirandas(&vtable_size_debug, &num_mirandas_debug, &all_mirandas_debug, _super, methods_no_overpasses,
                                                      _class_access_flags, _major_version, loader_h, class_name, _local_interfaces);
    assert(vtable_size == vtable_size_debug, "absence of overpass methods should not change the vtable size");
    assert(num_mirandas <= num_mirandas_debug, "overpasses might have been mirandas");

    const bool has_miranda_methods = num_mirandas_debug > 0 || (_super != nullptr && _super->has_miranda_methods());
    assert(_ik_flags.has_miranda_methods() == has_miranda_methods,
          "internal instance class flag 'has miranda methods' dumped with incorrect value: expected %s", BOOL_TO_STR(has_miranda_methods));
    MetadataFactory::free_array(_loader_data, methods_no_overpasses);
#endif

    return vtable_size;
  }

  FieldLayoutInfo compute_field_layout(const Symbol *class_name) {
    FieldLayoutInfo field_layout_info; // Will contain resource-allocated data
    FieldLayoutBuilder lb(class_name, _super, _cp, &_field_infos, _ik_flags.is_contended(), &field_layout_info);
    lb.build_layout(); // Fills FieldLayoutInfo and offsets of field infos
    return field_layout_info;
  }

  static Annotations *create_combined_annotations(ClassLoaderData *loader_data,
                                                  AnnotationArray *class_annos,
                                                  AnnotationArray *class_type_annos,
                                                  Array<AnnotationArray *> *field_annos,
                                                  Array<AnnotationArray *> *field_type_annos,
                                                  TRAPS) {
    if (class_annos == nullptr && class_type_annos == nullptr &&
        field_annos == nullptr && field_type_annos == nullptr) {
      return nullptr; // Don't create the Annotations object unnecessarily.
    }

    Annotations* const annotations = Annotations::allocate(loader_data, CHECK_NULL);
    annotations->set_class_annotations(class_annos);
    annotations->set_class_type_annotations(class_type_annos);
    annotations->set_fields_annotations(field_annos);
    annotations->set_fields_type_annotations(field_type_annos);
    return annotations;
  }

  void move_data_to_class(TRAPS) {
    InstanceKlass &ik = *_ik;
    // Move everything we've parsed so far and null the pointers so that they
    // won't get freed in the destructor

    _cp->set_operands(_bsm_operands);
    _cp->set_pool_holder(&ik);
    ik.set_constants(_cp); // Must do this before setting the indices below
    _bsm_operands = nullptr;
    _cp = nullptr;

    ik.set_nest_members(_nest_members);
    ik.set_inner_classes(_inner_classes);
    ik.set_source_debug_extension(_source_debug_extension);
    ik.set_record_components(_record_components);
    ik.set_permitted_subclasses(_permitted_subclasses);
    _nest_members = nullptr;
    _inner_classes = nullptr;
    _source_debug_extension = nullptr;
    _record_components = nullptr;
    _permitted_subclasses = nullptr;

    Annotations *const combined_annotations = create_combined_annotations(_loader_data,
                                                                          _class_annotations, _class_type_annotations,
                                                                          _field_annotations, _field_type_annotations,
                                                                          CHECK);
    ik.set_annotations(combined_annotations);
    _class_annotations = nullptr;
    _class_type_annotations = nullptr;
    _field_annotations = nullptr;
    _field_type_annotations = nullptr;

    ik.set_fieldinfo_stream(_field_info_stream);
    ik.set_fields_status(_field_statuses);
    _field_info_stream = nullptr;
    _field_statuses = nullptr;

    ik.set_methods(_methods);
    ik.set_method_ordering(_original_method_ordering);
    ik.set_default_methods(_default_methods); // Vtable indices for these will be set later not to get an allocation exception here
    _methods = nullptr;
    _original_method_ordering = nullptr;
    _default_methods = nullptr;

    ik.initialize_supers(_super, _transitive_interfaces, CHECK);
    ik.set_local_interfaces(_local_interfaces);
    ik.set_transitive_interfaces(_transitive_interfaces);
    _local_interfaces = nullptr;
    _transitive_interfaces = nullptr;
    // No need to set super to null because the destructor won't free it

#if INCLUDE_JVMTI
    ik.set_cached_class_file(_cached_class_file);
    _cached_class_file = nullptr;
#endif // INCLUDE_JVMTI
  }

  // Allocates and fills the class. Roughly equivalent to
  // ClassFileParser::create_instance_class().
  void create_class(TRAPS) {
    Thread *const thread = Thread::current();
    Symbol *const class_name = _heap_dump.get_class_name(_class_dump.id);

    // Allocate the class

    // TODO instead of re-computing the sizes from the ground up save
    //  vtable/itable lengths and quickly compute the sizes based on them
    _transitive_interfaces = ClassFileParser::compute_transitive_interfaces(_super, _local_interfaces, _loader_data, CHECK);
    Method::sort_methods(_methods); // Sort before they'll be used in vtable-related computations
    const int vtable_size = compute_vtable_size(class_name DEBUG_ONLY(COMMA CHECK));
    const int itable_size = !_class_access_flags.is_interface() ? klassItable::compute_itable_size(_transitive_interfaces) : 0;

    ResourceMark rm; // For FieldLayoutInfo contents
    FieldLayoutInfo field_layout_info = compute_field_layout(class_name); // Also fills offsets in _field_infos
    _field_info_stream = FieldInfoStream::create_FieldInfoStream(&_field_infos, _java_fields_num, _injected_fields_num, _loader_data, CHECK);
    _field_infos.clear_and_deallocate(); // Don't need them anymore
    guarantee(field_layout_info._has_nonstatic_fields == _ik_flags.has_nonstatic_fields(),
              "internal instance class flag 'has nonstatic fields' dumped with incorrect value: expected %s",
              BOOL_TO_STR(field_layout_info._has_nonstatic_fields));

    const InstanceKlassSizes ik_sizes{vtable_size, itable_size, field_layout_info._instance_size,
                                      field_layout_info._static_field_size,
                                      field_layout_info.oop_map_blocks->_nonstatic_oop_map_count};
    InstanceKlass &ik = *InstanceKlass::allocate_instance_klass(_loader_data, class_name, _super, _class_access_flags, ik_sizes, CHECK);
    _ik = &ik; // Set eagerly to get it deallocated in case of a error

    // Fill the allocated class

    ik.set_class_loader_data(_loader_data);
    ik.set_name(class_name);

    _loader_data->add_class(&ik, /* publicize = */ false);

    ik.set_internal_flags(_ik_flags);

    ik.set_nonstatic_field_size(field_layout_info._nonstatic_field_size);
    ik.set_static_oop_field_count(_static_oop_fields_num);
    // has_nonstatic_fields is set via internal class flags

    move_data_to_class(CHECK); // Cannot use the majority of the parser's fields from this point on

    // These require constant pool to be set
    ik.set_source_file_name_index(_source_file_name_index);
    ik.set_generic_signature_index(_generic_signature_index);
    ik.set_nest_host_index(_nest_host_index);

    // Method-related flags (including has_miranda_methods) has already been
    // checked, the original method ordering has also been set
    // TODO JVM TI RedefineClasses support may require this to be handeled
    //  differently (save/restore _idnum_allocated_count or take max idnum of
    //  all methods in this class and its previous versions)
    ik.set_initial_method_idnum(checked_cast<u2>(ik.methods()->length()));

    ik.set_this_class_index(_this_class_index);
    // Resolution of this class index for a hidden class will be done later,
    // together with the rest of the class references

    ik.set_minor_version(_minor_version);
    ik.set_major_version(_major_version);

    ClassLoaderData *non_reflection_loader_data =
      ClassLoaderData::class_loader_data_or_null(java_lang_ClassLoader::non_reflection_class_loader(ik.class_loader()));
    ik.set_package(non_reflection_loader_data, nullptr, CHECK);

    ClassFileParser::check_methods_for_intrinsics(&ik);

    // Update the corresponding CDS flag (which we don't save)
    if (_is_value_based) {
      ik.set_has_value_based_class_annotation();
    }
    // Other annotations- and attributes-related flags and values have already
    // been set

    // Interfaces have been already set, so can do this
    klassItable::setup_itable_offset_table(&ik);

    const OopMapBlocksBuilder &oop_map_blocks = *field_layout_info.oop_map_blocks;
    if (oop_map_blocks._nonstatic_oop_map_count > 0) {
      oop_map_blocks.copy(ik.start_of_nonstatic_oop_maps());
    }

    ClassFileParser::check_can_allocate_fast(&ik);
    // Other "precomputed" flags have been checked/set already

    // Access control checks are skipped for simplicity (if no one tampered with
    // the dump, this should've been checked when loading the class)

    precond(ik.is_being_restored()); // Makes create_mirror() omit static field initialization
    java_lang_Class::create_mirror(&ik,
                                   Handle(thread, _loader_data->class_loader()),
                                   Handle(thread, ik.module()->module()),
                                   Handle(), Handle(), // Prot. domain and class data -- to be restored later
                                   CHECK);

    if (ik.default_methods() != nullptr) {
      precond(ik.has_nonstatic_concrete_methods());
      Method::sort_methods(ik.default_methods(), /*set_idnums=*/ false);
      ik.create_new_default_vtable_indices(ik.default_methods()->length(), CHECK);
    }

    // TODO JVMTI redefine/retransform support: if the class was changed by a
    //  class loading hook, set has_default_read_edges flag for its module
    //  (that's what ClassFileParser does)

    ClassLoadingService::notify_class_loaded(&ik, false);

    JFR_ONLY(INIT_ID(&ik));

    JVMTI_ONLY(ik.constants()->set_version(_redefinition_version));

    DEBUG_ONLY(ik.verify());

    if (log_is_enabled(Debug, crac, class, parser)) {
      log_debug(crac, class, parser)("  Instance class created: %s", ik.external_name());
    }
  }
};

void CracClassDumpParser::parse(const char *path, const ParsedHeapDump &heap_dump,
                                ClassLoaderProvider *loader_provider,
                                HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> *iks,
                                HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> *aks,
                                HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> *unfilled_infos, TRAPS) {
  precond(path != nullptr && loader_provider != nullptr && iks != nullptr && aks != nullptr);
  log_info(crac, class, parser)("Started parsing class dump %s", path);

  FileBasicTypeReader reader;
  if (!reader.open(path)) {
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(),
              err_msg("Cannot open %s for reading: %s", path, os::strerror(errno)));
  }

  CracClassDumpParser dump_parser(&reader, heap_dump, loader_provider, iks, aks, unfilled_infos, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    Handle cause(Thread::current(), PENDING_EXCEPTION);
    CLEAR_PENDING_EXCEPTION;
    THROW_MSG_CAUSE(vmSymbols::java_lang_IllegalArgumentException(),
                    err_msg("Failed to create classes from dump %s", path), cause);
  } else {
    log_info(crac, class, parser)("Successfully parsed class dump %s", path);
  }
}

Method *CracClassDumpParser::find_method(InstanceKlass *holder,
                                         Symbol *name, Symbol *signature, CracClassDump::MethodKind kind,
                                         bool lookup_signature_polymorphic, TRAPS) {
  precond(holder != nullptr);
  if (lookup_signature_polymorphic && MethodHandles::is_signature_polymorphic_intrinsic_name(holder, name)) {
    // Signature polymorphic methods' specializations are dynamically generated,
    // but we only need to treat the basic (non-generic, intrinsic) ones
    // specially because the rest are generated as classes that should be in the
    // dump
    return LinkResolver::resolve_intrinsic_polymorphic_method(holder, name, signature, THREAD);
  }
  return holder->find_local_method(name, signature,
                                   CracClassDump::as_overpass_lookup_mode(kind),
                                   CracClassDump::as_static_lookup_mode(kind),
                                   Klass::PrivateLookupMode::find);
}

CracClassDumpParser::CracClassDumpParser(BasicTypeReader *reader, const ParsedHeapDump &heap_dump, ClassLoaderProvider *loader_provider,
                                         HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> *iks, HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> *aks,
                                         HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> *unfilled_infos, TRAPS) :
    ClassDumpReader(reader), _heap_dump(heap_dump), _loader_provider(loader_provider), _iks(iks), _aks(aks), _unfilled_infos(unfilled_infos) {
  if (Arguments::is_dumping_archive()) {
    // TODO should do something like ClassLoader::record_result() after loading each class
    log_warning(crac, class, parser, cds)("Classes restored by CRaC will not be included into the CDS archive");
  }
  parse_header(CHECK);
  parse_primitive_array_classes(CHECK);
  {
    ResourceMark rm;
    const GrowableArray<Pair<HeapDump::ID, InterclassRefs>> &interclass_refs = parse_instance_and_obj_array_classes(CHECK);
    for (const Pair<HeapDump::ID, InterclassRefs> &id_to_refs : interclass_refs) {
      CracClassStateRestorer::fill_interclass_references(*_iks->get(id_to_refs.first), _heap_dump, *_iks, *_aks, id_to_refs.second, CHECK);
    }
  }
  parse_initiating_loaders(CHECK);
};

void CracClassDumpParser::parse_header(TRAPS) {
  constexpr char HEADER_STR[] = "CRAC CLASS DUMP 0.1";

  char header_str[sizeof(HEADER_STR)];
  read_raw(header_str, sizeof(header_str), CHECK);
  header_str[sizeof(header_str) - 1] = '\0'; // Ensure nul-terminated
  if (strcmp(header_str, HEADER_STR) != 0) {
    THROW_MSG(vmSymbols::java_lang_IllegalArgumentException(), err_msg("Unknown header string: %s", header_str));
  }

  const auto id_size = read<u2>(CHECK);
  set_id_size(id_size, CHECK);

  const auto compressed_vm_options = read<u1>(CHECK);
  guarantee(CracClassDump::is_vm_options(compressed_vm_options), "unrecognized VM options");

  const bool was_sync_on_value_based_classes_diagnosed = is_set_nth_bit(compressed_vm_options, CracClassDump::is_sync_on_value_based_classes_diagnosed_shift);
  const bool were_all_annotation_preserved = is_set_nth_bit(compressed_vm_options, CracClassDump::are_all_annotations_preserved_shift);
  if ((DiagnoseSyncOnValueBasedClasses != 0) && !was_sync_on_value_based_classes_diagnosed) {
    if (!were_all_annotation_preserved) {
      // TODO either save the InstanceKlass::is_value_based() flag regardless
      //  of that option (like CDS does via
      //  Klass::has_value_based_class_annotation()) or parse annotations of
      //  each class to recompute InstanceKlass::is_value_based()
      log_warning(crac, class, parser)("Checkpointed VM wasn't diagnosing syncronization on value-based classes, but this VM is requested to (by the corresponding option). "
                                       "This will not be fulfullied for the restored classes.");
    } else {
      log_warning(crac, class, parser)("Checkpointed VM wasn't diagnosing syncronization on value-based classes, but this VM is requested to (by the corresponding option). "
                                       "This will not be fulfullied for the restored classes because the checkpointed VM also preserved RuntimeInvisibleAnnotations "
                                       "making them indistinguishable from RuntimeVisibleAnnotations.");
    }
  }
  if (were_all_annotation_preserved != PreserveAllAnnotations) {
    log_warning(crac, class, parser)("Checkpointed VM %s, but this VM is requested to %s them (by the corresponding option). "
                                      "This will not be fulfullied for the restored classes.",
                                      were_all_annotation_preserved ? "preserved RuntimeInvisibleAnnotations making them indistinguishable from RuntimeVisibleAnnotations" :
                                                                      "didn't preserve RuntimeInvisibleAnnotations",
                                      PreserveAllAnnotations        ? "preserve" :
                                                                      "omit");
  }

  log_debug(crac, class, parser)("Parsed class dump header: ID size = %i", id_size);
}

void CracClassDumpParser::parse_obj_array_classes(Klass *bottom_class, TRAPS) {
  precond(bottom_class->is_instance_klass() || bottom_class->is_typeArray_klass());
  Klass *cur_k = bottom_class;
  const auto num_arrays = read<u1>(CHECK);
  for (u1 i = 0; i < num_arrays; i++) {
    const HeapDump::ID obj_array_class_id = read_id(false, CHECK);
    ArrayKlass *const ak = cur_k->array_klass(CHECK);
    precond(!_aks->contains(obj_array_class_id));
    _aks->put_when_absent(obj_array_class_id, ak);
    cur_k = ak;
  }

  if (log_is_enabled(Trace, crac, class, parser)) {
    ResourceMark rm;
    log_trace(crac, class, parser)("Parsed object array classes with bottom class %s", bottom_class->external_name());
  }
}

void CracClassDumpParser::parse_primitive_array_classes(TRAPS) {
  precond(Universe::is_fully_initialized());
  for (u1 t = JVM_T_BOOLEAN; t <= JVM_T_LONG; t++) {
    const HeapDump::ID prim_array_class_id = read_id(false, CHECK);
    Klass *const tak = Universe::typeArrayKlassObj(static_cast<BasicType>(t));
    precond(!_aks->contains(prim_array_class_id));
    _aks->put_when_absent(prim_array_class_id, TypeArrayKlass::cast(tak));
    parse_obj_array_classes(tak, CHECK);
  }
  {
    const HeapDump::ID filler_array_class_id = read_id(false, CHECK);
    Klass *const tak = Universe::fillerArrayKlassObj();
    precond(!_aks->contains(filler_array_class_id));
    _aks->put_when_absent(filler_array_class_id, TypeArrayKlass::cast(tak));
    parse_obj_array_classes(tak, CHECK);
  }
  log_debug(crac, class, parser)("Parsed primitive array classes");
}

struct CracClassDumpParser::ClassPreamble {
  HeapDump::ID class_id = HeapDump::NULL_ID;
  CracClassDump::ClassLoadingKind loading_kind;
};

CracClassDumpParser::ClassPreamble CracClassDumpParser::parse_instance_class_preamble(TRAPS) {
  const HeapDump::ID class_id = read_id(true, CHECK_({}));
  if (class_id == HeapDump::NULL_ID) {
    return {};
  }
  assert(!_iks->contains(class_id), "class " HDID_FORMAT " is repeated", class_id);

  const auto loading_kind = read<u1>(CHECK_({}));
  guarantee(CracClassDump::is_class_loading_kind(loading_kind), "class " HDID_FORMAT " has unrecognized loading kind %i",
            class_id, loading_kind);

  log_debug(crac, class, parser)("Parsed instance class preamble: ID " HDID_FORMAT ", loading kind %i",
                                  class_id, loading_kind);
  return {class_id, checked_cast<CracClassDump::ClassLoadingKind>(loading_kind)};
}

Handle CracClassDumpParser::get_class_loader(HeapDump::ID loader_id, TRAPS) {
#ifdef ASSERT
  if (loader_id != HeapDump::NULL_ID) {
    const HeapDump::InstanceDump &loader_dump = _heap_dump.get_instance_dump(loader_id);
    assert(_iks->contains(loader_dump.class_id), "incorrect dump order: class dumped before its class loader");
    const InstanceKlass &loader_class = **_iks->get(loader_dump.class_id);
    if (!loader_class.is_being_restored()) {
      precond(loader_class.is_initialized() || loader_class.is_in_error_state());
      assert(loader_class.is_initialized(), "class loader " HDID_FORMAT " cannot be used to load classes: "
             "its class %s has failed to initialize", loader_id, loader_class.external_name());
    } else {
      precond(_unfilled_infos->contains(loader_dump.class_id));
      assert(_unfilled_infos->get(loader_dump.class_id)->target_state == InstanceKlass::fully_initialized,
             "class loader " HDID_FORMAT " cannot be used to load classes: its class %s was not initialized at dump time",
             loader_id, loader_class.external_name());
    }
  }
#endif
  const Handle class_loader = _loader_provider->get_class_loader(loader_id, CHECK_NH);
  postcond(class_loader.is_null() || class_loader->klass()->is_class_loader_instance_klass());
  guarantee(!java_lang_ClassLoader::is_reflection_class_loader(class_loader()),
            "defining loader must be a non-reflection one");
  return class_loader;
}

InstanceKlass *CracClassDumpParser::parse_and_define_instance_class(const HeapDump::ClassDump &class_dump,
                                                                    ClassLoaderData *loader_data,
                                                                    InterclassRefs *refs_out, TRAPS) {
  const CracInstanceClassDumpParser ik_parser(id_size(), reader(), _heap_dump, *_iks, class_dump, loader_data, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    const Handle cause(Thread::current(), PENDING_EXCEPTION);
    CLEAR_PENDING_EXCEPTION;
    THROW_MSG_CAUSE_(vmSymbols::java_lang_Exception(), err_msg("Cannot create class " HDID_FORMAT, class_dump.id), cause, {});
  }

  InstanceKlass *const ik = CracClassStateRestorer::define_created_class(ik_parser.created_class(), ik_parser.class_state(), CHECK_NULL);
  precond(!_iks->contains(class_dump.id));
  _iks->put_when_absent(class_dump.id, ik);
  _iks->maybe_grow();

  precond(!_unfilled_infos->contains(class_dump.id));
  if (ik->is_being_restored()) {
    _unfilled_infos->put_when_absent(class_dump.id, {ik_parser.class_state(), ik_parser.class_initialization_error_id()});
    _unfilled_infos->maybe_grow();
  }

  *refs_out = ik_parser.interclass_references();

  return ik;
}

GrowableArray<Pair<HeapDump::ID, InterclassRefs>> CracClassDumpParser::parse_instance_and_obj_array_classes(TRAPS) {
  HandleMark hm(Thread::current()); // Class loader handles
  GrowableArray<Pair<HeapDump::ID, InterclassRefs>> interclass_refs;
  for (ClassPreamble preamble = parse_instance_class_preamble(THREAD);
       !HAS_PENDING_EXCEPTION && preamble.class_id != HeapDump::NULL_ID;
       preamble = parse_instance_class_preamble(THREAD)) {
    assert(!_iks->contains(preamble.class_id), "instance class " HDID_FORMAT " dumped multiple times", preamble.class_id);

    const HeapDump::ClassDump *class_dump = _heap_dump.class_dumps.get(preamble.class_id);
    guarantee(class_dump != nullptr, "class " HDID_FORMAT " not found in heap dump", preamble.class_id);

    // TODO What to do with hidden classes? They have uniquely-generated names,
    //  so we won't find them by (class loader, class name) pair even if we
    //  iterate through all CLDs of the loader and all classes recorded it these
    //  CLD's class lists. This is a problem since we'll restore such classes
    //  even if they exist thus duplicating them.

    const Handle loader = get_class_loader(class_dump->class_loader_id, CHECK_({}));
    ClassLoaderData *const loader_data = SystemDictionary::register_loader(loader, preamble.loading_kind == CracClassDump::ClassLoadingKind::NON_STRONG_HIDDEN);

    InterclassRefs refs;
    InstanceKlass *const ik = parse_and_define_instance_class(*class_dump, loader_data, &refs, CHECK_({}));
    interclass_refs.append({class_dump->id, refs});

    parse_obj_array_classes(ik, CHECK_({}));
  }
  return interclass_refs;
}

void CracClassDumpParser::parse_initiating_loaders(TRAPS) {
  for (HeapDump::ID loader_id = read_id(true, THREAD);
       !HAS_PENDING_EXCEPTION && loader_id != HeapDump::NULL_ID;
       loader_id = read_id(true, THREAD)) {
    guarantee(loader_id != HeapDump::NULL_ID, "bootstrap loader cannot be a non-defining initiating loader");
    const Handle loader = get_class_loader(loader_id, CHECK);
    assert(loader->klass()->is_class_loader_instance_klass(), HDID_FORMAT " cannot be an initiating loader: "
           "its class is %s which is not a class loader class", loader_id, loader->klass()->external_name());
    const auto initiated_classes_num = read<jint>(CHECK);
    guarantee(initiated_classes_num >= 0, "amount of initiated classes cannot be negative");
    for (jint i = 0; i < initiated_classes_num; i++) {
      const HeapDump::ID class_id = read_id(false, CHECK);
      InstanceKlass **const ik = _iks->get(class_id);
      guarantee(ik != nullptr, "unknown class " HDID_FORMAT " dumped as initiated by class loader " HDID_FORMAT, class_id, loader_id);
      SystemDictionary::record_initiating_loader(*ik, loader, CHECK);
      log_trace(crac, class)("Recorded %s as initiating loader of %s defined by %s",
                             java_lang_ClassLoader::loader_data(loader())->loader_name_and_id(),
                             (*ik)->external_name(), (*ik)->class_loader_data()->loader_name_and_id());
    }
  }
  log_debug(crac, class, parser)("Parsed initiating loaders");
}
