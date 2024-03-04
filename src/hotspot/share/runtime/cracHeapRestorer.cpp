#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "logging/log.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/markWord.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/symbol.hpp"
#include "oops/symbolHandle.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/cracHeapRestorer.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/fieldDescriptor.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/handles.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/reflectionUtils.hpp"
#include "runtime/signature.hpp"
#include "runtime/thread.hpp"
#include "utilities/bitCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpClasses.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/hprofTag.hpp"
#include "utilities/macros.hpp"
#include "utilities/methodKind.hpp"
#ifdef ASSERT
#include "code/dependencyContext.hpp"
#include "utilities/autoRestore.hpp"
#endif // ASSERT

// #############################################################################
// WellKnownObjects implementation
// #############################################################################

void WellKnownObjects::find_well_known_class_loaders(const ParsedHeapDump &heap_dump, TRAPS) {
  heap_dump.load_classes.iterate([&](HeapDump::ID _, const HeapDump::LoadClass &lc) -> bool {
    const Symbol *name = heap_dump.get_symbol(lc.class_name_id);
    if (name == vmSymbols::jdk_internal_loader_ClassLoaders()) {
      lookup_builtin_class_loaders(heap_dump, lc);
    } else if (name == vmSymbols::java_lang_ClassLoader()) {
      lookup_actual_system_class_loader(heap_dump, lc);
    }
    return (_platform_loader_id == HeapDump::NULL_ID && _builtin_system_loader_id == HeapDump::NULL_ID) ||
           _actual_system_loader_id == HeapDump::NULL_ID;
  });

  const bool platform_found = _platform_loader_id != HeapDump::NULL_ID;
  const bool builtin_sys_found = _builtin_system_loader_id != HeapDump::NULL_ID;
  const bool actual_sys_found = _actual_system_loader_id != HeapDump::NULL_ID;
  guarantee(!actual_sys_found || (platform_found && builtin_sys_found),
            "system class loader cannot be present when built-in class loaders are absent");
  guarantee(!builtin_sys_found || platform_found,
            "built-in system class loader cannot be present when the platform class loader is absent");
  guarantee(!platform_found || (_platform_loader_id != _builtin_system_loader_id &&
                                _platform_loader_id != _actual_system_loader_id),
            "platform and system class loaders cannot be the same instance");

  // If there is a diviation, abort the restoration
  if (builtin_sys_found && SystemDictionary::java_system_loader() != nullptr) {
    const bool is_dumped_actual_sys_builtin = _builtin_system_loader_id == _actual_system_loader_id;
    const bool is_current_actual_sys_builtin = SystemDictionary::java_system_loader()->klass() == vmClasses::jdk_internal_loader_ClassLoaders_AppClassLoader_klass();
    if (is_dumped_actual_sys_builtin != is_current_actual_sys_builtin) {
      THROW_MSG(vmSymbols::java_lang_UnsupportedOperationException(),
                err_msg("Dumped system class loader is%s the built-in one while in the current VM it is%s",
                        is_dumped_actual_sys_builtin ? "" : " not", is_current_actual_sys_builtin ? "" : " not"));
    }
  }

  log_info(crac)("Found well known class loaders' IDs: platform - "         HDID_FORMAT ", "
                                                      "built-in system - "  HDID_FORMAT ", "
                                                      "actual system - "    HDID_FORMAT,
                 _platform_loader_id, _builtin_system_loader_id, _actual_system_loader_id);
}

// This relies on the ClassLoader.get*ClassLoader() implementation detail: the
// built-in platform and system class loaders are stored in
// PLATFORM_LOADER/APP_LOADER static fields of jdk.internal.loader.ClassLoaders.
void WellKnownObjects::lookup_builtin_class_loaders(const ParsedHeapDump &heap_dump,
                                                    const HeapDump::LoadClass &jdk_internal_loader_ClassLoaders) {
  static constexpr char PLATFORM_LOADER_FIELD_NAME[] = "PLATFORM_LOADER";
  static constexpr char APP_LOADER_FIELD_NAME[] = "APP_LOADER";
  precond(heap_dump.get_symbol(jdk_internal_loader_ClassLoaders.class_name_id) ==
          vmSymbols::jdk_internal_loader_ClassLoaders());

  // We have a jdk.internal.loader.ClassLoaders but is this the internal one (i.e. boot-loaded)?
  const HeapDump::ClassDump &dump = heap_dump.get_class_dump(jdk_internal_loader_ClassLoaders.class_id);
  if (dump.class_loader_id != HeapDump::NULL_ID) {
    return;
  }
  // From now on we know we have THE jdk.internal.loader.ClassLoaders

  guarantee(_platform_loader_id == HeapDump::NULL_ID && _builtin_system_loader_id == HeapDump::NULL_ID,
            "class %s dumped multiple times", vmSymbols::jdk_internal_loader_ClassLoaders()->as_klass_external_name());
  for (u2 i = 0; i < dump.static_fields.size(); i++) {
    const HeapDump::ClassDump::Field &field_dump = dump.static_fields[i];
    if (field_dump.info.type != HPROF_NORMAL_OBJECT) {
      continue;
    }
    const Symbol *field_name = heap_dump.get_symbol(field_dump.info.name_id);
    if (field_name->equals(PLATFORM_LOADER_FIELD_NAME)) {
      guarantee(_platform_loader_id == HeapDump::NULL_ID,
                "static field %s is repeated in %s dump " HDID_FORMAT, PLATFORM_LOADER_FIELD_NAME,
                vmClasses::jdk_internal_loader_ClassLoaders_klass()->external_name(), dump.id);
      _platform_loader_id = field_dump.value.as_object_id;       // Can be null if VM was dumped before initializing it
    } else if (field_name->equals(APP_LOADER_FIELD_NAME)) {
      guarantee(_builtin_system_loader_id == HeapDump::NULL_ID,
                "static field %s is repeated in %s dump " HDID_FORMAT, APP_LOADER_FIELD_NAME,
                vmClasses::jdk_internal_loader_ClassLoaders_klass()->external_name(), dump.id);
      _builtin_system_loader_id = field_dump.value.as_object_id; // Can be null if VM was dumped before initializing it
    }
  }
}

// This relies on ClassLoader.getSystemClassLoader() implementation detail: the
// actual system class loader is stored in "scl" static field of j.l.ClassLoader.
void WellKnownObjects::lookup_actual_system_class_loader(const ParsedHeapDump &heap_dump,
                                                         const HeapDump::LoadClass &java_lang_ClassLoader) {
  static constexpr char SCL_FIELD_NAME[] = "scl";
  precond(heap_dump.get_symbol(java_lang_ClassLoader.class_name_id) == vmSymbols::java_lang_ClassLoader());

  // We know we have THE j.l.ClassLoader because classes from java.* packages cannot be non-boot-loaded
  const HeapDump::ClassDump &dump = heap_dump.get_class_dump(java_lang_ClassLoader.class_id);
  guarantee(dump.class_loader_id == HeapDump::NULL_ID, "class %s can only be loaded by the bootstrap class loader",
            vmSymbols::java_lang_ClassLoader()->as_klass_external_name());

  guarantee(_actual_system_loader_id == HeapDump::NULL_ID, "class %s dumped multiple times",
            vmSymbols::java_lang_ClassLoader()->as_klass_external_name());
  for (u2 i = 0; i < dump.static_fields.size(); i++) {
    const HeapDump::ClassDump::Field &field_dump = dump.static_fields[i];
    if (field_dump.info.type != HPROF_NORMAL_OBJECT) {
      continue;
    }
    const Symbol *field_name = heap_dump.get_symbol(field_dump.info.name_id);
    if (field_name->equals(SCL_FIELD_NAME)) {
      guarantee(_actual_system_loader_id == HeapDump::NULL_ID, "static field %s is repeated in %s dump " HDID_FORMAT,
                SCL_FIELD_NAME, vmSymbols::java_lang_ClassLoader()->as_klass_external_name(), dump.id);
      _actual_system_loader_id = field_dump.value.as_object_id; // Can be null if VM was dumped before initializing it
    }
  }
}

static instanceOop get_builtin_system_loader() {
  // SystemDictionary::java_system_loader() gives the actual system loader which
  // is not necessarily the built-in one
  const oop loader = SystemDictionary::java_system_loader();
  if (loader != nullptr &&
      loader->klass() != vmClasses::jdk_internal_loader_ClassLoaders_AppClassLoader_klass()) {
    // TODO need to call into Java (ClassLoaders.appClassLoader()) or retrieve
    // the oop from ClassLoaders::APP_LOADER manually
    log_error(crac)("User-provided system class loader is not supported yet");
    Unimplemented();
  }
  return static_cast<instanceOop>(loader);
}

void WellKnownObjects::put_into(HeapDumpTable<Handle, AnyObj::C_HEAP> *objects) const {
  precond(objects->number_of_entries() == 0);
  Thread *const thread = JavaThread::current();
  if (_platform_loader_id != HeapDump::NULL_ID) {
    const auto loader = static_cast<instanceOop>(SystemDictionary::java_platform_loader());
    if (loader != nullptr) {
      guarantee(loader->klass() == vmClasses::jdk_internal_loader_ClassLoaders_PlatformClassLoader_klass(), "sanity check");
      objects->put_when_absent(_platform_loader_id, instanceHandle(thread, loader));
    }
  }
  if (_builtin_system_loader_id != HeapDump::NULL_ID) {
    const auto loader = get_builtin_system_loader();
    if (loader != nullptr) {
      objects->put_when_absent(_builtin_system_loader_id, instanceHandle(thread, loader));
    }
  }
  if (_actual_system_loader_id != HeapDump::NULL_ID && _actual_system_loader_id != _builtin_system_loader_id) {
    const auto loader = static_cast<instanceOop>(SystemDictionary::java_system_loader());
    if (loader != nullptr) {
      objects->put_when_absent(_builtin_system_loader_id, instanceHandle(thread, loader));
    }
  }
  objects->maybe_grow();
}

