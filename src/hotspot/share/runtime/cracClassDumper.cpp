#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/resolutionErrors.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "classfile_constants.h"
#include "interpreter/bytecode.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/bytecodes.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "metaprogramming/enableIf.hpp"
#include "oops/annotations.hpp"
#include "oops/array.hpp"
#include "oops/constMethod.hpp"
#include "oops/constantPool.hpp"
#include "oops/cpCache.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/fieldInfo.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceKlassFlags.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/methodFlags.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/recordComponent.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "oops/symbol.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "runtime/arguments.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/globals.hpp"
#include "runtime/handles.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/accessFlags.hpp"
#include "utilities/basicTypeWriter.hpp"
#include "utilities/bytes.hpp"
#include "utilities/constantTag.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/macros.hpp"
#include "utilities/resizeableResourceHash.hpp"
#include <cstdint>
#include <limits>
#include <type_traits>
#ifdef INCLUDE_JVMCI
#include "jvmci/jvmci_globals.hpp"
#endif
#ifdef ASSERT
#include "jvm_constants.h"
#endif // ASSERT

// Write IDs the same way HPROF heap dumper does.
static bool write_symbol_id(BasicTypeWriter *writer, const Symbol *s) {
  return writer->write(reinterpret_cast<uintptr_t>(s));
}
static bool write_object_id(BasicTypeWriter *writer, const oop o) {
  return writer->write(cast_from_oop<uintptr_t>(o));
}
static bool write_class_id(BasicTypeWriter *writer, const Klass &k) {
  assert(cast_from_oop<uintptr_t>(k.java_mirror()) != HeapDump::NULL_ID, "footer assumption");
  return write_object_id(writer, k.java_mirror());
}

class ClassDumpWriter : public KlassClosure, public CLDClosure {
 friend ClassLoaderDataGraph; // Provide access to do_klass()
 private:
  BasicTypeWriter *_writer;
  const char *_io_error_msg = nullptr;
  ResizeableResourceHashtable<const InstanceKlass *, bool> _dumped_classes{107, 1228891};

 public:
  explicit ClassDumpWriter(BasicTypeWriter *writer) : _writer(writer) {}

  const char *io_error_msg() const { return _io_error_msg; }

  void write_dump() {
    precond(io_error_msg() == nullptr); write_header();
    if (io_error_msg() == nullptr)      write_primitive_array_class_ids();
    // Instance and object array classes. Not using loaded_classes_do() because
    // our filter should be quicker.
    if (io_error_msg() == nullptr)      ClassLoaderDataGraph::classes_do(this);
    if (io_error_msg() == nullptr)      write_end_sentinel();
    log_debug(crac, class, dump)("Wrote instance and object array classes");
    if (io_error_msg() == nullptr)      ClassLoaderDataGraph::cld_do(this);
    if (io_error_msg() == nullptr)      write_end_sentinel();
    log_debug(crac, class, dump)("Wrote initiating class loaders info");
  }

 private:
  // ###########################################################################
  // Helpers
  // ###########################################################################

#define WRITE(value)           do { if (!_writer->write(value))           { _io_error_msg = os::strerror(errno); return; } } while (false)
#define WRITE_SYMBOL_ID(value) do { if (!write_symbol_id(_writer, value)) { _io_error_msg = os::strerror(errno); return; } } while (false)
#define WRITE_OBJECT_ID(value) do { if (!write_object_id(_writer, value)) { _io_error_msg = os::strerror(errno); return; } } while (false)
#define WRITE_CLASS_ID(value)  do { if (!write_class_id(_writer, value))  { _io_error_msg = os::strerror(errno); return; } } while (false)
#define WRITE_RAW(buf, size)   do { if (!_writer->write_raw(buf, size))   { _io_error_msg = os::strerror(errno); return; } } while (false)
#define DO_CHECKED(expr)       do { expr; if (_io_error_msg != nullptr) return;                                            } while (false)

  void write_obj_array_class_ids(Klass *bottom_class) {
    ResourceMark rm;
    GrowableArray<const ObjArrayKlass *> oaks;
    for (auto *ak = bottom_class->array_klass_or_null();
         ak != nullptr;
         ak = ak->array_klass_or_null()) {
      oaks.append(ObjArrayKlass::cast(ak));
    }

    assert(oaks.length() + (bottom_class->is_array_klass() ? ArrayKlass::cast(bottom_class)->dimension() : 0) <= 255,
           "arrays can have up to 255 dimensions");
    WRITE(checked_cast<u1>(oaks.length()));
    for (const ObjArrayKlass *oak : oaks) {
      WRITE_CLASS_ID(*oak);
    }
  }

  template <class UINT_T, ENABLE_IF(std::is_same<UINT_T, u1>::value || std::is_same<UINT_T, u2>::value ||
                                    std::is_same<UINT_T, u4>::value || std::is_same<UINT_T, u8>::value)>
  void write_uint_array_data(const UINT_T *data, size_t length) {
    if (Endian::is_Java_byte_ordering_different() && sizeof(UINT_T) > 1) { // Have to convert
      for (size_t i = 0; i < length; i++) {
        WRITE(data[i]);
      }
    } else { // Can write as is
      WRITE_RAW(data, length * sizeof(UINT_T));
    }
  }

  template <class UINT_T>
  void write_uint_array(const Array<UINT_T> *arr) {
    STATIC_ASSERT(std::numeric_limits<decltype(arr->length())>::max() < CracClassDump::NO_ARRAY_SENTINEL);
    if (arr != nullptr) {
      WRITE(checked_cast<u4>(arr->length()));
      write_uint_array_data(arr->data(), arr->length());
    } else {
      WRITE(CracClassDump::NO_ARRAY_SENTINEL);
    }
  }

  // Note: method idnum cannot be used to identify methods within classes
  // because it depends on method ordering which depends on address of method's
  // name symbol and that is not portable.
  void write_method_identification(const Method &m) {
    assert(!m.is_old(), "old methods require holder's redifinition version to also be written");
    WRITE_CLASS_ID(*m.method_holder());
    WRITE_SYMBOL_ID(m.name());
    WRITE_SYMBOL_ID(m.signature());
    CracClassDump::MethodKind kind;
    if (m.is_static()) {
      assert(!m.is_overpass(), "overpass methods are not static");
      kind = CracClassDump::MethodKind::STATIC;
    } else if (m.is_overpass()) {
      kind = CracClassDump::MethodKind::OVERPASS;
    } else {
      kind = CracClassDump::MethodKind::INSTANCE;
    }
    WRITE(checked_cast<u1>(kind));
  }

  // ###########################################################################
  // Sections
  // ###########################################################################