void WellKnownObjects::get_from(const HeapDumpTable<Handle, AnyObj::C_HEAP> &objects) const {
  if (_platform_loader_id != HeapDump::NULL_ID) {
    const Handle *restored = objects.get(_platform_loader_id);
    if (restored != nullptr) {
      const oop existing = SystemDictionary::java_platform_loader();
      if (existing != nullptr) {
        guarantee(*restored == existing, "restored platform loader must be the existing one");
      } else {
        log_error(crac)("Restoration of base class loaders is not implemented");
        Unimplemented();
      }
    }
  }
  if (_builtin_system_loader_id != HeapDump::NULL_ID) {
    const Handle *restored = objects.get(_builtin_system_loader_id);
    if (restored != nullptr) {
      const oop existing = get_builtin_system_loader();
      if (existing != nullptr) {
        guarantee(*restored == existing, "restored builtin system loader must be the existing one");
      } else {
        log_error(crac)("Restoration of base class loaders is not implemented");
        Unimplemented();
      }
    }
  }
  if (_actual_system_loader_id != HeapDump::NULL_ID) {
    const Handle *restored = objects.get(_actual_system_loader_id);
    if (restored != nullptr) {
      const oop existing = SystemDictionary::java_system_loader();
      if (existing != nullptr) {
        guarantee(*restored == existing, "restored actual system loader must be the existing one");
      } else {
        log_error(crac)("Restoration of base class loaders is not implemented");
        Unimplemented();
      }
    }
  }
}

// #############################################################################
// CracHeapRestorer implementation
// #############################################################################

// Helpers

InstanceKlass &CracHeapRestorer::get_instance_class(HeapDump::ID id) const {
  InstanceKlass **const ik_ptr = _instance_classes.get(id);
  guarantee(ik_ptr != nullptr, "unknown instance class " HDID_FORMAT " referenced", id);
  assert(*ik_ptr != nullptr, "must be");
  return **ik_ptr;
}

ArrayKlass &CracHeapRestorer::get_array_class(HeapDump::ID id) const {
  ArrayKlass **const ak_ptr = _array_classes.get(id);
  guarantee(ak_ptr != nullptr, "unknown array class " HDID_FORMAT " referenced", id);
  assert(*ak_ptr != nullptr, "must be");
  return **ak_ptr;
}

Handle CracHeapRestorer::get_object_when_present(HeapDump::ID id) const {
  assert(id != HeapDump::NULL_ID, "nulls are not recorded");
  assert(_objects.contains(id), "object " HDID_FORMAT " was expected to be recorded", id);
  return *_objects.get(id);
}

Handle CracHeapRestorer::get_object_if_present(HeapDump::ID id) const {
  assert(id != HeapDump::NULL_ID, "nulls are not recorded");
  const Handle *obj_h = _objects.get(id);
  return obj_h != nullptr ? *obj_h : Handle();
}

void CracHeapRestorer::put_object_when_absent(HeapDump::ID id, Handle obj) {
  assert(id != HeapDump::NULL_ID && obj.not_null(), "nulls should not be recorded");
  assert(!_objects.contains(id), "object " HDID_FORMAT " was expected to be absent", id);
  _objects.put_when_absent(id, obj);
  _objects.maybe_grow();
}

void CracHeapRestorer::put_object_if_absent(HeapDump::ID id, Handle obj) {
  assert(id != HeapDump::NULL_ID && obj.not_null(), "nulls should not be recorded");
  bool is_absent;
  const Handle &res = *_objects.put_if_absent(id, obj, &is_absent);
  guarantee(res == obj, "two different objects restored for ID " HDID_FORMAT ": " PTR_FORMAT " (%s) != " PTR_FORMAT " (%s)",
            id, p2i(res()), res->klass()->external_name(), p2i(obj()), obj->klass()->external_name());
  if (is_absent) {
    _objects.maybe_grow();
  }
}

#ifdef ASSERT
static void assert_builtin_class_instance(const ParsedHeapDump &heap_dump, HeapDump::ID obj_id,
                                          const Symbol *expected_class_name)  {
  precond(obj_id != HeapDump::NULL_ID);
  const HeapDump::InstanceDump &dump = heap_dump.get_instance_dump(obj_id);
  const Symbol *class_name = heap_dump.get_class_name(dump.class_id);
  const HeapDump::ID &class_loader_id = heap_dump.get_class_dump(dump.class_id).class_loader_id;
  assert(class_name == expected_class_name && class_loader_id == HeapDump::NULL_ID,
         "expected object " HDID_FORMAT " to be of the boot-loaded class %s but its class is %s loaded by " HDID_FORMAT,
         obj_id, expected_class_name->as_klass_external_name(), class_name->as_klass_external_name(), class_loader_id);
}
#endif // ASSERT

// Class loader preparation

instanceHandle CracHeapRestorer::get_class_loader_parent(const HeapDump::InstanceDump &loader_dump, TRAPS) {
  const HeapDump::ID parent_id = _loader_dump_reader.parent(loader_dump);
  guarantee(parent_id != loader_dump.id, "class loader hierarchy circularity: "
            HDID_FORMAT " references itself as its parent", loader_dump.id);
  const instanceHandle loader = get_class_loader(parent_id, CHECK_({}));
  return loader;
}

instanceHandle CracHeapRestorer::get_class_loader_name(const HeapDump::InstanceDump &loader_dump, bool with_id, TRAPS) {
  const HeapDump::ID name_id = with_id ? _loader_dump_reader.nameAndId(loader_dump) : _loader_dump_reader.name(loader_dump);
  if (name_id == HeapDump::NULL_ID) {
    return {};
  }
  DEBUG_ONLY(assert_builtin_class_instance(_heap_dump, name_id, vmSymbols::java_lang_String()));

  const Handle &str = restore_object(name_id, CHECK_({}));
  guarantee(str->klass() == vmClasses::String_klass(), "class loader " HDID_FORMAT " has its '%s' field referencing a %s "
            "but it must reference a %s", loader_dump.id, with_id ? "nameAndId" : "name",
            str->klass()->external_name(), vmSymbols::java_lang_String()->as_klass_external_name());

  return static_cast<const instanceHandle &>(str);
}

instanceHandle CracHeapRestorer::get_class_loader_unnamed_module(const HeapDump::InstanceDump &loader_dump, TRAPS) {
  const HeapDump::ID unnamed_module_id = _loader_dump_reader.unnamedModule(loader_dump);
  guarantee(unnamed_module_id != HeapDump::NULL_ID, "class loader " HDID_FORMAT " cannot be used to load classes: "
            "its 'unnamedModule' field is not set", loader_dump.id);
  DEBUG_ONLY(assert_builtin_class_instance(_heap_dump, unnamed_module_id, vmSymbols::java_lang_Module()));

  const Handle &unnamed_module = restore_object(unnamed_module_id, CHECK_({}));
  guarantee(unnamed_module->klass() == vmClasses::Module_klass(),
            "class loader " HDID_FORMAT " has its 'unnamedModule' field referencing a %s but it must reference a %s",
            loader_dump.id, unnamed_module->klass()->external_name(), vmSymbols::java_lang_Module()->as_klass_external_name());
#ifdef ASSERT
  { // Would be better to check all fields but loader are null and do it before restoring the object, but it's harder
    assert(java_lang_Module::name(unnamed_module()) == nullptr,
           "unnamed module of class loader " HDID_FORMAT " is not unnamed", loader_dump.id);
    const Handle loader = get_object_when_present(loader_dump.id);
    assert(loader == java_lang_Module::loader(unnamed_module()),
           "unnamed module of class loader " HDID_FORMAT " belongs to a different class loader", loader_dump.id);
  }
#endif // ASSERT
  return static_cast<const instanceHandle &>(unnamed_module);
}

instanceHandle CracHeapRestorer::get_class_loader_parallel_lock_map(const HeapDump::InstanceDump &loader_dump, TRAPS) {
  const HeapDump::ID map_id = _loader_dump_reader.parallelLockMap(loader_dump);
  if (map_id == HeapDump::NULL_ID) {
    return {};
  }
  DEBUG_ONLY(assert_builtin_class_instance(_heap_dump, map_id, vmSymbols::java_util_concurrent_ConcurrentHashMap()));

  // Check for null above, so it's either already created or we need to create one
  const Handle existing_map = get_object_if_present(map_id);
  if (existing_map == nullptr) {
    const instanceHandle map = vmClasses::ConcurrentHashMap_klass()->allocate_instance_handle(CHECK_({}));
    put_object_when_absent(map_id, map);
    return map;
  }
  guarantee(existing_map->klass() == vmClasses::ConcurrentHashMap_klass(),
            "class loader " HDID_FORMAT " has its 'parallelLockMap' field referencing a %s but it must reference a %s",
            loader_dump.id, existing_map->klass()->external_name(), vmClasses::ConcurrentHashMap_klass()->external_name());
  precond(existing_map->is_instance());
  return *static_cast<const instanceHandle *>(&existing_map);
}

// Allocates class loader and restores fields the VM may use for class loading:
// - parent -- to set dependent classes, to get non-reflection loader
// - name and nameAndId -- to create a CLD and print logs/errors
// - unnamedModule -- used and partially filled when creating the CLD
// - parallelLockMap -- defines whether the class loader is parallel capable,
//   only need the null/not-null fact, so no need to restore its state yet
instanceHandle CracHeapRestorer::prepare_class_loader(HeapDump::ID id, TRAPS) {
  log_trace(crac)("Preparing class loader " HDID_FORMAT, id);
  assert(id != HeapDump::NULL_ID, "cannot prepare the bootstrap loader");
  const HeapDump::InstanceDump &dump = _heap_dump.get_instance_dump(id);
  _loader_dump_reader.ensure_initialized(_heap_dump, dump.class_id);

  InstanceKlass &loader_klass = get_instance_class(dump.class_id);
  guarantee(loader_klass.is_class_loader_instance_klass(),
            "class loader " HDID_FORMAT " is of class %s (" HDID_FORMAT ") "
            "which does not subclass %s", id, loader_klass.external_name(), dump.class_id,
            vmSymbols::java_lang_ClassLoader()->as_klass_external_name());
  guarantee(loader_klass.is_being_restored() || loader_klass.is_initialized(), "class loader " HDID_FORMAT " cannot be an instance of "
            "uninitialized class %s (" HDID_FORMAT ")", dump.id, loader_klass.external_name(), dump.class_id);
  loader_klass.check_valid_for_instantiation(true, CHECK_({}));

  const instanceHandle loader = loader_klass.allocate_instance_handle(CHECK_({}));
  put_object_when_absent(id, loader); // Must record right now to be able to find it when restoring unnamedModule
  _prepared_loaders.put_when_absent(id, true);
  _prepared_loaders.maybe_grow();

  {
    const instanceHandle parent = get_class_loader_parent(dump, CHECK_({}));
    java_lang_ClassLoader::set_parent(loader(), parent());
  }
  {
    const instanceHandle name = get_class_loader_name(dump, /* with_id = */ false, CHECK_({}));
    java_lang_ClassLoader::set_name(loader(), name());
  }
  {
    const instanceHandle name_and_id = get_class_loader_name(dump, /* with_id = */ true, CHECK_({}));
    java_lang_ClassLoader::set_nameAndId(loader(), name_and_id());
  }
  {
    const instanceHandle unnamedModule = get_class_loader_unnamed_module(dump, CHECK_({}));
    java_lang_ClassLoader::set_unnamedModule(loader(), unnamedModule());
  }
  {
    const instanceHandle parallel_lock_map = get_class_loader_parallel_lock_map(dump, CHECK_({}));
    java_lang_ClassLoader::set_parallelLockMap(loader(), parallel_lock_map());
  }

  if (java_lang_ClassLoader::parallelCapable(loader())) { // Works because we set parallelLockMap above
    // TODO should add it into ClassLoader$ParallelLoaders::loaderTypes array
    log_error(crac)("Restoration of parallel-capable class loaders is not implemented");
    Unimplemented();
  }

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Prepared class loader " HDID_FORMAT " (%s)", id, loader->klass()->external_name());
  }
  return loader;
}