  // Creates a bit mask with VM options/capabilities that influence data stored
  // in classes.
  static u1 compress_class_related_vm_options() {
    // Warn about options/capabilities that may lead to unrecoverable data loss
    // TODO make HotSpot retain this data if a portable checkpoint was requested
    if (!JvmtiExport::can_get_source_debug_extension()) {
      // ClassFileParser skips SourceDebugExtension class attribute in such case
      log_warning(crac, class, dump, jvmti)("SourceDebugExtension class attribute will not be dumped: JVM TI's 'can_get_source_debug_extension' capability is unsupported");
    }
    if (JvmtiExport::has_redefined_a_class() && !JvmtiExport::can_maintain_original_method_order() && !Arguments::is_dumping_archive()) {
      // ClassFileParser doesn't save the original method order in such case
      log_warning(crac, class, dump, jvmti, cds)("Original method order of classes redefined via JVM TI will not be dumped: neither JVM TI's 'can_maintain_original_method_order' capability is supported, nor a CDS archive is to be created");
    }
    if (!vmClasses::Parameter_klass_loaded()) {
      // ClassFileParser doesn't save MethodParameters method attribute in such case
      // TODO Why can ClassFileParser do this safely? What if j.l.reflect.Parameter gets loaded after a class is loaded?
      ResourceMark rm;
      log_warning(crac, class, dump, jvmti, cds)("MethodParameters method attribute will not be dumped: parameter reflection hasn't been used (%s is not loaded)", vmSymbols::java_lang_reflect_Parameter()->as_klass_external_name());
    }

    // Check development options -- these should have the expected values in
    // product builds, so no warnings
    guarantee(LoadLineNumberTables, "line number tables cannot be dumped");
    guarantee(LoadLocalVariableTables, "local variable tables cannot be dumped");
    guarantee(LoadLocalVariableTypeTables, "local variable type tables cannot be dumped");
    // BinarySwitchThreshold is also used (in bytecode rewriting) but there is no meaningful value to assert the equality

    // Return options/capabilities that may lead to recoverable data loss
    // - Not including InstanceKlass::is_finalization_enabled() even though
    //   it influences JVM_ACC_HAS_FINALIZER flag (internal in Klass) since it's
    //   easily recomputed when parsing methods
    using offsets = CracClassDump::VMOptionShift;
    return
      // If false JVM_ACC_IS_VALUE_BASED_CLASS flags (internal in Klass'es access_flags) isn't set
      static_cast<u1>(DiagnoseSyncOnValueBasedClasses != 0) << offsets::is_sync_on_value_based_classes_diagnosed_shift |
      // If false runtime-invisible annotations are preserved are lost (otherwise they become indistinguishable from the visible ones)
      static_cast<u1>(PreserveAllAnnotations)               << offsets::are_all_annotations_preserved_shift;
  }

  void write_header() {
    constexpr char HEADER_STR[] = "CRAC CLASS DUMP 0.1";
    WRITE_RAW(HEADER_STR, sizeof(HEADER_STR));
    WRITE(checked_cast<u2>(oopSize));
    WRITE(compress_class_related_vm_options()); // Also prints warnings if needed
    log_debug(crac, class, dump)("Wrote class dump header");
  }

  void write_primitive_array_class_ids() {
    precond(Universe::is_fully_initialized());
    for (u1 t = JVM_T_BOOLEAN; t <= JVM_T_LONG; t++) {
      log_trace(crac, class, dump)("Writing primitive array class ID for %s", type2name(static_cast<BasicType>(t)));
      Klass *const tak = Universe::typeArrayKlassObj(static_cast<BasicType>(t));
      WRITE_CLASS_ID(*tak);
      DO_CHECKED(write_obj_array_class_ids(tak));
    }
    {
      log_trace(crac, class, dump)("Writing filler array class ID");
      Klass *const tak = Universe::fillerArrayKlassObj();
      WRITE_CLASS_ID(*tak);
      DO_CHECKED(write_obj_array_class_ids(tak));
    }
    log_debug(crac, class, dump)("Wrote primitive array IDs");
  }

  void do_klass(Klass *k) override {
    if (_io_error_msg != nullptr || !k->is_instance_klass()) return;

    InstanceKlass *const ik = InstanceKlass::cast(k);
    if (ik->is_loaded() && !ik->is_scratch_class()) {
      dump_class_hierarchy(ik);
    }
  }

  void write_end_sentinel() {
    // 1. No class would have this ID, so it marks the end of the series of
    //    class info dumps.
    // 2. The bootstrap loader is the loader with null ID and we don't write it
    //    as an initiating loader, so it marks the end of the series of
    //    initiating loader info dumps.
    WRITE(HeapDump::NULL_ID);
  }

  // ###########################################################################
  // General instance class data
  // ###########################################################################

  static CracClassDump::ClassLoadingKind loading_kind(const InstanceKlass &ik) {
    using Kind = CracClassDump::ClassLoadingKind;
    if (!ik.is_hidden()) {
      return Kind::NORMAL;
    }
    if (ik.is_non_strong_hidden()) {
      return Kind::NON_STRONG_HIDDEN;
    }
    return Kind::STRONG_HIDDEN;
  }

  // Writes access flags defined in the class file format as well as internal
  // Klass and InstanceKlass flags.
  void write_class_flags(const InstanceKlass &ik) {
    // Access flags defined in class file + internal flags defined in Klass
    u4 access_flags = ik.access_flags().as_int();
    // Fix an VM-options-dependent flag if we have CDS
    // TODO make has_value_based_class_annotation also available with CRaC
    if (ik.has_value_based_class_annotation() /* only set when CDS included */) {
      access_flags |= JVM_ACC_IS_VALUE_BASED_CLASS;
    }
    WRITE(access_flags);

    InstanceKlassFlags internal_flags = ik.internal_flags().drop_nonportable_flags(); // Copy to be mutated
    // Internal semi-immutable flags defined in InstanceKlass:
    // - Flags dependent on CDS archive dumping have been cleared by
    //   drop_nonportable_flags() -- they need to be set when restoring based on
    //   the VM options
    postcond(!internal_flags.shared_loading_failed() && internal_flags.is_shared_unregistered_class());
    // Note: should_verify_class flag has a complex dependency on multiple CLI
    // arguments and thus is not exactly portable. But it seems logical to
    // just save/restore its value as is (i.e. the dumping VM decides whether to
    // verify or not), even though it contradicts the general VM options policy
    // to change the behaviour according to the options of the restoring VM.
    WRITE(internal_flags.flags());
    // Internal mutable flags (aka statuses) defined in InstanceKlass -- remove
    // all but has_resolved_methods and has_been_redefined:
    // - is_being_redefined -- we are on safepoint, so this status being true
    //   means that the class either haven't started being redefined yet or has
    //   been redefined already, and since we won't restore the state of the
    //   redefinition code (which is native), we drop the flag
    internal_flags.set_is_being_redefined(false);
    // - is_scratch_class -- we skip these for the same reason as written above
    assert(!internal_flags.is_scratch_class(), "should have skipped it");
    // - is_marked_dependent -- is JIT-compilation-related and we don't dump
    //   such data (at least for now)
    internal_flags.set_is_marked_dependent(false);
    // - is_being_restored -- should not see these on a safepoint
    assert(!internal_flags.is_being_restored(), "should not appear on safepoint");
    WRITE(internal_flags.status());
  }

  void write_nest_host_attr(const InstanceKlass &ik) {
    // Nest host index from the class file (0 iff none).
    // Resolution error (if any) is dumped with the constant pool.
    WRITE(ik.nest_host_index());

    // Have to additionally write the resolved class for hidden classes because
    // it can be a dynamic nest host which may be not the class pointed to by
    // the nest host index
    const InstanceKlass *resolved_nest_host = ik.nest_host_noresolve();
    if (resolved_nest_host != nullptr && ik.is_hidden()) {
      WRITE_CLASS_ID(*resolved_nest_host);
    } else {
      WRITE_OBJECT_ID(nullptr);
    }
  }

  void write_source_debug_extension_attr(const char *source_debug_extension_str) {
    WRITE(static_cast<u1>(source_debug_extension_str != nullptr));
    if (source_debug_extension_str != nullptr) {
      const u4 len = checked_cast<u4>(strlen(source_debug_extension_str));
      WRITE(len);
      WRITE_RAW(source_debug_extension_str, len);
    }
  }