instanceHandle CracHeapRestorer::get_class_loader(HeapDump::ID id, TRAPS) {
  if (id == HeapDump::NULL_ID) {
    return {}; // Bootstrap loader
  }

  const Handle existing_loader = get_object_if_present(id);
  if (existing_loader.not_null()) {
    guarantee(existing_loader->klass()->is_class_loader_instance_klass(),
              "object " HDID_FORMAT " is not a class loader: its class %s does not subclass %s",
              id, existing_loader->klass()->external_name(), vmSymbols::java_lang_ClassLoader()->as_klass_external_name());
    precond(existing_loader->is_instance());
    return *static_cast<const instanceHandle *>(&existing_loader);
  }

  precond(!_prepared_loaders.contains(id));
  const instanceHandle loader = prepare_class_loader(id, CHECK_({})); // Allocate and partially restore the loader
  postcond(_prepared_loaders.contains(id) && get_object_when_present(id) == loader());
  guarantee(loader.not_null() && loader->klass()->is_class_loader_instance_klass(), "must be a class loader");

  return loader;
}

// Heap restoration driver

static bool is_jdk_crac_Core(const InstanceKlass &ik) {
  if (ik.name() == vmSymbols::jdk_crac_Core() && ik.class_loader_data()->is_the_null_class_loader_data()) {
    assert(ik.is_initialized(), "%s is not pre-initialized", ik.external_name());
    return true;
  }
  return false;
}

void CracHeapRestorer::restore_heap(const HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> &class_infos,
                                    const GrowableArrayView<CracStackTrace *> &stack_traces, TRAPS) {
  log_info(crac)("Started heap restoration");
  HandleMark hm(Thread::current());

  // Before actually restoring anything, record existing objects so that they
  // are not re-created
  // TODO Currently only the mirrors themselves + contents of a few of their
  //  fields are recorded. Ideally, we should walk recursively and record all
  //  existing objects so that we don't re-create them, but this should be
  //  fairly complex since the dumped and the current state may not match.
  _heap_dump.class_dumps.iterate([&](HeapDump::ID _, const HeapDump::ClassDump &dump) -> bool {
    find_and_record_class_mirror(dump, CHECK_false);
    return true;
  });
  if (HAS_PENDING_EXCEPTION) {
    return;
  }

  // Restore objects reachable from classes being restored.
  // TODO should also restore array and primitive mirrors?
  _instance_classes.iterate([&](HeapDump::ID class_id, InstanceKlass *ik) -> bool {
    if (!ik->is_being_restored()) {
      // TODO jdk.crac.Core is pre-initialized but we need to restore its fields
      //  since the global resource context is among them. This discards the new
      //  global context but we assume it is a subset of the restored one. Such
      //  special treatment should be removed when we implement restoration of
      //  all classes (it should stop being pre-initialized then).
      if (is_jdk_crac_Core(*ik)) {
        const HeapDump::ClassDump &dump = _heap_dump.get_class_dump(class_id);
        restore_static_fields(ik, dump, CHECK_false);
      }
      return true; // Skip pre-initialized since they may already have a new state
    }

    precond(class_infos.contains(class_id));
    const UnfilledClassInfo &info = *class_infos.get(class_id);

    restore_class_mirror(class_id, CHECK_false);

    Handle init_error;
    if (info.class_initialization_error_id != HeapDump::NULL_ID) {
      init_error = restore_object(info.class_initialization_error_id, CHECK_false);
      guarantee(init_error->is_instance(), "%s's initialization exception " HDID_FORMAT " is an array",
                init_error->klass()->external_name(), info.class_initialization_error_id);
    }
    CracClassStateRestorer::apply_init_state(ik, info.target_state, init_error);

    return true;
  });
  if (HAS_PENDING_EXCEPTION) {
    return;
  }
#ifdef ASSERT
  _instance_classes.iterate_all([](HeapDump::ID _, const InstanceKlass *ik) {
    assert(!ik->is_being_restored(), "%s has not been restored", ik->external_name());
    CracClassStateRestorer::assert_hierarchy_init_states_are_consistent(*ik);
  });
#endif // ASSERT
  guarantee(_prepared_loaders.number_of_entries() == 0, "some prepared class loaders have not defined any classes");

  // Restore objects reachable from the thread stacks
  for (const auto *trace : stack_traces) {
    for (u4 frame_i = 0; frame_i < trace->frames_num(); frame_i++) {
      const CracStackTrace::Frame &frame = trace->frame(frame_i);
      const u2 num_locals = frame.locals().length();
      for (u2 loc_i = 0; loc_i < num_locals; loc_i++) {
        CracStackTrace::Frame::Value &value = *frame.locals().adr_at(loc_i);
        if (value.type() == CracStackTrace::Frame::Value::Type::REF) {
          const Handle obj = restore_object(value.as_obj_id(), CHECK);
          value = CracStackTrace::Frame::Value::of_obj(obj);
        }
      }
      const u2 num_operands = frame.operands().length();
      for (u2 op_i = 0; op_i < num_operands; op_i++) {
        CracStackTrace::Frame::Value &value = *frame.operands().adr_at(op_i);
        if (value.type() == CracStackTrace::Frame::Value::Type::REF) {
          const Handle obj = restore_object(value.as_obj_id(), CHECK);
          value = CracStackTrace::Frame::Value::of_obj(obj);
        }
      }
    }
  }

  _well_known_objects.get_from(_objects);
  log_info(crac)("Finished heap restoration");
}

// Recording of existing objects

// Finds j.l.Class object corresponding to the class dump and records it.
void CracHeapRestorer::find_and_record_class_mirror(const HeapDump::ClassDump &class_dump, TRAPS) {
  Thread *const current = Thread::current();

  const HeapDump::InstanceDump &mirror_dump = _heap_dump.get_instance_dump(class_dump.id);
  _mirror_dump_reader.ensure_initialized(_heap_dump, mirror_dump.class_id);
  using MirrorType = HeapDumpClasses::java_lang_Class::Kind;
  switch (_mirror_dump_reader.kind(mirror_dump)) {
    case MirrorType::INSTANCE: {
      const InstanceKlass &ik = get_instance_class(class_dump.id);
      const instanceHandle mirror(current, static_cast<instanceOop>(ik.java_mirror()));
      record_class_mirror(mirror, mirror_dump, CHECK);
      break;
    }
    case MirrorType::ARRAY: {
      const ArrayKlass &ak = get_array_class(class_dump.id);
      const instanceHandle mirror(current, static_cast<instanceOop>(ak.java_mirror()));
      record_class_mirror(mirror, mirror_dump, CHECK);

      // Primitive mirrors are also recorded here because they don't have a
      // Klass to be dumped with directly but always have a TypeArrayKlass
      if (ak.is_typeArray_klass() && &ak != Universe::fillerArrayKlassObj() /* same as int[] */) {
        const oop prim_mirror_obj = java_lang_Class::component_mirror(mirror());
        assert(prim_mirror_obj != nullptr, "type array's mirror must have a component mirror");
        const instanceHandle prim_mirror(current, static_cast<instanceOop>(prim_mirror_obj));

        const HeapDump::ID prim_mirror_dump_id = _mirror_dump_reader.componentType(mirror_dump);
        guarantee(prim_mirror_dump_id != HeapDump::NULL_ID, "primitive array " HDID_FORMAT " has no component type",
                  prim_mirror_dump_id);
        const HeapDump::InstanceDump &prim_mirror_dump = _heap_dump.get_instance_dump(prim_mirror_dump_id);

        record_class_mirror(prim_mirror, prim_mirror_dump, CHECK);
      }
      break;
    }
    case MirrorType::PRIMITIVE:
      // Class dumps are only created from InstanceKlasses and ArrayKlasses
      guarantee(false, "instance or array class " HDID_FORMAT " has a primitive type mirror", class_dump.id);
  }
}

void CracHeapRestorer::record_class_mirror(instanceHandle mirror, const HeapDump::InstanceDump &mirror_dump, TRAPS) {
  if (log_is_enabled(Trace, crac)) {
    Klass *mirrored_class;
    const BasicType bt = java_lang_Class::as_BasicType(mirror(), &mirrored_class);
    const char *type_name = is_reference_type(bt) ? mirrored_class->external_name() : type2name(bt);
    log_trace(crac)("Recording class mirror " HDID_FORMAT " of %s", mirror_dump.id, type_name);
  }
  precond(!_objects.contains(mirror_dump.id) && mirror.not_null());
  put_object_when_absent(mirror_dump.id, mirror);

  _mirror_dump_reader.ensure_initialized(_heap_dump, mirror_dump.class_id);

  const HeapDump::ID module_id = _mirror_dump_reader.module(mirror_dump);
  const auto module_obj = static_cast<instanceOop>(java_lang_Class::module(mirror()));
  assert(module_obj != nullptr, "module must be set");
  put_object_if_absent(module_id, instanceHandle(Thread::current(), module_obj)); // Can be pre-recorded via another class from this module

  // Name can be initialized concurrently, so if it was dumped, initialize and
  // record it eagerly
  const HeapDump::ID name_id = _mirror_dump_reader.name(mirror_dump);
  if (name_id != HeapDump::NULL_ID) {
    const oop name_oop = java_lang_Class::name(mirror, CHECK);
    const auto name_obj = static_cast<instanceOop>(name_oop);
    put_object_if_absent(name_id, instanceHandle(Thread::current(), name_obj)); // Checks it's either absent or set to the same oop
  }

#ifdef ASSERT
  // TODO would be more accurate to check the classLoader field of the mirror dump itself
  if (!java_lang_Class::is_primitive(mirror())) {
    const HeapDump::ID loader_id = _heap_dump.get_class_dump(mirror_dump.id).class_loader_id;
    assert(loader_id == HeapDump::NULL_ID || get_object_when_present(loader_id) == java_lang_Class::class_loader(mirror()),
           "class loader must already be recorded");
  }

  const HeapDump::ID component_mirror_id = _mirror_dump_reader.componentType(mirror_dump);
  const oop expected_component_mirror = java_lang_Class::component_mirror(mirror());
  assert((component_mirror_id == HeapDump::NULL_ID) == (expected_component_mirror == nullptr),
         "component mirror must be dumped iff it exists in the runtime");
  if (component_mirror_id != HeapDump::NULL_ID) {
    assert(_heap_dump.instance_dumps.contains(component_mirror_id), "unknown component mirror " HDID_FORMAT, component_mirror_id);
    const Handle component_mirror = get_object_if_present(component_mirror_id); // May not be recorded yet
    if (component_mirror.not_null()) {
      assert(component_mirror == expected_component_mirror, "unexpected component mirror recorded as " HDID_FORMAT, component_mirror_id);
    } else {
      assert(java_lang_Class::is_primitive(expected_component_mirror) || _heap_dump.class_dumps.contains(component_mirror_id),
             "non-primitive component mirror " HDID_FORMAT " corresponds to no class", component_mirror_id);
    }
  }
#endif

  // TODO for the pre-created mirrors, should we fill the rest of the mirror
  //  instance fields + class static fields?
  //  - If we do, it's not straight forward because the fields may have
  //    different values of different classes than they were when dumped
  //  - If we don't and these values were references somewhere in the dump,
  //    they will be restored and thus duplicated
}

// Field restoration
// TODO use other means to iterate over fields: FieldStream performs a linear
//  search for each field

#ifdef ASSERT

static bool is_same_basic_type(const Symbol *signature, BasicType dump_t, bool allow_intptr_t = false) {
  const BasicType sig_t = Signature::basic_type(signature);
  return sig_t == dump_t ||
         // Heap dump uses T_OBJECT for arrays
         (sig_t == T_ARRAY && dump_t == T_OBJECT) ||
         // Java equivalent of intptr_t is platform-dependent
         (allow_intptr_t && signature == vmSymbols::intptr_signature() && (dump_t == T_INT || dump_t == T_LONG));
}

static Klass *get_ref_field_type(const InstanceKlass &holder, Symbol *signature) {
  Thread *const thread = Thread::current();
  const Handle holder_loader = Handle(thread, holder.class_loader());
  if (Signature::has_envelope(signature)) {
    const TempNewSymbol class_name = Signature::strip_envelope(signature);
    return SystemDictionary::find_constrained_instance_or_array_klass(thread, class_name, holder_loader);
  }
  return SystemDictionary::find_constrained_instance_or_array_klass(thread, signature, holder_loader);
}

#endif // ASSERT

void CracHeapRestorer::set_field(instanceHandle obj, const FieldStream &fs, const HeapDump::BasicValue &val, TRAPS) {
  precond(obj.not_null());
  DEBUG_ONLY(const InstanceKlass &field_holder = *fs.field_descriptor().field_holder());
  assert(!fs.access_flags().is_static() || field_holder.init_state() < InstanceKlass::fully_initialized,
         "trying to modify static field %s of pre-initialized class %s", fs.name()->as_C_string(), field_holder.external_name());
  // Static fields of pre-defined classes already have their initial values set
  // but we can overwrite them until the class is marked initialized
  DEBUG_ONLY(const bool prefilled = fs.access_flags().is_static() && fs.field_descriptor().has_initial_value());
  switch (Signature::basic_type(fs.signature())) {
    case T_OBJECT:
    case T_ARRAY: {
      precond(prefilled || obj->obj_field(fs.offset()) == nullptr);
      const Handle restored = restore_object(val.as_object_id, CHECK);
#ifdef ASSERT
      if (restored.not_null()) {
        Klass *const field_type = get_ref_field_type(field_holder, fs.signature());
        // TODO until restoration of loader constraints is implemented we may get null here
        // assert(field_type != nullptr, "field's type must be loaded since the field is assigned");
        if (field_type != nullptr) {
          assert(restored->klass()->is_subtype_of(field_type), "field of type %s cannot be assigned a value of class %s",
                 fs.signature()->as_C_string(), restored->klass()->external_name());
        } else {
          log_warning(crac, class)("Loader constraint absent: %s should be constrained on loading %s",
                                   field_holder.class_loader_data()->loader_name_and_id(), fs.signature()->as_C_string());
        }
      }
#endif // ASSERT
      obj->obj_field_put(fs.offset(), restored());
      break;
    }
    case T_BOOLEAN: precond(prefilled || obj->bool_field(fs.offset()) == false); obj->bool_field_put(fs.offset(), val.as_boolean);  break;
    case T_CHAR:    precond(prefilled || obj->char_field(fs.offset()) == 0);     obj->char_field_put(fs.offset(), val.as_char);     break;
    case T_FLOAT:   precond(prefilled || obj->float_field(fs.offset()) == 0.0F); obj->float_field_put(fs.offset(), val.as_float);   break;
    case T_DOUBLE:  precond(prefilled || obj->double_field(fs.offset()) == 0.0); obj->double_field_put(fs.offset(), val.as_double); break;
    case T_BYTE:    precond(prefilled || obj->byte_field(fs.offset()) == 0);     obj->byte_field_put(fs.offset(), val.as_byte);     break;
    case T_SHORT:   precond(prefilled || obj->short_field(fs.offset()) == 0);    obj->short_field_put(fs.offset(), val.as_short);   break;
    case T_INT:     precond(prefilled || obj->int_field(fs.offset()) == 0);      obj->int_field_put(fs.offset(), val.as_int);       break;
    case T_LONG:    precond(prefilled || obj->long_field(fs.offset()) == 0);     obj->long_field_put(fs.offset(), val.as_long);     break;
    default:        ShouldNotReachHere();
  }
}

template<class OBJ_DUMP_T>
static void restore_identity_hash(oop obj, const OBJ_DUMP_T &dump) {
  const auto hash = bit_cast<jint>(dump.stack_trace_serial); // We use HPROF's stack_trace_serial to store identity hash
  guarantee((hash & markWord::hash_mask) == checked_cast<decltype(markWord::hash_mask)>(hash), "identity hash too big: %i", hash);
  if (hash == markWord::no_hash) {
    return; // No hash computed at dump time, nothing to restore
  }

  log_trace(crac)("Restoring " HDID_FORMAT ": identity hash", dump.id);
  const intptr_t installed_hash = obj->identity_hash(hash);
  if (installed_hash != hash) {
#ifdef ASSERT
    if (obj->klass()->is_instance_klass()) {
      const InstanceKlass *ik = InstanceKlass::cast(obj->klass());
      assert(!ik->is_being_restored() && ik->is_initialized(), "can only happen to pre-initialized classes");
    } else if (obj->klass()->is_objArray_klass()) {
      const Klass *bk = ObjArrayKlass::cast(obj->klass())->bottom_klass();
      const InstanceKlass *ik = InstanceKlass::cast(bk);
      assert(!ik->is_being_restored() && ik->is_initialized(), "can only happen to pre-initialized classes");
    } else {
      assert(obj->klass()->is_typeArray_klass(), "must be"); // No InstanceKlass to check
    }
#endif // ASSERT
    if (log_is_enabled(Info, crac)) {
      ResourceMark rm;
      log_info(crac)("Pre-created object " INTPTR_FORMAT " (%s) differs in identity hash: saved with %i, now got " INTX_FORMAT,
                     cast_from_oop<intptr_t>(obj), obj->klass()->external_name(), hash, installed_hash);
    }
  }
}

bool CracHeapRestorer::set_class_loader_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                                  const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  precond(obj->klass()->is_subclass_of(vmClasses::ClassLoader_klass()));
  precond(!obj_fs.access_flags().is_static());
  if (obj_fs.field_descriptor().field_holder() != vmClasses::ClassLoader_klass()) {
    return false;
  }

  const Symbol *field_name = obj_fs.name();

  // Skip the CLD pointer which is set when registering the loader
  if (field_name == vmSymbols::loader_data_name()) {
    return true;
  }

  // Restoration is only called for prepared or just allocated class loaders
  // Note: don't check _prepared_loaders here because we pop the loader from
  // there before restoring it, and also this should be more efficient
  const bool is_prepared = java_lang_ClassLoader::unnamedModule(obj()) != nullptr;
  if (!is_prepared) {
    // The loader has just been allocated by us and has not been prepared, so
    // can restore it as a general object
    return false;
  }

  // Skip the fields already restored during the preparation
  if (field_name == vmSymbols::parent_name() || field_name == vmSymbols::name_name() ||
      field_name->equals("nameAndId") || field_name->equals("unnamedModule")) {
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID obj_id = dump_fs.value().as_object_id;
    assert(obj_id == HeapDump::NULL_ID && obj->obj_field(obj_fs.offset()) == nullptr ||
           get_object_when_present(obj_id) == obj->obj_field(obj_fs.offset()),
           "either null or recorded with same value");
    return true;
  }

  // When preparing, parallelLockMap is only allocated and left unrestored, so
  // restore it now
  if (field_name->equals("parallelLockMap")) {
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID parallel_lock_map_id = dump_fs.value().as_object_id;
    const oop parallel_lock_map = obj->obj_field(obj_fs.offset());
    if (parallel_lock_map != nullptr) {
      assert(parallel_lock_map->klass() == vmClasses::ConcurrentHashMap_klass(), "must be");
      assert(get_object_when_present(parallel_lock_map_id) == parallel_lock_map, "must be recorded when preparing");
      const HeapDump::InstanceDump &parallel_lock_map_dump = _heap_dump.get_instance_dump(parallel_lock_map_id);
      restore_identity_hash(parallel_lock_map, parallel_lock_map_dump);
      restore_instance_fields(obj, parallel_lock_map_dump, CHECK_false);
    } else {
      assert(parallel_lock_map_id == HeapDump::NULL_ID, "must be");
    }
    return true;
  }

  // The rest of the fields are untouched by the preparation and should be
  // restored as usual
  return false;
}