  void write_record_attr(const Array<RecordComponent *> *record_components) {
    WRITE(static_cast<u1>(record_components != nullptr));
    if (record_components == nullptr) {
      return;
    }

    const u2 record_components_num = checked_cast<u2>(record_components->length());
    WRITE(record_components_num); // u2 components_count
    for (int comp_i = 0; comp_i < record_components_num; comp_i++) {
      const RecordComponent &component = *record_components->at(comp_i);
      WRITE(component.name_index());
      WRITE(component.descriptor_index());
      WRITE(component.attributes_count());
      WRITE(component.generic_signature_index());                 // Signature, 0 iff unspecified
      DO_CHECKED(write_uint_array(component.annotations()));      // Runtime(In)VisibleAnnotations
      DO_CHECKED(write_uint_array(component.type_annotations())); // Runtime(In)VisibleTypeAnnotations
    }
  }

  void write_class_attrs(const InstanceKlass &ik) {
    WRITE(ik.source_file_name_index());                        // SourceFile (0 iff none)
    WRITE(ik.generic_signature_index());                       // Signature (0 iff none)
    DO_CHECKED(write_nest_host_attr(ik));
    DO_CHECKED(write_uint_array(ik.nest_members() != Universe::the_empty_short_array() ? ik.nest_members() : nullptr));   // NestMembers (sentinel iff none)
    DO_CHECKED(write_uint_array(ik.inner_classes() != Universe::the_empty_short_array() ? ik.inner_classes() : nullptr)); // InnerClasses, possibly concatenated with EnclosingMethod (sentinel iff none)
    DO_CHECKED(write_source_debug_extension_attr(ik.source_debug_extension()));
    DO_CHECKED(write_uint_array(ik.constants()->operands()));  // BootstrapMethods (null if none)
    DO_CHECKED(write_record_attr(ik.record_components()));
    DO_CHECKED(write_uint_array(ik.permitted_subclasses() != Universe::the_empty_short_array() ? ik.permitted_subclasses() : nullptr)); // PermittedSubclasses
    DO_CHECKED(write_uint_array(ik.class_annotations()));      // Runtime(In)VisibleAnnotations (null if none)
    DO_CHECKED(write_uint_array(ik.class_type_annotations())); // Runtime(In)VisibleTypeAnnotations (null if none)
    // Synthetic attribute is stored in access flags, others are not available
  }

  void write_resolution_error_symbols(const ResolutionErrorEntry &entry) {
    WRITE_SYMBOL_ID(entry.error());       // not null unless a special nest host error case
    WRITE_SYMBOL_ID(entry.message());     // null if no message
    WRITE_SYMBOL_ID(entry.cause());       // null if no cause
    if (entry.cause() != nullptr) {
      WRITE_SYMBOL_ID(entry.cause_msg()); // null if no cause message
    } else {
      assert(entry.cause_msg() == nullptr, "must be null if there is no cause");
    }
  }

  // For non-nest-host resolution errors.
  void write_resolution_error(const ConstantPool &cp, int err_table_index) {
    constantPoolHandle cph(Thread::current(), const_cast<ConstantPool *>(&cp));

    // Not using SystemDictionary::find_resolution_error() to get around the mutex used there (we're on safepoint)
    const ResolutionErrorEntry *entry = ResolutionErrorTable::find_entry(cph, err_table_index);
    assert(entry != nullptr, "no resolution error recorded for %i", err_table_index);
    assert(entry->error() != nullptr , "recorded resolution error cannot be null for a non-nest-host error");
    assert(entry->nest_host_error() == nullptr, "not for nest host errors");

    write_resolution_error_symbols(*entry);
  }

  // For nest host resolution errors.
  void write_nest_host_resolution_error_if_exists(const ConstantPool &cp) {
    const u2 nest_host_i = cp.pool_holder()->nest_host_index();
    constantPoolHandle cph(Thread::current(), const_cast<ConstantPool *>(&cp));

    const ResolutionErrorEntry *entry = ResolutionErrorTable::find_entry(cph, nest_host_i);
    WRITE(static_cast<u1>(entry != nullptr));
    if (entry == nullptr) {
      return;
    }

    write_resolution_error_symbols(*entry);

    assert(entry->nest_host_error() != nullptr, "nest host error always has this");
    const auto nest_host_err_len = checked_cast<u4>(strlen(entry->nest_host_error()));
    WRITE(nest_host_err_len);
    WRITE_RAW(entry->nest_host_error(), nest_host_err_len);
  }

  // Writes constant pool contents, including resolved classes and resolution
  // errors and excluding constant pool cache and indy resolution errors.
  void write_constant_pool(const ConstantPool &cp) {
    auto &cp_ = const_cast<ConstantPool &>(cp); // FIXME a ton of ConstantPool's methods that could've been const are not marked as such
    WRITE(checked_cast<u2>(cp.length()));
    WRITE(checked_cast<u2>(cp.resolved_klasses()->length())); // To avoid multiple passes during parsing
    for (int pool_i = 1 /* index 0 is unused */; pool_i < cp.length(); pool_i++) {
      const u1 tag = cp.tag_at(pool_i).value();
      WRITE(tag);
      switch (tag) {
        // Fundamental structures
        case JVM_CONSTANT_Utf8:
          WRITE_SYMBOL_ID(cp.symbol_at(pool_i));
          break;
        case JVM_CONSTANT_NameAndType:
          WRITE(cp_.name_ref_index_at(pool_i));
          WRITE(cp_.signature_ref_index_at(pool_i));
          break;

        // Static constants
        case JVM_CONSTANT_Integer: WRITE(cp_.int_at(pool_i));      break;
        case JVM_CONSTANT_Float:   WRITE(cp_.float_at(pool_i));    break;
        case JVM_CONSTANT_Long:    WRITE(cp_.long_at(pool_i++));   break; // Next index is unused, so skip it
        case JVM_CONSTANT_Double:  WRITE(cp_.double_at(pool_i++)); break; // Next index is unused, so skip it
        case JVM_CONSTANT_String:
          // String entries are also kind of resolved, even though they are not
          // considered symbolic references (JVMS §5.1):
          // a) until a string is queried the first time, only a Symbol* is stored
          WRITE_SYMBOL_ID(cp_.unresolved_string_at(pool_i)); // always not null
          // b) when the string is queried, a j.l.String object is created for it,
          //    and all the later queries should return this same object -- a
          //    reference to this object is stored in the reolved references array
          //    of the cache (it is null until resolved)
          break;

        // Symbolic references
        case JVM_CONSTANT_Class:
        case JVM_CONSTANT_UnresolvedClass:
        case JVM_CONSTANT_UnresolvedClassInError:
          // Static data
          WRITE(checked_cast<u2>(cp.klass_name_index_at(pool_i)));
          // Resolution state info
          if (tag == JVM_CONSTANT_Class) {
            // Not ConstantPool::resolved_klass_at() to get around a redundant aquire (no concurrency on safepoint)
            Klass *resolved_class = cp.resolved_klasses()->at(cp.klass_slot_at(pool_i).resolved_klass_index());
            assert(resolved_class != nullptr, "Unresolved class in JVM_CONSTANT_Class slot");
            WRITE_CLASS_ID(*resolved_class);
            // NestHost resolution error may happen even if the referenced class itself was successfully resolved
            if (pool_i == cp.pool_holder()->nest_host_index()) {
              DO_CHECKED(write_nest_host_resolution_error_if_exists(cp));
            }
          } else if (tag == JVM_CONSTANT_UnresolvedClassInError) {
            DO_CHECKED(write_resolution_error(cp, pool_i));
          }
          break;
        case JVM_CONSTANT_Fieldref:
        case JVM_CONSTANT_Methodref:
        case JVM_CONSTANT_InterfaceMethodref:
          // Static data
          WRITE(cp_.uncached_klass_ref_index_at(pool_i));
          WRITE(cp_.uncached_name_and_type_ref_index_at(pool_i));
          // Field/method resolution ussually consists of:
          // 1. Holder class resolution --- we record results of these during
          //    JVM_CONSTANT_(Unresolved)Class(InError) dump, so Klass* is
          //    obtainable from the class reference dumped above.
          // 2. Field/method lookup + access control --- this should produce the
          //    same result given the same (Klass*, mathod/field name, signature)
          //    combination, and the last two parts are obtainable via the
          //    NameAndType reference dumped above.
          // In such cases we don't need to record any resolution data.
          //
          // But there is also a special case of signature-polymorphic
          // invokevirtual calls the resolution process of which is more like it
          // of InvokeDynamic resulting into an adapter method (stored in the
          // cache itself) and "appendix" object (stored in resolved references
          // array of the cache) being resolved.
          break;
        case JVM_CONSTANT_MethodType:
        case JVM_CONSTANT_MethodTypeInError:
          // Static data
          WRITE(checked_cast<u2>(cp_.method_type_index_at(pool_i)));
          // Resolution state info
          if (tag == JVM_CONSTANT_MethodTypeInError) {
            DO_CHECKED(write_resolution_error(cp, pool_i));
          } else {
            // MethodType object is stored in the resolved references array of the
            // cache (null if unresolved)
          }
          break;
        case JVM_CONSTANT_MethodHandle:
        case JVM_CONSTANT_MethodHandleInError:
          // Static data
          WRITE(checked_cast<u1>(cp_.method_handle_ref_kind_at(pool_i)));
          WRITE(checked_cast<u2>(cp_.method_handle_index_at(pool_i)));
          // Resolution state info
          if (tag == JVM_CONSTANT_MethodHandleInError) {
            DO_CHECKED(write_resolution_error(cp, pool_i));
          } else {
            // MethodHandle object is stored in the resolved references array of
            // the cache (null if unresolved)
          }
          break;
        case JVM_CONSTANT_Dynamic:
        case JVM_CONSTANT_DynamicInError:
        case JVM_CONSTANT_InvokeDynamic:
          // Static data
          WRITE(cp_.bootstrap_methods_attribute_index(pool_i));
          WRITE(cp_.bootstrap_name_and_type_ref_index_at(pool_i));
          // Resolution state info
          if (tag == JVM_CONSTANT_DynamicInError) {
            DO_CHECKED(write_resolution_error(cp, pool_i));
          } else {
            // Dynamic: computed constants are stored in the resolved references
            // array of the cache (null if unresolved, primitives are boxed).
            //
            // InvokeDynamic:
            // 1. One InvokeDynamic constant pool entry can correspond to multiple
            //    cache entries: one for each indy instruction in the class --
            //    their info is stored in a special constant pool cache array,
            //    appendices are stored in the resolved references array of the
            //    cache.
            // 2. Error messages, if any, are written as part of cache dump.
          }
          break;

        default: // Module tags and internal pool-construction-time tags
          ShouldNotReachHere();
      }
    }

    // - Not writing flags since they are either trivial to obtain from the data
    //   written above (has_dynamic_constant), are handeled while working with
    //   holder's methods (has_preresolution, on_stack) or are CDS-related and
    //   thus JVM-instance-dependent (on_stack, is_shared)
    // - Generic signature index, source file name index and operands are written
    //   as the corresponding class attributes
  }