bool CracHeapRestorer::set_class_mirror_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                                  const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  assert(obj_fs.field_descriptor().field_holder() == vmClasses::Class_klass(), "must be");
  precond(!obj_fs.access_flags().is_static());
  const Symbol *field_name = obj_fs.name();

  // Skip primitive fields set when creating the mirror
  if (field_name == vmSymbols::klass_name() || field_name == vmSymbols::array_klass_name() ||
      field_name == vmSymbols::oop_size_name() || field_name == vmSymbols::static_oop_field_count_name()) {
    return true;
  }
  // Component class mirror (aka component type) is also set when creating the
  // mirror iff it corresponds to an array class, and it must be already
  // recorded because we pre-record all mirors
  if (field_name == vmSymbols::componentType_name()) {
#ifdef ASSERT
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID component_mirror_id = dump_fs.value().as_object_id;
    if (component_mirror_id != HeapDump::NULL_ID) {
      assert(java_lang_Class::as_Klass(obj())->is_array_klass(),
              "a %s<%s> object has 'componentType' dumped referencing " HDID_FORMAT " when it represents a non-array class",
              vmSymbols::java_lang_Class()->as_klass_external_name(), java_lang_Class::as_Klass(obj())->external_name(), component_mirror_id);
      const oop component_mirror = obj->obj_field(obj_fs.offset());
      assert(component_mirror != nullptr, "array class mirror must have its component mirror set");
      assert(get_object_when_present(component_mirror_id) == component_mirror, "component class mirror must be pre-recorded");
    } else {
      assert(java_lang_Class::as_Klass(obj())->is_instance_klass(),
              "a %s<%s> object has 'componentType' dumped as null when it represents an array class",
              vmSymbols::java_lang_Class()->as_klass_external_name(), java_lang_Class::as_Klass(obj())->external_name());
      assert(obj->obj_field(obj_fs.offset()) == nullptr, "instance class mirror cannot have its component mirror set");
    }
#endif // ASSERT
    return true;
  }
  // Module is also set when creating the mirror and is pre-recorded
  if (field_name->equals("module")) {
#ifdef ASSERT
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID module_id = dump_fs.value().as_object_id;
    const oop module = obj->obj_field(obj_fs.offset());
    assert(module_id != HeapDump::NULL_ID && module != nullptr, "mirror's module is always not null");
    assert(get_object_when_present(module_id) == module, "mirror's module must be pre-recorded");
#endif // ASSERT
    return true;
  }

  // Name can be set concurrently and thus is pre-recorded if it existed at dump time
  if (field_name == vmSymbols::name_name()) {
#ifdef ASSERT
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID name_id = dump_fs.value().as_object_id;
    if (name_id != HeapDump::NULL_ID) {
      const oop name = obj->obj_field(obj_fs.offset());
      assert(name != nullptr, "non-null-dumped mirror's name must be pre-initialized");
      assert(get_object_when_present(name_id) == name, "non-null-dumped mirror's name must be pre-recorded");
    }
#endif // ASSERT
    return true;
  }

  // If the defining loader is a prepared one we should restore the fields
  // unfilled by its preparation, and unmark the loader as prepared so that
  // this won't be repeated when restoring other classes defined by the loader
  if (field_name == vmSymbols::classLoader_name()) {
    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID loader_id = dump_fs.value().as_object_id;
    assert(loader_id == HeapDump::NULL_ID || _objects.contains(loader_id), "used loaders must already be recorded");
    if (loader_id != HeapDump::NULL_ID && _prepared_loaders.remove(loader_id)) { // If the loader is prepared
      const oop loader = obj->obj_field(obj_fs.offset());
      precond(java_lang_ClassLoader::is_instance(loader));
      const instanceHandle loader_h(Thread::current(), static_cast<instanceOop>(loader));
      // We use this fact to distinguish prepared loaders from the unprepared
      // ones when restoring them
      assert(java_lang_ClassLoader::unnamedModule(loader) != nullptr, "preparation must set the unnamed module");
      const HeapDump::InstanceDump &loader_dump = _heap_dump.get_instance_dump(loader_id);
      restore_identity_hash(loader_h(), loader_dump);
      restore_instance_fields(loader_h, loader_dump, CHECK_false);
    }
    return true;
  }

  // Incremented by the VM when the mirrored class is redefined, and it might
  // have been, so keep the new value
  if (field_name == vmSymbols::classRedefinedCount_name()) {
    assert(dump_fs.type() == T_INT, "must be");
    // TODO JVM TI's RedefineClasses support will require this to be revised
    guarantee(dump_fs.value().as_int == 0, "redefined classes are not dumped");
    return true;
  }

  // Mirrors of pre-defined classes may have some fields already set
  // TODO ...and also the mirrors may be accessed concurrently -- this may break
  //  something. We can get rid of this if we figure out how to pre-record
  //  all pre-existing objects and block other threads from creating new ones
  //  until the restoration completes.
  assert(is_reference_type(Signature::basic_type(obj_fs.signature())), "all primitives are handled above");
  const oop preexisting = obj->obj_field(obj_fs.offset());
  if (preexisting != nullptr) {
    precond(dump_fs.type() == T_OBJECT);
    Handle preexisting_h;
    if (preexisting->is_instance()) {            preexisting_h = instanceHandle(Thread::current(),  static_cast<instanceOop>(preexisting)); }
    else if (preexisting->is_objArray()) {       preexisting_h = objArrayHandle(Thread::current(),  static_cast<objArrayOop>(preexisting)); }
    else { precond(preexisting->is_typeArray()); preexisting_h = typeArrayHandle(Thread::current(), static_cast<typeArrayOop>(preexisting)); }
    put_object_if_absent(dump_fs.value().as_object_id, preexisting_h); // Also ensures there is no overwriting
    return true;
  }

  return false;
}

bool CracHeapRestorer::set_string_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                            const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  assert(obj_fs.field_descriptor().field_holder() == vmClasses::String_klass(), "must be");
  precond(!obj_fs.access_flags().is_static());

  // Flags are internal and depend on VM options. They will be set as needed,
  // so just ignore them.
  if (obj_fs.name() == vmSymbols::flags_name()) {
    return true;
  }

  // Interning is handled separately
  assert(obj_fs.name() != vmSymbols::is_interned_name(), "not a real field");

  return false;
}

bool CracHeapRestorer::set_member_name_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                                 const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  assert(obj_fs.field_descriptor().field_holder() == vmClasses::MemberName_klass(), "must be");
  precond(!obj_fs.access_flags().is_static());

  // VM-internal intptr_t field
  if (obj_fs.name() == vmSymbols::vmindex_name()) {
    const BasicType basic_type = dump_fs.type();
    guarantee(basic_type == T_INT || basic_type == T_LONG, "must be a Java equivalent of intptr_t");

    const HeapDump::BasicValue val = dump_fs.value();
    const auto vmindex = checked_cast<intptr_t>(basic_type == T_INT ? val.as_int : val.as_long);

    _member_name_dump_reader.ensure_initialized(_heap_dump, dump.class_id);
    if (_member_name_dump_reader.method(dump) != HeapDump::NULL_ID) {
      // vmindex is set to a vtable/itable index which is portable
      java_lang_invoke_MemberName::set_vmindex(obj(), vmindex);
    } else if (_member_name_dump_reader.is_field(dump) && _member_name_dump_reader.is_resolved(dump)) {
      // vmindex is set to a field offset which is not portable
      // TODOs:
      //  1. Checking is_resolved is not enough: checkpoint may be created after
      //     the resolution happens but before the indicator is set, so we may
      //     lose some of the resolved objects.
      //  2. Implement restoration when 'name' and/or 'type' fields are not set.

      const HeapDump::ID holder_id = _member_name_dump_reader.clazz(dump);
      guarantee(holder_id != HeapDump::NULL_ID, "holder of resolved field must be set");
      const InstanceKlass &holder = get_instance_class(holder_id);

      const HeapDump::ID name_id = _member_name_dump_reader.name(dump);
      if (name_id == HeapDump::NULL_ID) {
        log_error(crac)("Restoration of resolved field-referensing %s with 'name' not set is not implemented",
                        vmSymbols::java_lang_invoke_MemberName()->as_klass_external_name());
        Unimplemented();
      }
      const Handle name_str = restore_object(name_id, CHECK_false);
      const TempNewSymbol name = java_lang_String::as_symbol(name_str());

      const HeapDump::ID type_id = _member_name_dump_reader.type(dump);
      if (type_id == HeapDump::NULL_ID) {
        log_error(crac)("Restoration of resolved field-referensing %s with 'type' not set is not implemented",
                        vmSymbols::java_lang_invoke_MemberName()->as_klass_external_name());
        Unimplemented();
      }
      const Handle type_mirror = get_object_when_present(type_id); // Must be a non-void mirror, so should be pre-recorded
      TempNewSymbol signature;
      {
        Klass *k;
        const BasicType bt = java_lang_Class::as_BasicType(type_mirror(), &k);
        if (is_java_primitive(bt)) {
          signature = vmSymbols::type_signature(bt);
          signature->increment_refcount(); // TempNewSymbol will decrement this
        } else {
          ResourceMark rm;
          signature = SymbolTable::new_symbol(k->signature_name());
        }
      }

      fieldDescriptor fd;
      const bool found = holder.find_local_field(name, signature, &fd);
      guarantee(found, "cannot find field %s %s::%s resolved by %s " HDID_FORMAT,
                signature->as_C_string(), holder.external_name(), name->as_C_string(),
                vmSymbols::java_lang_invoke_MemberName()->as_klass_external_name(), dump.id);

      java_lang_invoke_MemberName::set_vmindex(obj(), fd.offset());
    } else {
      guarantee(vmindex == 0, "only set for resolved methods and fields");
    }

    return true;
  }

  return false;
}

bool CracHeapRestorer::set_call_site_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                               const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  precond(obj.not_null() && java_lang_invoke_CallSite::is_instance(obj()));
  precond(!obj_fs.access_flags().is_static());

  // CallSiteContext contains compilation-related data that should be cleared;
  // the context itself has a special deallocation policy and must be registered
  if (obj_fs.name() == vmSymbols::context_name()) {
    assert(obj_fs.field_descriptor().field_holder() == vmClasses::CallSite_klass(), "permitted subclasses don't have such field");

    precond(dump_fs.type() == T_OBJECT);
    const HeapDump::ID context_id = dump_fs.value().as_object_id;
    guarantee(context_id != HeapDump::NULL_ID, "class site must have a context");

    Handle context = get_object_if_present(context_id);
    if (context.is_null()) { // ID is not null so this means the context has not yet been restored
      const HeapDump::InstanceDump &context_dump = _heap_dump.get_instance_dump(context_id);

      InstanceKlass &context_class = get_instance_class(context_dump.class_id);
      assert(context_class.name() == vmSymbols::java_lang_invoke_MethodHandleNatives_CallSiteContext() &&
            context_class.class_loader_data()->is_the_null_class_loader_data(), "expected boot-loaded %s, got %s loaded by %s",
            vmSymbols::java_lang_invoke_MethodHandleNatives_CallSiteContext()->as_klass_external_name(),
            context_class.external_name(), context_class.class_loader_data()->loader_name_and_id());

      // Allocate a new context and register it with this call site
      // If this'll be failing, restore CallSiteContext before the rest of the classes
      guarantee(context_class.is_initialized(), "no need to pre-initialize %s", context_class.external_name());
      JavaValue result(T_OBJECT);
      const TempNewSymbol make_name = SymbolTable::new_symbol("make");
      const TempNewSymbol make_sig = SymbolTable::new_symbol("(Ljava/lang/invoke/CallSite;)Ljava/lang/invoke/MethodHandleNatives$CallSiteContext;");
      JavaCalls::call_static(&result, &context_class, make_name, make_sig, obj, CHECK_false);

      context = instanceHandle(Thread::current(), static_cast<instanceOop>(result.get_oop()));
      put_object_when_absent(context_id, context); // Should still be absent
    } else {
      DEBUG_ONLY(const DependencyContext vmcontext = java_lang_invoke_MethodHandleNatives_CallSiteContext::vmdependencies(context()));
      assert(vmcontext.is_unused(), "must be");
      // TODO register the context with this call site (CallSiteContext::make()
      //  does this for us in the above case)
    }

    obj->obj_field_put(obj_fs.offset(), context());
    return true;
  }

  return false;
}

bool CracHeapRestorer::set_call_site_context_instance_field_if_special(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                                       const FieldStream &obj_fs, const DumpedInstanceFieldStream &dump_fs, TRAPS) {
  precond(obj.not_null() && java_lang_invoke_MethodHandleNatives_CallSiteContext::is_instance(obj()));
  precond(!obj_fs.access_flags().is_static());

  // CallSiteContext contains compilation-related data that should be cleared
  assert(obj_fs.field_descriptor().field_flags().is_injected(), "all %s fields are injected",
         vmSymbols::java_lang_invoke_MethodHandleNatives_CallSiteContext()->as_klass_external_name());
#ifdef ASSERT
  switch (Signature::basic_type(obj_fs.signature())) {
    case T_INT:  assert(obj->int_field(obj_fs.offset()) == 0, "must be cleared when allocated");  break;
    case T_LONG: assert(obj->long_field(obj_fs.offset()) == 0, "must be cleared when allocated"); break;
    default: ShouldNotReachHere();
  }
#endif // ASSERT

  return true;
}

void CracHeapRestorer::restore_special_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump,
                                                       set_instance_field_if_special_ptr_t set_field_if_special, TRAPS) {
  precond(obj.not_null());
  FieldStream obj_fs(InstanceKlass::cast(obj->klass()), false,  // Include supers
                                                        true,   // Exclude interfaces: they only have static fields
                                                        false); // Include injected fields
  DumpedInstanceFieldStream dump_fs(_heap_dump, dump);
  while (!obj_fs.eos() && !dump_fs.eos()) {
    if (obj_fs.access_flags().is_static()) {
      obj_fs.next();
      continue;
    }

    assert(obj_fs.name() == dump_fs.name(),
           "conflict at field #%i of object " HDID_FORMAT " : dumped '%s' is '%s' in the runtime",
           obj_fs.index(), dump.id, dump_fs.name()->as_C_string(), obj_fs.name()->as_C_string());
    assert(is_same_basic_type(obj_fs.signature(), dump_fs.type(), true),
           "conflict at field #%i of object " HDID_FORMAT ": cannot assign dumped '%s' value to a '%s' field",
           obj_fs.index(), dump.id, type2name(dump_fs.type()), obj_fs.signature()->as_C_string());
    if (log_is_enabled(Trace, crac)) {
      ResourceMark rm;
      log_trace(crac)("Restoring " HDID_FORMAT ": potentially-special instance field %s", dump.id, obj_fs.name()->as_C_string());
    }

    const HeapDump::BasicValue val = dump_fs.value();
    const bool is_special = (this->*set_field_if_special)(obj, dump, obj_fs, dump_fs, CHECK);
    if (!is_special) {
      set_field(obj, obj_fs, val, CHECK);
    }

    obj_fs.next();
    dump_fs.next();
  }

#ifdef ASSERT
  u4 unfilled_bytes = 0;
  for (; !obj_fs.eos(); obj_fs.next()) {
    if (!obj_fs.access_flags().is_static()) {
      const BasicType type = Signature::basic_type(obj_fs.signature());
      unfilled_bytes += HeapDump::value_size(type, _heap_dump.id_size);
    }
  }
  assert(unfilled_bytes == 0,
         "object " HDID_FORMAT " has less non-static fields' data dumped than needed by its class %s and its super classes: "
         "only " UINT32_FORMAT " bytes dumped, but additional " UINT32_FORMAT " bytes are expected",
         dump.id, obj->klass()->external_name(), dump.fields_data.size(), unfilled_bytes);
  if (java_lang_String::is_instance(obj())) {
    // There is a fake is_interned field in j.l.String instance dumps
    assert(!dump_fs.eos(), "%s field missing from %s instance dump " HDID_FORMAT,
           vmSymbols::is_interned_name()->as_C_string(), vmSymbols::java_lang_String()->as_klass_external_name(), dump.id);
    assert(dump_fs.name() == vmSymbols::is_interned_name(), "unexpected field %s in %s instance dump " HDID_FORMAT,
           dump_fs.name()->as_C_string(), vmSymbols::java_lang_String()->as_klass_external_name(), dump.id);
    dump_fs.next();
  }
  assert(dump_fs.eos(),
         "object " HDID_FORMAT " has more non-static fields' data dumped than needed by its class %s and its super classes",
         dump.id, obj->klass()->external_name());
#endif // ASSERT
}

// This is faster than the special case above as it does not require querying
// dumps of all classes (direct and super) of the instance.
void CracHeapRestorer::restore_ordinary_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump, TRAPS) {
  precond(obj.not_null());
  FieldStream fs(InstanceKlass::cast(obj->klass()), false,  // Include supers
                                                    true,   // Exclude interfaces: they only have static fields
                                                    false); // Include injected fields
  u4 dump_offset = 0;
  for (; !fs.eos() && dump_offset < dump.fields_data.size(); fs.next()) {
    if (fs.access_flags().is_static()) {
      continue;
    }
    if (log_is_enabled(Trace, crac)) {
      ResourceMark rm;
      log_trace(crac)("Restoring " HDID_FORMAT ": ordinary instance field %s", dump.id, fs.name()->as_C_string());
    }

    const BasicType type = Signature::basic_type(fs.signature());
    const u4 type_size = HeapDump::value_size(type, _heap_dump.id_size);
    guarantee(dump_offset + type_size <= dump.fields_data.size(),
              "object " HDID_FORMAT " has less non-static fields' data dumped than needed by its class %s and its super classes: "
              "read " UINT32_FORMAT " bytes and expect at least " UINT32_FORMAT " more to read %s value, but only " UINT32_FORMAT " bytes left",
              dump.id, obj->klass()->external_name(), dump_offset, type_size, type2name(type), dump.fields_data.size() - dump_offset);
    const HeapDump::BasicValue val = dump.read_field(dump_offset, type, _heap_dump.id_size);
    set_field(obj, fs, val, CHECK);

    dump_offset += type_size;
  }

#ifdef ASSERT
  u4 unfilled_bytes = 0;
  for (; !fs.eos(); fs.next()) {
    if (!fs.access_flags().is_static()) {
      const BasicType type = Signature::basic_type(fs.signature());
      unfilled_bytes += HeapDump::value_size(type, _heap_dump.id_size);
    }
  }
  assert(unfilled_bytes == 0,
         "object " HDID_FORMAT " has less non-static fields' data dumped than needed by its class %s and its super classes: "
         "only " UINT32_FORMAT " bytes dumped, but additional " UINT32_FORMAT " bytes are expected",
         dump.id, obj->klass()->external_name(), dump.fields_data.size(), unfilled_bytes);
  assert(dump_offset == dump.fields_data.size(),
         "object " HDID_FORMAT " has more non-static fields' data dumped than needed by its class %s and its super classes: "
         UINT32_FORMAT " bytes dumped, but only " UINT32_FORMAT " expected",
         dump.id, obj->klass()->external_name(), dump.fields_data.size(), dump_offset);
#endif // ASSERT
}

void CracHeapRestorer::restore_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump, TRAPS) {
  // ResolvedMethodName is restored in a special manner as a whole
  assert(!java_lang_invoke_ResolvedMethodName::is_instance(obj()), "should not be manually restoring fields of this instance");

  if (obj->klass()->is_class_loader_instance_klass()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_class_loader_instance_field_if_special, CHECK);
  } else if (obj->klass()->is_mirror_instance_klass()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_class_mirror_instance_field_if_special, CHECK);
  } else if (obj->klass() == vmClasses::String_klass()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_string_instance_field_if_special, CHECK);
  } else if (obj->klass() == vmClasses::MemberName_klass()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_member_name_instance_field_if_special, CHECK);
  } else if (obj->klass() == vmClasses::CallSite_klass() || obj->klass()->super() == vmClasses::CallSite_klass()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_call_site_instance_field_if_special, CHECK);
  } else if (obj->klass()->class_loader_data()->is_the_null_class_loader_data() &&
             obj->klass()->name() == vmSymbols::java_lang_invoke_MethodHandleNatives_CallSiteContext()) {
    restore_special_instance_fields(obj, dump, &CracHeapRestorer::set_call_site_context_instance_field_if_special, CHECK);
  } else { // TODO other special cases (need to check all classes from javaClasses)
    precond(!java_lang_invoke_CallSite::is_instance(obj()));
    restore_ordinary_instance_fields(obj, dump, CHECK);
  }
}