  // Writes data from the constant pool cache.check won't crash with NPE when there is no
  //  fields/indys.
  void write_constant_pool_cache(const ConstantPoolCache &cp_cache) {
    // TODO simplify lengths calculations below by making
    // resolved_*_entries_length() return 0 when resolved_*_entries == nullptr
    // Write lengths of the main arrays first to be able to outright allocate
    // the cache when parsing:
    // 1. Field entries:  u2 -- same amount as of Fieldrefs.
    const bool has_fields = const_cast<ConstantPoolCache &>(cp_cache).resolved_field_entries() != nullptr;
    assert(!has_fields || cp_cache.resolved_field_entries_length() > 0, "allocated resolved fields array is always non-empty");
    const u2 field_entries_len = has_fields ? cp_cache.resolved_field_entries_length() : 0;
    WRITE(field_entries_len);
    // 2. Method entries: jint -- can be twice as much as methods (one per
    //    Methodref, one or two per InterfaceMethodref) for which we need u2.
    WRITE(checked_cast<jint>(cp_cache.length()));
    // 3. Indy entries: jint -- one per indy, and there may be a lot of those in
    //    the class, but the array is int-indexed.
    const bool has_indys = const_cast<ConstantPoolCache &>(cp_cache).resolved_indy_entries() != nullptr;
    assert(!has_indys || cp_cache.resolved_indy_entries_length() > 0, "allocated resolved indys array is always non-empty");
    const jint indy_entries_len = has_indys ? cp_cache.resolved_indy_entries_length() : 0;
    WRITE(indy_entries_len);
    // 4. Resolved references: doesn't influence cache allocation, so don't need
    //    its length here.

    for (int field_i = 0; field_i < field_entries_len; field_i++) {
      const ResolvedFieldEntry &field_info = *cp_cache.resolved_field_entry_at(field_i);

      assert(field_info.constant_pool_index() > 0, "uninitialized field entry");
      WRITE(field_info.constant_pool_index());

      const u1 get_code = field_info.get_code();
      const u1 put_code = field_info.put_code();
      assert(!(get_code == 0 && put_code != 0), "if resolved for put, must be resolved for get as well");
      WRITE(get_code);
      WRITE(put_code);

      if (get_code != 0) { // If resolved, write the data
        assert(field_info.field_holder() != nullptr, "must be resolved");
        // field_offset is omitted since it depends on VM options and the platform
        WRITE_CLASS_ID(*field_info.field_holder());
        WRITE(field_info.field_index());
        WRITE(field_info.tos_state());
        WRITE(field_info.flags());
      }
    }

    // Resolved methods are still stored as generic cache entries, but these
    // aren't used for anything else anymore (fields and indys got moved to
    // separate arrays). The upcoming cache changes will simplify this code.
    for (int cache_i = 0; cache_i < cp_cache.length(); cache_i++) {
      const ConstantPoolCacheEntry &info = *cp_cache.entry_at(cache_i);

      assert(info.constant_pool_index() > 0, "uninitialized cache entry");
      WRITE(checked_cast<u2>(info.constant_pool_index()));

      const Bytecodes::Code bytecode1 = info.bytecode_1();
      const Bytecodes::Code bytecode2 = info.bytecode_2();
      WRITE(checked_cast<u1>(bytecode1));
      WRITE(checked_cast<u1>(bytecode2));

      // This will be simplified when ResolvedMethodEntry replaces ConstantPoolCacheEntry
      if (bytecode1 > 0 || bytecode2 > 0) { // If resolved, write the data
        assert(info.is_method_entry(), "not used for field entries anymore");
        assert(bytecode1 != Bytecodes::_invokedynamic, "not used for indys anymore");
        // Flags go first since they, together with the bytecodes, define the contents of f1 and f2
        using shifts = CracClassDump::ResolvedMethodEntryFlagShift;
        WRITE(checked_cast<u1>(info.has_local_signature() << shifts::has_local_signature_shift |
                               info.has_appendix()        << shifts::has_appendix_shift        |
                               info.is_forced_virtual()   << shifts::is_forced_virtual_shift   |
                               info.is_final()            << shifts::is_final_shift            |
                               info.is_vfinal()           << shifts::is_vfinal_shift));
        WRITE(checked_cast<u1>(info.flag_state())); // ToS state
        WRITE(checked_cast<u1>(info.parameter_size()));
        // f1
        switch (bytecode1) {
          case Bytecodes::_invokestatic:
          case Bytecodes::_invokespecial:
          case Bytecodes::_invokehandle: {
            const Method *method = info.f1_as_method(); // Resolved method for non-virtual calls or adapter method for invokehandle
            assert(method != nullptr, "must be resolved");
            assert(!method->is_old(), "cache never contains old methods"); // Lets us omit holder's redefinition version
            write_method_identification(*method);
            break;
          }
          case Bytecodes::_invokeinterface:
            if (!info.is_forced_virtual()) {
              const Klass *klass = info.f1_as_klass(); // Resolved interface class
              assert(klass != nullptr, "must be resolved");
              WRITE_CLASS_ID(*klass);
              break;
            }
            // Fallthrough
          case 0: // bytecode1 is not set
            assert(info.f1_ord() == 0, "f1 must be unused");
            break;
          default:
            ShouldNotReachHere();
        }
        // f2
        if (info.is_vfinal() || (bytecode1 == Bytecodes::_invokeinterface && !info.is_forced_virtual())) {
          assert(bytecode1 == Bytecodes::_invokeinterface || bytecode2 == Bytecodes::_invokevirtual, "must be");
          assert(bytecode1 != Bytecodes::_invokestatic && bytecode1 != Bytecodes::_invokehandle, "these cannot share an entry with invokevirtual");
          const Method *method = info.is_vfinal() ? info.f2_as_vfinal_method() : info.f2_as_interface_method(); // Resolved final or interface method
          assert(method != nullptr, "must be resolved");
          assert(!method->is_old(), "cache never contains old methods"); // Lets us omit holder's redefinition version
          write_method_identification(*method);
        } else if (bytecode1 == Bytecodes::_invokeinterface || bytecode2 == Bytecodes::_invokevirtual ||
                  (bytecode1 == Bytecodes::_invokehandle && info.has_appendix())) {
          assert(bytecode1 != Bytecodes::_invokestatic, "invokestatic cannot share an entry with invokevirtual");
          WRITE(checked_cast<jint>(info.f2_as_index())); // vtable/itable index for virtual/interface calls or appendix index (if any) for invokehandle
        } else {
          // f2 is unused
        }
      }
    }

    for (int indy_i = 0; indy_i < indy_entries_len; indy_i++) {
      const ResolvedIndyEntry &indy_info = *cp_cache.resolved_indy_entry_at(indy_i);

      assert(indy_info.constant_pool_index() > 0, "uninitialized indy entry");
      WRITE(indy_info.constant_pool_index());
      WRITE(indy_info.resolved_references_index()); // Why is this u2? Should be int to index the whole resolved references for all indys which are int-indexed themselves.
      WRITE(checked_cast<u1>(indy_info.flags() | indy_info.is_resolved() << ResolvedIndyEntry::num_flags));
      assert((indy_info.is_resolved() && !indy_info.resolution_failed()) ||
             (!indy_info.is_resolved() && !indy_info.has_appendix()), "illegal state");

      if (indy_info.is_resolved()) {
        assert(!indy_info.resolution_failed(), "cannot be failed if succeeded");
        const Method *adapter = indy_info.method();
        assert(adapter != nullptr, "must be resolved");
        assert(!adapter->is_old(), "cache never contains old methods"); // Lets us omit holder's redefinition version
        write_method_identification(*adapter);
        WRITE(indy_info.num_parameters());
        WRITE(indy_info.return_type());
      } else if (indy_info.resolution_failed()) {
        const int indy_res_err_i = ResolutionErrorTable::encode_cpcache_index(ConstantPool::encode_invokedynamic_index(indy_i));
        DO_CHECKED(write_resolution_error(*cp_cache.constant_pool(), indy_res_err_i));
      }
    }

    // Resolved references:
    // - String objects created when a String constant pool entry is queried the
    //   first time and interned
    // - MethodHandle objects for resolved MethodHandle constant pool entries
    // - MethodType objects for resolved MethodType constant pool entries
    // - Appendix objects created for each invokedynamic/invokehandle bytecode,
    //   as well as for Dynamic constant pool entries
    //
    // The array itself is dumped as part of HPROF, so only write the mapping
    // from indices of the first part of resolved references (i.e. excluding
    // appendices) to constant pool indices:
    DO_CHECKED(write_uint_array(cp_cache.reference_map())); // u2 is enough for length (not larger than the constant pool), but using u4 for the null array sentinel
  }

  void write_interfaces(const Array<InstanceKlass *> &interfaces) {
    WRITE(checked_cast<u2>(interfaces.length()));
    for (int index = 0; index < interfaces.length(); index++) {
      WRITE_CLASS_ID(*interfaces.at(index));
    }
  }

  // ###########################################################################
  // Fields
  // ###########################################################################

  void write_fields(const InstanceKlass &ik) {
    // Cannot write the field info stream as is, even though it is in a portable
    // UNSIGNED5 encoding, because it contains field offsets which aren't
    // portable (depend on the platform and the specified VM options) and
    // UNSIGNED5 doesn't allow content updates, so we cannot change the offsets
    // without re-encoding the whole stream
    // TODO measure if re-encoding will actually be slower
    ResourceMark rm;
    int java_fields_count;
    int injected_fields_count;
    const GrowableArray<FieldInfo> &infos = *FieldInfoStream::create_FieldInfoArray(ik.fieldinfo_stream(), &java_fields_count, &injected_fields_count);

    // Field statuses (mutable VM-internal field data)
    const Array<FieldStatus> &statuses = *ik.fields_status();

    const Array<AnnotationArray *> *annotations = ik.fields_annotations();
    const Array<AnnotationArray *> *type_annotations = ik.fields_type_annotations();

    assert(java_fields_count + injected_fields_count == ik.total_fields_count() &&
           infos.length() == ik.total_fields_count() &&
           statuses.length() == ik.total_fields_count() &&
           (annotations == nullptr || annotations->length() == java_fields_count) &&
           (type_annotations == nullptr || type_annotations->length() == java_fields_count), "must be");
    WRITE(checked_cast<u2>(java_fields_count));
    WRITE(checked_cast<u2>(injected_fields_count));
    for (int i = 0; i < infos.length(); i++) {
      const FieldInfo &field_info = infos.at(i);
      WRITE(field_info.name_index());
      WRITE(field_info.signature_index());
      assert((field_info.access_flags().as_int() & JVM_RECOGNIZED_FIELD_MODIFIERS) == field_info.access_flags().as_int(), "illegal field flags");
      WRITE(field_info.access_flags().as_short()); // Includes Synthetic attribute
      WRITE(checked_cast<u1>(field_info.field_flags().as_uint()));
      WRITE(field_info.initializer_index());       // ConstantValue attribute
      WRITE(field_info.generic_signature_index()); // Signature attribute
      WRITE(field_info.contention_group());

      WRITE(statuses.at(i).as_uint()); // Includes JVM TI's access/modification watch flags

      assert(field_info.field_flags().is_injected() == (i >= java_fields_count), "injected fields go last");
      if (i < java_fields_count) {
        // Runtime(In)Visible(Type)Annotations attributes: only non-injected fields have them
        DO_CHECKED(write_uint_array(annotations != nullptr      ? annotations->at(i)      : nullptr));
        DO_CHECKED(write_uint_array(type_annotations != nullptr ? type_annotations->at(i) : nullptr));
      }
    }

    // Static fields' values aren't written since they are part of the heap dump
  }

  // ###########################################################################
  // Methods
  // ###########################################################################