bool CracHeapRestorer::set_static_field_if_special(instanceHandle mirror, const FieldStream &fs, const HeapDump::BasicValue &val, TRAPS) {
  precond(fs.access_flags().is_static());

  // Array classes don't have static fields
  const InstanceKlass *ik = InstanceKlass::cast(java_lang_Class::as_Klass(mirror()));

  // j.l.r.SoftReference::clock is set by the GC (notably, it is done even
  // before the class is initialized)
  if (ik == vmClasses::SoftReference_klass() && fs.name()->equals("clock")) {
    return true;
  }

  // jdk.crac.Core is the only pre-initialized class we restore and thus
  // overwrite its pre-filled fields which is not expected in the general path
  if (is_jdk_crac_Core(*ik)) {
    precond(ik->is_initialized());
    const Symbol *field_name = fs.name();
    const BasicType field_type = Signature::basic_type(fs.signature());
    if (field_type == T_OBJECT) {
      assert(field_name->equals("globalContext") || field_name->equals("checkpointRestoreLock"), "must be");
      guarantee(val.as_object_id != HeapDump::NULL_ID, "global context and C/R lock must exist");
      const Handle restored = restore_object(val.as_object_id, CHECK_false);
      mirror->obj_field_put(fs.offset(), restored());
    } else if (field_name->equals("checkpointInProgress")) {
      assert(field_type == T_BOOLEAN, "must be");
      guarantee(val.as_boolean, "no checkpoint was in progress?!");
      mirror->bool_field_put(fs.offset(), checked_cast<jboolean>(true));
    } else {
      // Should be a static final primitive already set to the same value
#ifdef ASSERT
      switch (field_type) {
        case T_BOOLEAN: assert(mirror->bool_field(fs.offset()) == val.as_boolean, "must be"); break;
        case T_INT:     assert(mirror->int_field(fs.offset()) == val.as_int, "must be");      break;
        case T_LONG:    assert(mirror->long_field(fs.offset()) == val.as_long, "must be");    break;
        default: ShouldNotReachHere();
      }
#endif // ASSERT
    }
    return true;
  }

  // TODO other special cases (need to check all classes from javaClasses)
  return false;
}

static void set_resolved_references(InstanceKlass *ik, Handle resolved_refs) {
  // If resolved references are dumped, they should not be null
  guarantee(ik->is_rewritten(), "class %s cannot have resolved references because it has not been rewritten",
            ik->external_name());
  guarantee(resolved_refs.not_null(), "rewritten class %s has null resolved references dumped",
            ik->external_name());
  guarantee(resolved_refs->klass()->is_objArray_klass() &&
            ObjArrayKlass::cast(resolved_refs->klass())->element_klass() == vmClasses::Object_klass(),
            "class %s has resolved references of illegal type", ik->external_name());

  assert(ik->constants()->cache() != nullptr, "rewritten class must have a CP cache");
  if (ik->constants()->resolved_references() == nullptr) {
    ik->constants()->cache()->set_resolved_references(ik->class_loader_data()->add_handle(resolved_refs));
    return;
  }

  for (InstanceKlass *prev_ver = ik->previous_versions(); prev_ver != nullptr; prev_ver = prev_ver->previous_versions()) {
    guarantee(prev_ver->is_rewritten(), "there are more resolved references dumped for %s than expected",
              prev_ver->external_name());
    assert(prev_ver->constants()->cache() != nullptr, "rewritten class must have a CP cache");
    if (prev_ver->constants()->resolved_references() == nullptr) {
      prev_ver->constants()->cache()->set_resolved_references(prev_ver->class_loader_data()->add_handle(resolved_refs));
      return;
    }
  }

  guarantee(false, "there are more resolved references dumped for %s than expected", ik->external_name());
  ShouldNotReachHere();
}

void CracHeapRestorer::restore_static_fields(InstanceKlass *ik, const HeapDump::ClassDump &dump, TRAPS) {
  instanceHandle mirror(Thread::current(), static_cast<instanceOop>(ik->java_mirror()));

  FieldStream fs(ik, true,  // Only fields declared in this class/interface directly
                     true,  // This doesn't metter when the above is true
                     true); // Exclude injected fields: ther are always non-static
  u2 static_i = 0;
  while (!fs.eos() && static_i < dump.static_fields.size()) {
    if (!fs.access_flags().is_static()) {
      fs.next();
      continue;
    }
    if (log_is_enabled(Trace, crac)) {
      ResourceMark rm;
      log_trace(crac)("Restoring " HDID_FORMAT ": static field %s", dump.id, fs.name()->as_C_string());
    }

    const HeapDump::ClassDump::Field &field = dump.static_fields[static_i++];
    const Symbol *field_name = _heap_dump.get_symbol(field.info.name_id);
    guarantee(field_name != vmSymbols::resolved_references_name(),
              "class %s (ID " HDID_FORMAT ") has resolved references dumped before some of the actual static fields",
              ik->external_name(), dump.id);

    assert(fs.name() == field_name && is_same_basic_type(fs.signature(), HeapDump::htype2btype(field.info.type)),
           "expected static field #%i of class %s (ID " HDID_FORMAT ") to be %s %s but it is %s %s in the dump",
           static_i, ik->external_name(), dump.id,
           type2name(Signature::basic_type(fs.signature())), fs.name()->as_C_string(),
           type2name(HeapDump::htype2btype(field.info.type)), field_name->as_C_string());
    const bool is_special = set_static_field_if_special(mirror, fs, field.value, CHECK);
    if (!is_special) {
      set_field(mirror, fs, field.value, CHECK);
    }

    fs.next();
  }

#ifdef ASSERT
  u2 unfilled_fields_num = 0;
  for (; !fs.eos(); fs.next()) {
    if (fs.access_flags().is_static()) {
      unfilled_fields_num++;
    }
  }
  assert(unfilled_fields_num == 0, "class %s (ID " HDID_FORMAT ") has not enough static fields dumped: expected %i more",
         ik->external_name(), dump.id, unfilled_fields_num);

  { // HeapDumper includes constant pool's resolved references as static fields
    const AutoSaveRestore<u2> save_restore_static_i(static_i);
    while (static_i < dump.static_fields.size()) {
      const HeapDump::ClassDump::Field &field = dump.static_fields[static_i++];
      const Symbol *field_name = _heap_dump.get_symbol(field.info.name_id);
      assert(field_name == vmSymbols::resolved_references_name(),
             "class %s (ID " HDID_FORMAT ") has excess static field dumped: %s",
             ik->external_name(), dump.id, field_name->as_C_string());
    }
  }
#endif // ASSERT

  // Restore resolved references if they are not pre-created
  if (ik->is_linked() /*pre-linked*/ || (ik->is_rewritten() && ik->is_shared()) /*pre-rewritten*/) {
    return;
  }
  while (static_i < dump.static_fields.size()) {
    log_trace(crac)("Restoring " HDID_FORMAT ": resolved references (pseudo static field #%i)", dump.id, static_i);
    const HeapDump::ClassDump::Field &field = dump.static_fields[static_i++];
    guarantee(field.info.type == HPROF_NORMAL_OBJECT, "resolved references dumped as %s: static field #%i of %s (ID " HDID_FORMAT ")",
              type2name(HeapDump::htype2btype(field.info.type)), (static_i - 1), ik->external_name(), dump.id);
    const Handle restored = restore_object(field.value.as_object_id, CHECK);
    set_resolved_references(ik, restored);
  }
}

// Object restoration

void CracHeapRestorer::restore_class_mirror(HeapDump::ID id, TRAPS) {
  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    const char *type_name;
    const HeapDump::LoadClass *lc = _heap_dump.load_classes.get(id);
    if (lc != nullptr) {
      type_name = _heap_dump.get_symbol(lc->class_name_id)->as_klass_external_name();
    } else {
      type_name = "a primitive type";
    }
    log_trace(crac)("Restoring mirror " HDID_FORMAT " of %s", id, type_name);
  }

  // Instance mirrors must be pre-recorded
  instanceHandle mirror;
  {
    Handle mirror_h = get_object_when_present(id);
    assert(mirror_h->is_instance(), "mirrors are instances");
    mirror = *static_cast<instanceHandle *>(&mirror_h);
  }

  // Side-effect: finishes restoration of the class loader if only prepared
  const HeapDump::InstanceDump &mirror_dump = _heap_dump.get_instance_dump(id);
  restore_identity_hash(mirror(), mirror_dump);
  restore_instance_fields(mirror, mirror_dump, CHECK);

  Klass *const mirrored_k = java_lang_Class::as_Klass(mirror());
  if (mirrored_k != nullptr && mirrored_k->is_instance_klass()) {
    const HeapDump::ClassDump &dump = _heap_dump.get_class_dump(id);
    // Side-effect: restores resolved references array of the constant pool
    restore_static_fields(InstanceKlass::cast(mirrored_k), dump, CHECK);
  }

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    const char *type_name;
    if (mirrored_k != nullptr) {
      type_name = mirrored_k->external_name();
    } else {
      type_name = type2name(java_lang_Class::as_BasicType(mirror()));
    }
    log_trace(crac)("Restored mirror " HDID_FORMAT " of %s", id, type_name);
  }
}

Handle CracHeapRestorer::restore_object(HeapDump::ID id, TRAPS) {
  if (id == HeapDump::NULL_ID) {
    return {};
  }
  const Handle *ready = _objects.get(id);
  if (ready != nullptr) {
    return *ready;
  }

  const HeapDump::InstanceDump *instance_dump = _heap_dump.instance_dumps.get(id);
  if (instance_dump != nullptr) {
    assert(!_instance_classes.contains(id) && !_array_classes.contains(id), "unrecorded class mirror " HDID_FORMAT, id);
    assert(!_heap_dump.obj_array_dumps.contains(id) && !_heap_dump.prim_array_dumps.contains(id),
           "object " HDID_FORMAT " duplicated in multiple dump categories: instance and some kind of array", id);
    return restore_instance(*instance_dump, CHECK_NH);
  }

  const HeapDump::ObjArrayDump  *obj_array_dump  = _heap_dump.obj_array_dumps.get(id);
  if (obj_array_dump != nullptr) {
    assert(!_heap_dump.prim_array_dumps.contains(id),
           "object " HDID_FORMAT " duplicated in multiple dump categories: object and primitive array", id);
    return restore_obj_array(*obj_array_dump, CHECK_NH);
  }

  const HeapDump::PrimArrayDump *prim_array_dump = _heap_dump.prim_array_dumps.get(id);
  guarantee(prim_array_dump != nullptr, "object " HDID_FORMAT " not found in the heap dump", id);
  return restore_prim_array(*prim_array_dump, CHECK_NH);
}