  // Removes non-portable elements from method statuses (aka mutable internal
  // flags).
  static int filter_method_statuses(MethodFlags statuses /* copy to mutate */, bool has_annotations) {
    // Clear the JIT-related bits
    statuses.set_queued_for_compilation(false);
    statuses.set_is_not_c1_compilable(false);
    statuses.set_is_not_c2_compilable(false);
    statuses.set_is_not_c2_osr_compilable(false);

    if (JVMCI_ONLY(UseJVMCICompiler) NOT_JVMCI(false) && statuses.dont_inline()) {
      // dont_inline status may be set not only via the corresponding
      // annotation, but also by JVMCI -- in the latter case it becomes
      // compiler-dependent and should not be dumped (at least until we also
      // support JIT dumping)
      if (has_annotations) {
        // TODO need to parse RuntimeVisibleAnnotations to see if DontInline is
        //  there and also check if the holder class can access VM annotations.
        //  If PreserveAllAnnotations is set need to do something not to look
        //  into RuntimeInvisibleAnnotations.
        ResourceMark rm;
        log_warning(crac, class, dump, jvmci)("Method marked 'don't inline' which is ambiguos when JVM CI JIT is used");
      } else {
        // No RuntimeVisibleAnnotations, so it must have been set by JVMCI
        statuses.set_dont_inline(false);
      }
    }

    return statuses.as_int();
  }

  static jint get_linenumber_table_size(const ConstMethod &cmethod) {
    precond(cmethod.has_linenumber_table());
    CompressedLineNumberReadStream stream(cmethod.compressed_linenumber_table());
    while (stream.read_pair()) {}
    const jint size = stream.position();
    assert(size > 0, "existing line number table cannot be empty");
    return size;
  }

  // Writes method's bytecodes. Internal bytecodes the usage of which is
  // (mostly) platform-independent are preserved (Zero interpreter will still
  // need some rewriting -- see the related comments below).
  void write_bytecodes(const Method &method) {
    if (!method.method_holder()->is_rewritten()) {
      // Can just write the whole code buffer as is
      assert(method.number_of_breakpoints() == 0, "class must be linked (and thus rewritten) for breakpoints to exist");
      WRITE_RAW(method.code_base(), method.code_size());
      return;
    }
    // Else, have to partially revert the rewriting to make the code portable:
    // - Bytecode rewriting done for interpreter optimization (both by the
    //   interpreters themselves and the rewriter) depends on the interpreter
    //   (e.g. Zero currently doesn't support some fast* internal bytecodes),
    //   so have to revert these
    // - Constant pool indices of get/put, ldc and invoke instructions are
    //   rewritten into native byte order by the rewriter, so need to rewrite
    //   them back if native endianness differs from Java's
    // TODO decide what to do with breakpoints (for now they are cleared)

    // BytecodeStream reverts internal bytecodes and breakpoints for us
    BytecodeStream stream(methodHandle(Thread::current(), const_cast<Method *>(&method)));
    for (Bytecodes::Code code = stream.next(); code >= 0; code = stream.next()) {
      const Bytecodes::Code raw_code = stream.raw_code(); // Possibly internal code but not a breakpoint
      precond(raw_code != Bytecodes::_breakpoint);

      if (Bytecodes::is_field_code(code) || Bytecodes::is_invoke(code)) {
        // If this is actualy an invokehandle write it since it is portable
        WRITE(checked_cast<u1>(raw_code != Bytecodes::_invokehandle ? code : Bytecodes::_invokehandle));
        // Convert index byte order: u4 for invokedynamic, u2 for others
        if (code == Bytecodes::_invokedynamic) {
          WRITE(Bytes::get_native_u4(stream.bcp() + 1));
        } else {
          WRITE(Bytes::get_native_u2(stream.bcp() + 1));
          // invokeinterface has two additional bytes untoched by the rewriter
          if (code == Bytecodes::_invokeinterface) {
            WRITE_RAW(stream.bcp() + 3, 2);
          }
        }
        continue;
      }
      if (raw_code == Bytecodes::_fast_aldc || raw_code == Bytecodes::_fast_aldc_w) {
        // These rewritten versions of ldc and ldc_w are portable, so write them directly
        WRITE(checked_cast<u1>(raw_code));
        if (raw_code == Bytecodes::_fast_aldc) {
          WRITE(checked_cast<u1>(*(stream.bcp() + 1)));
        } else {
          // Also, the index needs to be converted from the native byte order
          WRITE(Bytes::get_native_u2(stream.bcp() + 1));
        }
        continue;
      }
      postcond(!Bytecodes::native_byte_order(code));

      if (code == Bytecodes::_lookupswitch) {
        // Template interpreters expect this to always be rewritten. Zero, on
        // the other hand, currently doesn't support the fast versions. So we do
        // the rewriter's job to keep this uniform across all interpreters. The
        // template interpreters' way is chosen to restore faster for them.
#ifdef ZERO
        assert(raw_code == code, "Zero doesn't support the rewriting");
        Bytecode_lookupswitch switch_inspector(const_cast<Method *>(&method), stream.bcp());
        // The threshold is fixed in product builds, so this should be portable
        const Bytecodes::Code rewritten = switch_inspector.number_of_pairs() < BinarySwitchThreshold ? Bytecodes::_fast_linearswitch
                                                                                                     : Bytecodes::_fast_binaryswitch;
#else // ZERO
        assert(raw_code != code, "must be already rewritten");
        const Bytecodes::Code rewritten = raw_code;
#endif // ZERO
        WRITE(checked_cast<u1>(rewritten));
      } else if (raw_code == Bytecodes::_return_register_finalizer) {
        // This special case of return is portable, so write it as is
        WRITE(checked_cast<u1>(raw_code));
      } else {
        // Otherwise, write the code as converted and its parameters as raw
        WRITE(checked_cast<u1>(stream.is_wide() ? Bytecodes::_wide : code));
      }
      WRITE_RAW(stream.bcp() + 1, stream.instruction_size() - 1); // Parameters
    }

    DEBUG_ONLY(ResourceMark rm;)
    assert(stream.is_last_bytecode(), "error reading bytecodes of %s at index %i", method.external_name(), stream.bci());
  }

  void write_code_attr(const Method &method, u4 linenumber_table_size /* costly to recalculate */) {
    const ConstMethod &cmethod = *method.constMethod();
    precond(cmethod.code_size() > 0); // Code size is dumped with the rest of the embedded method data sizes

    WRITE(cmethod.max_stack());
    WRITE(cmethod.max_locals());

    DO_CHECKED(write_bytecodes(method)); // Bytecodes with some of the internal ones preserved

    if (cmethod.has_exception_table()) {
      // Length is dumped with the rest of the embedded method data sizes
      precond(method.exception_table_length() > 0);
      STATIC_ASSERT(sizeof(ExceptionTableElement) == 4 * sizeof(u2)); // Check no padding
      const size_t len = method.exception_table_length() * sizeof(ExceptionTableElement) / sizeof(u2);
      write_uint_array_data(reinterpret_cast<u2 *>(method.exception_table_start()), len);
    }

    if (cmethod.has_linenumber_table()) {
      // Table size is dumped with the rest of the embedded method data sizes
      precond(linenumber_table_size > 0);
      // Linenumber table is stored in a portable compressed format (a series of
      // single-byte elements and UNSIGNED5-encoded ints from 0 to 65535), so can
      // be dumped as is
      WRITE_RAW(cmethod.compressed_linenumber_table(), linenumber_table_size);
    }
    if (cmethod.has_localvariable_table()) { // LocalVariableTable and LocalVariableTypeTable
      precond(cmethod.localvariable_table_length() > 0);
      // Length is dumped with the rest of the embedded method data sizes
      STATIC_ASSERT(sizeof(LocalVariableTableElement) == 6 * sizeof(u2)); // Check no padding
      const size_t len = cmethod.localvariable_table_length() * sizeof(LocalVariableTableElement) / sizeof(u2);
      write_uint_array_data(reinterpret_cast<u2 *>(cmethod.localvariable_table_start()), len);
    }
    { // StackMapTable
      assert(cmethod.stackmap_data() == nullptr || !cmethod.stackmap_data()->is_empty(), "must be non-empty if exists");
      DO_CHECKED(write_uint_array(cmethod.stackmap_data())); // Null if not specified
    }
    // Other code attributes are not available
  }

  void write_method(const Method &method) {
    const ConstMethod &cmethod = *method.constMethod();

    if (cmethod.method_idnum() != cmethod.orig_method_idnum()) {
      // TODO method ID is not dumped since it is not portable (depends on
      // method ordering which depends on method's name symbol addresses), but
      // what to do with the original ID? It is also non-portable but it should
      // probably be restored somehow...
      precond(method.is_obsolete()); // Implies is_old
      log_error(crac, class, dump)("Dumping old versions of redefined classes is not supported yet");
      Unimplemented();
    }

    // Access flags defined in class file, fits in u2 according to JVMS
    assert(method.access_flags().as_int() == (method.access_flags().get_flags() & JVM_RECOGNIZED_METHOD_MODIFIERS), "only method-related flags should be present");
    WRITE(checked_cast<u2>(method.access_flags().get_flags()));
    // Immutable internal flags
    WRITE(checked_cast<jint>(cmethod.flags()));
    // Mutable internal flags (statuses)
    WRITE(checked_cast<jint>(filter_method_statuses(method.statuses(), cmethod.has_method_annotations())));

    WRITE(cmethod.name_index());
    WRITE(cmethod.signature_index());

    // Write lengths/sizes of all embedded data first to allow the method to be
    // allocated (allocating memory for the data) before reading the data
    WRITE(cmethod.code_size()); // u2 is enough (code_length is limited to 65535 even though occupies u4)
    assert(cmethod.code_size() > 0 || // code_size == 0 iff no Code was specified
          (!cmethod.has_exception_table() && !cmethod.has_linenumber_table() && !cmethod.has_localvariable_table()),
          "being parts of Code attribute they cannot exist without it");
    const jint linenumber_table_size = cmethod.has_linenumber_table() ? get_linenumber_table_size(cmethod) : 0;
    if (cmethod.has_exception_table())       WRITE(cmethod.exception_table_length());
    if (cmethod.has_linenumber_table())      WRITE(linenumber_table_size);
    if (cmethod.has_localvariable_table())   WRITE(cmethod.localvariable_table_length());
    if (cmethod.has_checked_exceptions())    WRITE(cmethod.checked_exceptions_length());
    if (cmethod.has_method_parameters())     WRITE(checked_cast<u1>(cmethod.method_parameters_length())); // u1 is enough as specified in the class file format
    if (cmethod.has_generic_signature())     WRITE(cmethod.generic_signature_index()); // Signature attribute, participates in the method allocation size calculation
    if (cmethod.has_method_annotations())    WRITE(checked_cast<jint>(cmethod.method_annotations_length()));
    if (cmethod.has_parameter_annotations()) WRITE(checked_cast<jint>(cmethod.parameter_annotations_length()));
    if (cmethod.has_type_annotations())      WRITE(checked_cast<jint>(cmethod.type_annotations_length()));
    if (cmethod.has_default_annotations())   WRITE(checked_cast<jint>(cmethod.default_annotations_length()));

    // Now write the data (i.e. method attributes), omitting their lengths/sizes
    if (cmethod.code_size() > 0)             DO_CHECKED(write_code_attr(method, linenumber_table_size));
    if (cmethod.has_checked_exceptions()) {
      assert(cmethod.checked_exceptions_length() > 0, "existing stackmap table cannot be empty");
      STATIC_ASSERT(sizeof(CheckedExceptionElement) == sizeof(u2)); // Check no padding
      const size_t len = cmethod.checked_exceptions_length() * sizeof(CheckedExceptionElement) / sizeof(u2);
      DO_CHECKED(write_uint_array_data(reinterpret_cast<u2 *>(cmethod.checked_exceptions_start()), len));
    }
    if (cmethod.has_method_parameters()) { // Does not imply method_parameters_length > 0
      STATIC_ASSERT(sizeof(MethodParametersElement) == 2 * sizeof(u2)); // Check no padding
      const size_t len = cmethod.method_parameters_length() * sizeof(MethodParametersElement) / sizeof(u2);
      DO_CHECKED(write_uint_array_data(reinterpret_cast<u2 *>(cmethod.method_parameters_start()), len));
    }
    if (cmethod.has_method_annotations()) { // Runtime(In)VisibleAnnotations
      assert(!cmethod.method_annotations()->is_empty(), "existing method annotations cannot be empty");
      DO_CHECKED(write_uint_array_data(cmethod.method_annotations()->data(), cmethod.method_annotations_length()));
    }
    if (cmethod.has_parameter_annotations()) { // Runtime(In)VisibleParameterAnnotations
      assert(!cmethod.method_annotations()->is_empty(), "existing method annotations cannot be empty");
      DO_CHECKED(write_uint_array_data(cmethod.parameter_annotations()->data(), cmethod.method_annotations_length()));
    }
    if (cmethod.has_type_annotations()) { // Runtime(In)VisibleTypeAnnotations
      assert(!cmethod.method_annotations()->is_empty(), "existing method annotations cannot be empty");
      DO_CHECKED(write_uint_array_data(cmethod.type_annotations()->data(), cmethod.type_annotations_length()));
    }
    if (cmethod.has_default_annotations()) { // AnnotationDefault
      assert(!cmethod.method_annotations()->is_empty(), "existing method annotations cannot be empty");
      DO_CHECKED(write_uint_array_data(cmethod.default_annotations()->data(), cmethod.default_annotations_length()));
    }
    // Synthetic attribute is stored in access flags, others are not available

    // TODO examine if any other intrinsics should be dumped
    WRITE(checked_cast<u1>(method.is_compiled_lambda_form())); // ClassFileParser sets this intrinsic based on an annotation
  }

  void write_methods(const InstanceKlass &ik) {
    // Normal methods, including overpasses
    const Array<Method *> &methods = *ik.methods();
    const Array<int> &original_ordering = *ik.method_ordering();
    assert(&original_ordering == Universe::the_empty_int_array() || methods.length() == original_ordering.length(), "must be");
    WRITE(checked_cast<u2>(methods.length()));
    for (int i = 0; i < methods.length(); i++) {
      // Original index of this method in class file
      if (&original_ordering != Universe::the_empty_int_array()) {
        WRITE(checked_cast<u2>(original_ordering.at(i)));
      } else {
        assert(!JvmtiExport::can_maintain_original_method_order() && !Arguments::is_dumping_archive(), "original method ordering must be available");
        WRITE(checked_cast<u2>(i)); // Pretend this is the original ordering
      }
      DO_CHECKED(write_method(*methods.at(i)));
    }

    // Descriptions of the default methods, if any
    const Array<Method *> *defaults = ik.default_methods();
    if (defaults != nullptr) {
      assert(ik.has_nonstatic_concrete_methods(), "must be");
      assert(defaults->length() > 0, "must not be allocated if there are no defaults");
      WRITE(checked_cast<u2>(defaults->length()));
      for (int i = 0; i < defaults->length(); i++) {
        const Method &method = *defaults->at(i);
        assert(!method.is_old(), "default methods must not be old"); // Lets us omit holder's redefinition version
        write_method_identification(method);
      }
    } else {
      WRITE(checked_cast<u2>(0));
    }

    // TODO If the class has been linked, write its vtable/itable and the
    //  corresponding method indices. These indices are actually already saved
    //  as part of resolved method entries of constant pool cache.
  }

  // ###########################################################################
  // JVM TI-related data
  // ###########################################################################