// Void mirror is the only class mirror that we don't pre-record.
instanceHandle CracHeapRestorer::get_void_mirror(const HeapDump::InstanceDump &dump) {
  _mirror_dump_reader.ensure_initialized(_heap_dump, dump.class_id); // Also checks this is a mirror dump
  guarantee(_mirror_dump_reader.mirrors_void(dump), "unrecorded non-void class mirror " HDID_FORMAT, dump.id);
  const instanceHandle mirror(Thread::current(), static_cast<instanceOop>(Universe::void_mirror()));
  return mirror;
}

// Strings may be interned.
instanceHandle CracHeapRestorer::get_string(const HeapDump::InstanceDump &dump, TRAPS) {
  const instanceHandle str = vmClasses::String_klass()->allocate_instance_handle(CHECK_({}));
  // String's fields don't reference it back so it's safe to restore them before recording the string
  restore_instance_fields(str, dump, CHECK_({}));

  _string_dump_reader.ensure_initialized(_heap_dump, dump.class_id);
  if (_string_dump_reader.is_interned(dump)) {
    const oop interned = StringTable::intern(str(), CHECK_({}));
    return {Thread::current(), static_cast<instanceOop>(interned)};
  }

  // Identity hash is to be restored by the caller
  return str;
}

// ResolvedMethodNames are interned by the VM.
instanceHandle CracHeapRestorer::get_resolved_method_name(const HeapDump::InstanceDump &dump, TRAPS) {
  _resolved_method_name_dump_reader.ensure_initialized(_heap_dump, dump.class_id);

  const HeapDump::ID holder_id = _resolved_method_name_dump_reader.vmholder(dump);
  InstanceKlass *const holder = &get_instance_class(holder_id);

  const HeapDump::ID name_id = _resolved_method_name_dump_reader.method_name_id(dump);
  Symbol *const name = _heap_dump.get_symbol(name_id);

  const HeapDump::ID sig_id = _resolved_method_name_dump_reader.method_signature_id(dump);
  Symbol *const sig = _heap_dump.get_symbol(sig_id);

  const jbyte kind_raw = _resolved_method_name_dump_reader.method_kind(dump);
  guarantee(MethodKind::is_method_kind(kind_raw), "illegal resolved method kind: %i", kind_raw);
  const auto kind = static_cast<MethodKind::Enum>(kind_raw);

  Method *const m = CracClassDumpParser::find_method(holder, name, sig, kind, true, CHECK_({}));
  const methodHandle resolved_method(Thread::current(), m);

  // If this'll be failing, restore ResolvedMethodName before the rest of the classes
  guarantee(vmClasses::ResolvedMethodName_klass()->is_initialized(), "need to pre-initialize %s",
            vmSymbols::java_lang_invoke_ResolvedMethodName()->as_klass_external_name());
  const oop method_name_o = java_lang_invoke_ResolvedMethodName::find_resolved_method(resolved_method, CHECK_({}));

  return {Thread::current(), static_cast<instanceOop>(method_name_o)};
}

// MethodTypes are interned on the Java side.
instanceHandle CracHeapRestorer::get_method_type(const HeapDump::InstanceDump &dump, TRAPS) {
  // TODO this check is actually not enough and we can get a deadlock when
  //  calling into Java below if that method has not been called before the
  //  restoration began (this really can happen, I've been a witness...)
  assert(vmClasses::MethodType_klass()->is_initialized(), "no need for this if no cache is pre-initialized");

  _method_type_dump_reader.ensure_initialized(_heap_dump, dump.class_id);
  const HeapDump::ID rtype_id = _method_type_dump_reader.rtype(dump);
  const HeapDump::ID ptypes_id = _method_type_dump_reader.ptypes(dump);

  // These are class mirrors so it's safe to restore them before recording the MethodType
  const Handle rtype = restore_object(rtype_id, CHECK_({})); // Can be a void mirror so must restore
  const Handle ptypes = restore_object(ptypes_id, CHECK_({}));

  JavaValue res(T_OBJECT);
  const TempNewSymbol name = SymbolTable::new_symbol("methodType");
  const TempNewSymbol sig = SymbolTable::new_symbol("(Ljava/lang/Class;[Ljava/lang/Class;Z)Ljava/lang/invoke/MethodType;");
  JavaCallArguments args;
  args.push_oop(rtype);
  args.push_oop(ptypes);
  args.push_int(static_cast<jboolean>(true)); // trusted
  JavaCalls::call_static(&res, vmClasses::MethodType_klass(), name, sig, &args, CHECK_({}));

  const instanceHandle mt(Thread::current(), static_cast<instanceOop>(res.get_oop()));
  guarantee(mt.not_null() && mt->is_instance(), "must be");

  // The interned MethodType can have some fields already set, need to synchronize
  assert(rtype == java_lang_invoke_MethodType::rtype(mt()), "there can only be one mirror of a class");
  if (ptypes != java_lang_invoke_MethodType::ptypes(mt())) {
    const Handle actual_ptypes(Thread::current(), java_lang_invoke_MethodType::ptypes(mt()));
    _objects.put(ptypes_id, actual_ptypes);
  }
  // TODO restore/record the rest of the fields

  return mt;
}

instanceHandle CracHeapRestorer::restore_instance(const HeapDump::InstanceDump &dump, TRAPS) {
  assert(!_objects.contains(dump.id), "use restore_object() instead");
  log_trace(crac)("Restoring instance " HDID_FORMAT, dump.id);

  InstanceKlass &ik = get_instance_class(dump.class_id);
  guarantee(ik.is_being_restored() || ik.is_initialized(),
            "object " HDID_FORMAT " is an instance of pre-defined uninitialized class %s (" HDID_FORMAT ")",
            dump.id, ik.external_name(), dump.class_id);

  instanceHandle obj;
  if (ik.is_mirror_instance_klass()) {
    // This must be the void mirror because every other one is pre-recorded
    obj = get_void_mirror(dump);
    record_class_mirror(obj, dump, CHECK_({}));
  } else {
    NOT_PRODUCT(ik.check_valid_for_instantiation(true, CHECK_({})));
    bool generic_class = false;
    if (&ik == vmClasses::String_klass()) {
      obj = get_string(dump, CHECK_({}));
    } else if (&ik == vmClasses::ResolvedMethodName_klass()) {
      obj = get_resolved_method_name(dump, CHECK_({}));
    } else if (&ik == vmClasses::MethodType_klass() && ik.is_initialized()) {
      obj = get_method_type(dump, CHECK_({}));
    } else {
      obj = ik.allocate_instance_handle(CHECK_({}));
      generic_class = true;
    }
    put_object_when_absent(dump.id, obj);
    restore_identity_hash(obj(), dump);
    if (generic_class) { // Special cases get their fields restored above
      restore_instance_fields(obj, dump, CHECK_({}));
    }
  }

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Restored instance " HDID_FORMAT " of %s", dump.id, ik.external_name());
  }
  return obj;
}

objArrayHandle CracHeapRestorer::restore_obj_array(const HeapDump::ObjArrayDump &dump, TRAPS) {
  assert(!_objects.contains(dump.id), "use restore_object() instead");
  log_trace(crac)("Restoring object array " HDID_FORMAT, dump.id);

  ObjArrayKlass *oak;
  {
    ArrayKlass &ak = get_array_class(dump.array_class_id);
    guarantee(ak.is_objArray_klass(), "object array " HDID_FORMAT " has a primitive array class", dump.id);
    oak = ObjArrayKlass::cast(&ak);
  }

  guarantee(dump.elem_ids.size() <= INT_MAX, "object array " HDID_FORMAT " is too long: "
            UINT32_FORMAT " > %i", dump.id, dump.elem_ids.size(), INT_MAX);
  const int length = checked_cast<int>(dump.elem_ids.size());

  objArrayHandle array;
  {
    const objArrayOop o = oak->allocate(length, CHECK_({}));
    array = objArrayHandle(Thread::current(), o);
  }
  put_object_when_absent(dump.id, array); // Record first to be able to find in case of circular references

  restore_identity_hash(array(), dump);

  for (int i = 0; i < length; i++) {
    const Handle elem = restore_object(dump.elem_ids[i], CHECK_({}));
    assert(elem == nullptr || elem->klass()->is_subtype_of(oak->element_klass()),
           "object array " HDID_FORMAT " is expected to have elements of type %s, "
           "but its element #%i has class %s which is not a subtype of the element type",
           dump.id, oak->element_klass()->external_name(), i, elem->klass()->external_name());
    array->obj_at_put(i, elem());
  }

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Restored object array " HDID_FORMAT " of %s", dump.id, oak->external_name());
  }
  return array;
}

typeArrayHandle CracHeapRestorer::restore_prim_array(const HeapDump::PrimArrayDump &dump, TRAPS) {
  assert(!_objects.contains(dump.id), "use restore_object() instead");
  log_trace(crac)("Restoring primitive array " HDID_FORMAT, dump.id);

  guarantee(dump.elems_num <= INT_MAX, "primitive array " HDID_FORMAT " is too long: "
            UINT32_FORMAT " > %i", dump.id, dump.elems_num, INT_MAX);
  const int length = checked_cast<int>(dump.elems_num);
  const BasicType elem_type = HeapDump::htype2btype(dump.elem_type);

  const typeArrayOop array = oopFactory::new_typeArray_nozero(elem_type, length, CHECK_({}));
  restore_identity_hash(array, dump);
  precond(static_cast<size_t>(length) * type2aelembytes(elem_type) == dump.elems_data.size());
  if (length > 0) {
    memcpy(array->base(elem_type), dump.elems_data.mem(), dump.elems_data.size());
  }

  const typeArrayHandle array_h(Thread::current(), array);
  put_object_when_absent(dump.id, array_h);

  if (log_is_enabled(Trace, crac)) {
    ResourceMark rm;
    log_trace(crac)("Restored primitive array " HDID_FORMAT " of %s", dump.id, array->klass()->external_name());
  }
  return array_h;
}