  // JVM TI RetransformClasses support.
  void write_cached_class_file(JvmtiCachedClassFileData *cached_class_file) {
    if (cached_class_file == nullptr) {
      WRITE(CracClassDump::NO_CACHED_CLASS_FILE_SENTINEL);
      return;
    }

    guarantee(cached_class_file->length >= 0, "length cannot be negative");
    WRITE(cached_class_file->length);
    WRITE_RAW(cached_class_file->data, cached_class_file->length);
  }

  // JVM TI RedefineClasses support.
  void write_previous_versions(InstanceKlass *ik) {
    if (!ik->has_been_redefined()) {
      assert(ik->previous_versions() == nullptr, "only redefined class can have previous versions");
      return;
    }

    InstanceKlass::purge_previous_versions(ik); // Remove redundant previous versions
    if (ik->previous_versions() != nullptr) {
      // TODO implement previous versions dumping (and fail on restore if the
      //  restoring VM won't have JVM TI included)
      ResourceMark rm;
      log_error(crac, class, dump)("Old versions of redefined %s's methods are still executing", ik->external_name());
      Unimplemented();
    }
  }

  // ###########################################################################
  // Instance and object array classes dumping
  // ###########################################################################

  void write_instance_class_data(InstanceKlass *ik) {
    precond(ik != nullptr);
    if (log_is_enabled(Trace, crac, class, dump)) {
      ResourceMark rm;
      log_trace(crac, class, dump)("Writing instance class data: %s (ID " UINTX_FORMAT ")",
                                   ik->external_name(), cast_from_oop<uintptr_t>(ik->java_mirror()));
    }

    WRITE_CLASS_ID(*ik);
    WRITE(checked_cast<u1>(loading_kind(*ik)));

    assert(ik->is_loaded(), "too young, must've been filtered out");
    assert(!ik->is_being_linked() && !ik->is_being_initialized(), "should've failed during stack dumping (linking thread must have an in-VM frame)");
    WRITE(checked_cast<u1>(ik->init_state()));
    if (ik->is_in_error_state()) {
      WRITE_OBJECT_ID(ik->get_initialization_error()); // Can be null
    }

    WRITE(ik->minor_version());
    WRITE(ik->major_version());
    WRITE(checked_cast<jint>(ik->constants()->version())); // Version of redefined classes (0 if not redefined), may be negative

    DO_CHECKED(write_class_flags(*ik));

    DO_CHECKED(write_class_attrs(*ik)); // Consatant pool parsing depends on NestHost attribute

    DO_CHECKED(write_constant_pool(*ik->constants()));
    if (ik->is_rewritten()) {
      precond(ik->constants()->cache() != nullptr);
      DO_CHECKED(write_constant_pool_cache(*ik->constants()->cache()));
    }

    WRITE(ik->this_class_index());
    DO_CHECKED(write_interfaces(*ik->local_interfaces()));

    DO_CHECKED(write_fields(*ik));

    DO_CHECKED(write_methods(*ik));

    DO_CHECKED(write_cached_class_file(ik->get_cached_class_file()));
    DO_CHECKED(write_previous_versions(ik));

    // TODO save and restore CDS-related stuff (if there is any that is portable)
  }

  // Dumps instance class and its array classes, ensuring its ancestors are
  // dumped first in the required order.
  void dump_class_hierarchy(InstanceKlass *ik) {
    precond(ik != nullptr);

    bool not_dumped_yet;
    _dumped_classes.put_if_absent(ik, &not_dumped_yet);
    if (!not_dumped_yet) {
      assert(ik->is_class_loader_instance_klass() ||
             ik->is_subtype_of(vmClasses::ProtectionDomain_klass()) ||
             ik->subklass() != nullptr || ik->is_interface(),
             "shouldn't have been dumped yet");
      return;
    }
    _dumped_classes.maybe_grow();

    if (ik->class_loader() != nullptr) {
      const oop loader_parent = java_lang_ClassLoader::parent(ik->class_loader());
      if (loader_parent != nullptr) {
        DO_CHECKED(dump_class_hierarchy(InstanceKlass::cast(loader_parent->klass())));
      }
      DO_CHECKED(dump_class_hierarchy(InstanceKlass::cast(ik->class_loader()->klass())));
    } else {
      assert(ik->class_loader_data()->is_boot_class_loader_data(), "must be");
    }

    if (ik->java_super() != nullptr) {
      DO_CHECKED(dump_class_hierarchy(ik->java_super()));
    }

    const Array<InstanceKlass *> &interfaces = *ik->local_interfaces();
    for (int i = 0; i < interfaces.length(); i++) {
      DO_CHECKED(dump_class_hierarchy(interfaces.at(i)));
    }

    DO_CHECKED(write_instance_class_data(ik));
    DO_CHECKED(write_obj_array_class_ids(ik));
  }

  // ###########################################################################
  // Initiating class loaders info
  // ###########################################################################

  void do_cld(ClassLoaderData *cld) override {
    if (cld->is_the_null_class_loader_data()) {
      // Bootstrap loader never delegates, so if it is an initiating loader than
      // it is also the defining one, and the defining loaders are known from
      // the heap dump
#ifdef ASSERT
      struct : public KlassClosure {
        void do_klass(Klass *k) override {
          assert(k->class_loader() == nullptr, "must be defined by the boot loader");
        }
      } asserter;
      cld->dictionary()->all_entries_do(&asserter);
#endif // ASSERT
      return;
    }
    if (cld->has_class_mirror_holder()) {
      // These CLDs are exclusive to the holder
      guarantee(cld->dictionary() == nullptr, "CLDs with mirror holder have no dictionaries");
      return;
    }
    postcond(cld->class_loader() != nullptr && cld->dictionary() != nullptr);
    assert(java_lang_ClassLoader::loader_data(cld->class_loader()) == cld,
           "must be true for CLD without a mirror holder");

    ResourceMark rm;
    GrowableArray<const InstanceKlass *> initiated_classes;
    // Find all classes known to the class loader but not defined by it.
    struct InitiatedKlassCollector : public KlassClosure {
      const ClassLoaderData &cld;
      GrowableArray<const InstanceKlass *> &iks;
      InitiatedKlassCollector(const ClassLoaderData &cld, GrowableArray<const InstanceKlass *> *iks) :
        cld(cld), iks(*iks) {}
      void do_klass(Klass *k) override {
        precond(k->is_instance_klass());
        if (k->class_loader_data() != &cld) {
          InstanceKlass *const ik = InstanceKlass::cast(k);
          assert(!ik->is_hidden(), "hidden classes cannot be seen outside of the defining loader");
          iks.append(ik);
        }
      }
    } collector(*cld, &initiated_classes);
    cld->dictionary()->all_entries_do(&collector);

    if (!initiated_classes.is_empty()) {
      WRITE_OBJECT_ID(cld->class_loader());
      WRITE(checked_cast<jint>(initiated_classes.length()));
      for (const InstanceKlass *ik : initiated_classes) {
        WRITE_CLASS_ID(*ik);
      }
    }
  }

#undef DO_CHECKED
#undef WRITE_RAW
#undef WRITE_CLASS_ID
#undef WRITE_OBJECT_ID
#undef WRITE_SYMBOL_ID
#undef WRITE
};

const char *CracClassDumper::dump(const char *path, bool overwrite) {
  guarantee(SafepointSynchronize::is_at_safepoint(), "need safepoint to ensure classes are not modified concurrently");
  log_info(crac, class, dump)("Dumping classes into %s", path);

  FileBasicTypeWriter file_writer;
  if (!file_writer.open(path, overwrite)) {
    return os::strerror(errno);
  }

  ClassDumpWriter dump_writer(&file_writer);
  dump_writer.write_dump();
  return dump_writer.io_error_msg();
}
