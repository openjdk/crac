#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
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
#include "oops/oopsHierarchy.hpp"
#include "oops/symbol.hpp"
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/cracHeapRestorer.hpp"
#include "runtime/cracStackDumpParser.hpp"
#include "runtime/handles.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/reflectionUtils.hpp"
#include "runtime/signature.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpClasses.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/hprofTag.hpp"
#include "utilities/macros.hpp"

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

static oop get_builtin_system_loader() {
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
  return loader;
}

void WellKnownObjects::put_into(HeapDumpTable<jobject, AnyObj::C_HEAP> *objects) const {
  precond(objects->number_of_entries() == 0);
  Thread *const thread = JavaThread::current();
  if (_platform_loader_id != HeapDump::NULL_ID) {
    const oop loader = SystemDictionary::java_platform_loader();
    if (loader != nullptr) {
      guarantee(loader->klass() == vmClasses::jdk_internal_loader_ClassLoaders_PlatformClassLoader_klass(), "sanity check");
      const jobject loader_h = JNIHandles::make_global(Handle(thread, loader));
      objects->put_when_absent(_platform_loader_id, loader_h);
    }
  }
  if (_builtin_system_loader_id != HeapDump::NULL_ID) {
    const oop loader = get_builtin_system_loader();
    if (loader != nullptr) {
      const jobject loader_h = JNIHandles::make_global(Handle(thread, loader));
      objects->put_when_absent(_builtin_system_loader_id, loader_h);
    }
  }
  if (_actual_system_loader_id != HeapDump::NULL_ID && _actual_system_loader_id != _builtin_system_loader_id) {
    const oop loader = SystemDictionary::java_system_loader();
    if (loader != nullptr) {
      const jobject loader_h = JNIHandles::make_global(Handle(thread, loader));
      objects->put_when_absent(_builtin_system_loader_id, loader_h);
    }
  }
  objects->maybe_grow();
}

void WellKnownObjects::get_from(const HeapDumpTable<jobject, AnyObj::C_HEAP> &objects) const {
  if (_platform_loader_id != HeapDump::NULL_ID) {
    const jobject *restored = objects.get(_platform_loader_id);
    if (restored != nullptr) {
      const oop existing = SystemDictionary::java_platform_loader();
      if (existing != nullptr) {
        guarantee(JNIHandles::resolve_non_null(*restored) == existing,
                  "restored platform loader must be the existing one");
      } else {
        log_error(crac)("Restoration of base class loaders is not implemented");
        Unimplemented();
      }
    }
  }
  if (_builtin_system_loader_id != HeapDump::NULL_ID) {
    const jobject *restored = objects.get(_builtin_system_loader_id);
    if (restored != nullptr) {
      const oop existing = get_builtin_system_loader();
      if (existing != nullptr) {
        guarantee(JNIHandles::resolve_non_null(*restored) == existing,
                  "restored builtin system loader must be the existing one");
      } else {
        log_error(crac)("Restoration of base class loaders is not implemented");
        Unimplemented();
      }
    }
  }
  if (_actual_system_loader_id != HeapDump::NULL_ID) {
    const jobject *restored = objects.get(_actual_system_loader_id);
    if (restored != nullptr) {
      const oop existing = SystemDictionary::java_system_loader();
      if (existing != nullptr) {
        guarantee(JNIHandles::resolve_non_null(*restored) == existing,
                  "restored actual system loader must be the existing one");
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

oop CracHeapRestorer::get_object_when_present(HeapDump::ID id) const {
  assert(id != HeapDump::NULL_ID, "nulls are not recorded");
  assert(_objects.contains(id), "object " HDID_FORMAT " was expected to be recorded", id);
  return JNIHandles::resolve_non_null(*_objects.get(id));
}

oop CracHeapRestorer::get_object_if_present(HeapDump::ID id) const {
  assert(id != HeapDump::NULL_ID, "nulls are not recorded");
  const jobject *obj_h = _objects.get(id);
  return obj_h != nullptr ? JNIHandles::resolve_non_null(*obj_h) : nullptr;
}

jobject CracHeapRestorer::put_object_when_absent(HeapDump::ID id, Handle obj) {
  assert(id != HeapDump::NULL_ID && obj.not_null(), "nulls should not be recorded");
  assert(!_objects.contains(id), "object " HDID_FORMAT " was expected to be absent", id);
  const jobject h = JNIHandles::make_global(obj);
  _objects.put_when_absent(id, h);
  _objects.maybe_grow();
  return h;
}

jobject CracHeapRestorer::put_object_when_absent(HeapDump::ID id, oop obj) {
  return put_object_when_absent(id, Handle(Thread::current(), obj));
}

jobject CracHeapRestorer::put_object_if_absent(HeapDump::ID id, Handle obj) {
  assert(id != HeapDump::NULL_ID && obj.not_null(), "nulls should not be recorded");
  bool is_absent;
  jobject *const node = _objects.put_if_absent(id, &is_absent);
  if (is_absent) {
    // Only allocate JNI handle if will record: it must be freed
    *node = JNIHandles::make_global(obj);
    _objects.maybe_grow();
  } else {
    assert(JNIHandles::resolve(*node) == obj(), "two different objects restored for ID " HDID_FORMAT, id);
  }
  return *node;
}

jobject CracHeapRestorer::put_object_if_absent(HeapDump::ID id, oop obj) {
  assert(id != HeapDump::NULL_ID && obj != nullptr, "nulls should not be recorded");
  bool is_absent;
  jobject *const node = _objects.put_if_absent(id, &is_absent);
  if (is_absent) {
    // Only allocate JNI handle if will record: it must be freed, also don't
    // create a usual handle if it won't be used
    *node = JNIHandles::make_global(Handle(Thread::current(), obj));
    _objects.maybe_grow();
  } else {
    guarantee(JNIHandles::resolve(*node) == obj, "two different objects restored for ID " HDID_FORMAT, id);
  }
  return *node;
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
    const oop loader = get_object_when_present(loader_dump.id);
    assert(java_lang_Module::loader(unnamed_module()) == loader,
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
  const oop existing_map = get_object_if_present(map_id);
  if (existing_map == nullptr) {
    const instanceHandle map = vmClasses::ConcurrentHashMap_klass()->allocate_instance_handle(CHECK_({}));
    put_object_when_absent(map_id, map);
    return map;
  }
  guarantee(existing_map->klass() == vmClasses::ConcurrentHashMap_klass(),
            "class loader " HDID_FORMAT " has its 'parallelLockMap' field referencing a %s but it must reference a %s",
            loader_dump.id, existing_map->klass()->external_name(), vmClasses::ConcurrentHashMap_klass()->external_name());
  return {Thread::current(), static_cast<instanceOop>(existing_map)};
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

  const oop existing_loader = get_object_if_present(id);
  if (existing_loader != nullptr) {
    guarantee(existing_loader->klass()->is_class_loader_instance_klass(),
              "object " HDID_FORMAT " is not a class loader: its class %s does not subclass %s",
              id, existing_loader->klass()->external_name(), vmSymbols::java_lang_ClassLoader()->as_klass_external_name());
    return {Thread::current(), static_cast<instanceOop>(existing_loader)};
  }

  precond(!_prepared_loaders.contains(id));
  const instanceHandle loader = prepare_class_loader(id, CHECK_({})); // Allocate and partially restore the loader
  postcond(_prepared_loaders.contains(id) && get_object_when_present(id) == loader());
  guarantee(loader.not_null() && loader->klass()->is_class_loader_instance_klass(), "must be a class loader");

  return loader;
}

// Heap restoration driver

void CracHeapRestorer::restore_heap(const HeapDumpTable<UnfilledClassInfo, AnyObj::C_HEAP> &class_infos,
                                    const GrowableArrayView<StackTrace *> &stack_traces, TRAPS) {
  log_info(crac)("Started heap restoration");
  HandleMark hm(Thread::current());

  // Before actually restoring anything, record existing objects so that they
  // are not re-created
  // TODO Currently only the mirrors themselves + contents of a few of their
  //  fields are recorded. Ideally, we should walk recursively and record all
  //  existing objects so that we don't re-create them, but this should be
  //  fairly complex since the dumped and the current state may not match.
  _heap_dump.class_dumps.iterate([&](HeapDump::ID _, const HeapDump::ClassDump &dump) -> bool {
    find_and_record_java_class(dump, CHECK_false);
    return true;
  });
  if (HAS_PENDING_EXCEPTION) {
    return;
  }

  // Restore objects reachable from classes being restored.
  // TODO should also restore array and primitive mirrors?
  _instance_classes.iterate([&](HeapDump::ID class_id, InstanceKlass *ik) -> bool {
    if (!ik->is_being_restored()) {
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
    CracClassStateRestorer::assert_hierarchy_init_states_are_consistent(*ik);
  });
#endif // ASSERT
  guarantee(_prepared_loaders.number_of_entries() == 0, "some prepared class loaders have not defined any classes");

  // Restore objects reachable from thread stacks to be restored.
  for (const auto *trace : stack_traces) {
    for (u4 i = 0; i < trace->frames_num(); i++) {
      const StackTrace::Frame &frame = trace->frames(i);
      for (u2 j = 0; j < frame.locals.size(); j++) {
        const StackTrace::Frame::Value &value = frame.locals[j];
        if (value.type == DumpedStackValueType::REFERENCE) {
          restore_object(value.obj_id, CHECK);
        }
      }
      for (u2 j = 0; j < frame.operands.size(); j++) {
        const StackTrace::Frame::Value &value = frame.operands[j];
        if (value.type == DumpedStackValueType::REFERENCE) {
          restore_object(value.obj_id, CHECK);
        }
      }
    }
  }

  _well_known_objects.get_from(_objects);
  log_info(crac)("Finished heap restoration");
}

// Recording of existing objects

// Finds j.l.Class object corresponding to the class dump and records it.
void CracHeapRestorer::find_and_record_java_class(const HeapDump::ClassDump &class_dump, TRAPS) {
  Thread *const current = Thread::current();

  const HeapDump::InstanceDump &mirror_dump = _heap_dump.get_instance_dump(class_dump.id);
  _mirror_dump_reader.ensure_initialized(_heap_dump, mirror_dump.class_id);
  using MirrorType = HeapDumpClasses::java_lang_Class::Kind;
  switch (_mirror_dump_reader.kind(mirror_dump)) {
    case MirrorType::INSTANCE: {
      const InstanceKlass &ik = get_instance_class(class_dump.id);
      const instanceHandle mirror(current, static_cast<instanceOop>(ik.java_mirror()));
      record_java_class(mirror, mirror_dump, CHECK);
      break;
    }
    case MirrorType::ARRAY: {
      const ArrayKlass &ak = get_array_class(class_dump.id);
      const instanceHandle mirror(current, static_cast<instanceOop>(ak.java_mirror()));
      record_java_class(mirror, mirror_dump, CHECK);

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

        record_java_class(prim_mirror, prim_mirror_dump, CHECK);
      }
      break;
    }
    case MirrorType::PRIMITIVE:
      // Class dumps are only created from InstanceKlasses and ArrayKlasses
      guarantee(false, "instance or array class " HDID_FORMAT " has a primitive type mirror", class_dump.id);
  }
}

void CracHeapRestorer::record_java_class(instanceHandle mirror, const HeapDump::InstanceDump &mirror_dump, TRAPS) {
  precond(!_objects.contains(mirror_dump.id) && mirror.not_null());
  const jobject recorded_mirror = put_object_when_absent(mirror_dump.id, mirror);

  _mirror_dump_reader.ensure_initialized(_heap_dump, mirror_dump.class_id);

#ifdef ASSERT
  const HeapDump::ID loader_id = _heap_dump.get_class_dump(mirror_dump.id).class_loader_id;
  assert(loader_id == HeapDump::NULL_ID || get_object_when_present(loader_id) == java_lang_Class::class_loader(mirror()),
         "class loader must already be recorded");

  const HeapDump::ID component_mirror_id = _mirror_dump_reader.componentType(mirror_dump);
  assert(component_mirror_id == HeapDump::NULL_ID || get_object_when_present(component_mirror_id) == java_lang_Class::component_mirror(mirror()),
         "component mirror must already be recorded");
#endif

  const HeapDump::ID module_id = _mirror_dump_reader.module(mirror_dump);
  const oop module_obj = java_lang_Class::module(mirror());
  assert(module_obj != nullptr, "module must be set");
  put_object_if_absent(module_id, module_obj); // Can be pre-recorded via another class from this module

  // For classes that are already in use (the pre-created ones) name can be
  // initialized concurrently, so if it was dumped, initialize and record it
  // eagerly
  const HeapDump::ID name_id = _mirror_dump_reader.name(mirror_dump);
  if (name_id != HeapDump::NULL_ID) {
    const oop name = java_lang_Class::name(mirror, CHECK);
    put_object_if_absent(name_id, name); // Checks it's either absent or set to the same oop
  }

  // TODO for the pre-created mirrors, should we fill the rest of the mirror
  //  instance fields + class static fields?
  //  - If we do, it's not straight forward because the fields may have
  //    different values of different classes than they were when dumped
  //  - If we don't and these values were references somewhere in the dump,
  //    they will be restored and thus duplicated

  if (log_is_enabled(Debug, crac)) {
    log_debug(crac)("Recorded mirror " HDID_FORMAT " of %s", mirror_dump.id,
                    java_lang_Class::as_Klass(mirror())->external_name());
  }
}

// Actual restoration

#ifdef ASSERT
static Klass *get_ref_field_type(const FieldStream &fs) {
  Thread *const thread = Thread::current();
  const InstanceKlass *field_holder = fs.field_descriptor().field_holder();
  const Handle holder_loader = Handle(thread, field_holder->class_loader());
  return SystemDictionary::find_constrained_instance_or_array_klass(thread, fs.signature(), holder_loader);
}
#endif // ASSERT

void CracHeapRestorer::set_field(instanceHandle obj, const FieldStream &fs, const HeapDump::BasicValue &val, TRAPS) {
  precond(obj.not_null());
  switch (Signature::basic_type(fs.signature())) {
    case T_OBJECT:
    case T_ARRAY: {
      precond(obj->obj_field(fs.offset()) == nullptr);
      const Handle restored = restore_object(val.as_object_id, CHECK);
#ifdef ASSERT
      if (restored.not_null()) {
        Klass *const field_type = get_ref_field_type(fs);
        assert(field_type != nullptr, "field's type must be loaded since the field is assigned");
        assert(restored->klass()->is_subtype_of(field_type), "field of type %s cannot be assigned a value of class %s",
               fs.signature()->as_klass_external_name(), restored->klass()->external_name());
      }
#endif // ASSERT
      obj->obj_field_put(fs.offset(), restored());
      break;
    }
    case T_BOOLEAN: precond(obj->bool_field(fs.offset()) == false); obj->bool_field_put(fs.offset(), val.as_boolean);  break;
    case T_CHAR:    precond(obj->char_field(fs.offset()) == 0);     obj->char_field_put(fs.offset(), val.as_char);     break;
    case T_FLOAT:   precond(obj->float_field(fs.offset()) == 0.0F); obj->float_field_put(fs.offset(), val.as_float);   break;
    case T_DOUBLE:  precond(obj->double_field(fs.offset()) == 0.0); obj->double_field_put(fs.offset(), val.as_double); break;
    case T_BYTE:    precond(obj->byte_field(fs.offset()) == 0);     obj->byte_field_put(fs.offset(), val.as_byte);     break;
    case T_SHORT:   precond(obj->short_field(fs.offset()) == 0);    obj->short_field_put(fs.offset(), val.as_short);   break;
    case T_INT:     precond(obj->int_field(fs.offset()) == 0);      obj->int_field_put(fs.offset(), val.as_int);       break;
    case T_LONG:    precond(obj->long_field(fs.offset()) == 0);     obj->long_field_put(fs.offset(), val.as_long);     break;
    default:        ShouldNotReachHere();
  }
}

// Returns true iff the field has been set.
bool CracHeapRestorer::set_field_if_special(instanceHandle obj, const FieldStream &fs, const HeapDump::BasicValue &val, TRAPS) {
  precond(obj.not_null());
  if (fs.access_flags().is_static()) {
    return false;
  }

  if (obj->klass()->is_class_loader_instance_klass() && fs.field_descriptor().field_holder() == vmClasses::ClassLoader_klass()) {
    // Skip the CLD pointer which is set when registering the loader
    if (fs.name() == vmSymbols::loader_data_name()) {
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
    if (fs.name() == vmSymbols::parent_name() || fs.name() == vmSymbols::name_name() ||
        fs.name()->equals("nameAndId") || fs.name()->equals("unnamedModule")) {
      const HeapDump::ID obj_id = val.as_object_id;
      assert(obj_id == HeapDump::NULL_ID && obj->obj_field(fs.offset()) == nullptr ||
             get_object_when_present(obj_id) == obj->obj_field(fs.offset()),
             "either null or recorded with same value");
      return true;
    }

    // When preparing, parallelLockMap is only allocated and left unrestored, so
    // restore it now
    if (fs.name()->equals("parallelLockMap")) {
      const HeapDump::ID parallel_lock_map_id = val.as_object_id;
      const oop parallel_lock_map = obj->obj_field(fs.offset());
      if (parallel_lock_map != nullptr) {
        assert(parallel_lock_map->klass() == vmClasses::ConcurrentHashMap_klass(), "must be");
        assert(get_object_when_present(parallel_lock_map_id) == parallel_lock_map, "must be recorded when preparing");
        const HeapDump::InstanceDump &parallel_lock_map_dump = _heap_dump.get_instance_dump(parallel_lock_map_id);
        restore_instance_fields(obj, parallel_lock_map_dump, CHECK_false);
      } else {
        assert(parallel_lock_map_id == HeapDump::NULL_ID, "must be");
      }
      return true;
    }

    // TODO "classes" field: ArrayList into which mirrors of defined classes are put

    // The rest of the fields are untouched by the preparation and should be
    // restored as usual
    return false;
  }

  if (obj->klass()->is_mirror_instance_klass()) {
    assert(fs.field_descriptor().field_holder() == vmClasses::Class_klass(),  "the only super is j.l.Object which has no fields");

    // Skip primitive fields set when creating the mirror
    if (fs.name() == vmSymbols::klass_name() || fs.name() == vmSymbols::array_klass_name() ||
        fs.name() == vmSymbols::oop_size_name() || fs.name() == vmSymbols::static_oop_field_count_name()) {
      return true;
    }
    // Component class mirror (aka component type) is also set when creating the
    // mirror iff it corresponds to an array class, and it must be already
    // recorded because we pre-record all mirors
    if (fs.name() == vmSymbols::componentType_name()) {
#ifdef ASSERT
      const HeapDump::ID component_mirror_id = val.as_object_id;
      if (component_mirror_id != HeapDump::NULL_ID) {
        assert(java_lang_Class::as_Klass(obj())->is_array_klass(),
               "a %s<%s> object has 'componentType' dumped referencing " HDID_FORMAT " when it represents a non-array class",
               vmSymbols::java_lang_Class()->as_klass_external_name(), java_lang_Class::as_Klass(obj())->external_name(), component_mirror_id);
        const oop component_mirror = obj->obj_field(fs.offset());
        assert(component_mirror != nullptr, "array class mirror must have its component mirror set");
        assert(get_object_when_present(component_mirror_id) == component_mirror, "component class mirror must be pre-recorded");
      } else {
        assert(java_lang_Class::as_Klass(obj())->is_instance_klass(),
               "a %s<%s> object has 'componentType' dumped as null when it represents an array class",
               vmSymbols::java_lang_Class()->as_klass_external_name(), java_lang_Class::as_Klass(obj())->external_name());
        assert(obj->obj_field(fs.offset()) == nullptr, "instance class mirror cannot have its component mirror set");
      }
#endif // ASSERT
      return true;
    }
    // Module is also set when creating the mirror and is pre-recorded
    if (fs.name()->equals("module")) {
#ifdef ASSERT
      const HeapDump::ID module_id = val.as_object_id;
      const oop module = obj->obj_field(fs.offset());
      assert(module_id != HeapDump::NULL_ID && module != nullptr, "mirror's module is always not null");
      assert(_objects.contains(module_id) && JNIHandles::resolve_non_null(*_objects.get(module_id)) == module,
             "mirror's module must be pre-recorded");
#endif // ASSERT
      return true;
    }

    // If the defining loader is a prepared one we should restore the fields
    // unfilled by its preparation, and unmark the loader as prepared so that
    // this won't be repeated when restoring other classes defined by the loader
    if (fs.name() == vmSymbols::classLoader_name()) {
      const HeapDump::ID loader_id = val.as_object_id;
      assert(_objects.contains(loader_id), "used loaders must already be recorded");
      if (_prepared_loaders.remove(loader_id)) { // If the loader is prepared
        const oop loader = obj->obj_field(fs.offset());
        precond(java_lang_ClassLoader::is_instance(loader));
        const instanceHandle loader_h(Thread::current(), static_cast<instanceOop>(loader));
        // We use this fact to distinguish prepared loaders from the unprepared
        // ones when restoring them
        assert(java_lang_ClassLoader::unnamedModule(loader) != nullptr, "preparation must set the unnamed module");
        restore_instance_fields(loader_h, _heap_dump.get_instance_dump(loader_id), CHECK_false);
      }
      return true;
    }

    return false;
  }

  if (obj->klass() == vmClasses::String_klass()) {
    assert(fs.field_descriptor().field_holder() == vmClasses::String_klass(), "the only super is j.l.Object which has no fields");
    // TODO ensure interned strings are indeed interned when restoring them
    //  (e.g. constant pool strings and j.l.Class::name are always interned)

    // Flags are internal and depend on VM options. They will be set as needed,
    // so just ignore them.
    if (fs.name() == vmSymbols::flags_name()) {
      return true;
    }
  }

  // TODO other special cases (need to check all classes from javaClasses)
  return false;
}

// TODO use other means to iterate over fields: FieldStream performs a linear
//  search for each field

void CracHeapRestorer::restore_instance_fields(instanceHandle obj, const HeapDump::InstanceDump &dump, TRAPS) {
  precond(obj.not_null());
  FieldStream fs(InstanceKlass::cast(obj->klass()), false, true); // Includes supers' fields but not interfaces' since those are static
  u4 dump_offset = 0;
  for (; !fs.eos() && dump_offset < dump.fields_data.size(); fs.next()) {
    if (fs.access_flags().is_static()) {
      continue;
    }

    const BasicType type = Signature::basic_type(fs.signature());
    const u4 type_size = (is_java_primitive(type) ? type2aelembytes(type) : _heap_dump.id_size);
    guarantee(dump_offset + type_size < dump.fields_data.size(),
              "object " HDID_FORMAT " has less non-static fields' data dumped than needed by its class %s and its super classes: "
              "read " UINT32_FORMAT " bytes and expect at least " UINT32_FORMAT " more for %s value, but only " UINT32_FORMAT " bytes left",
              dump.id, obj->klass()->external_name(), dump_offset, type_size, type2name(type), dump.fields_data.size() - dump_offset);
    const HeapDump::BasicValue val = dump.read_field(dump_offset, type, _heap_dump.id_size);

    const bool is_special = set_field_if_special(obj, fs, val, CHECK);
    if (!is_special) {
      set_field(obj, fs, val, CHECK);
    }

    dump_offset += type_size;
  }

#ifdef ASSERT
  u4 unfilled_bytes = 0;
  for (; !fs.eos(); fs.next()) {
    if (!fs.access_flags().is_static()) {
      const BasicType type = Signature::basic_type(fs.signature());
      unfilled_bytes += (is_java_primitive(type) ? type2aelembytes(type) : _heap_dump.id_size);
    }
  }
  assert(unfilled_bytes == 0 || dump_offset == dump.fields_data.size(), "must be");
  assert(unfilled_bytes > 0,
         "object " HDID_FORMAT " has less non-static fields' data dumped than needed by its class %s and its super classes: "
         "only " UINT32_FORMAT " bytes dumped, but additional " UINT32_FORMAT " bytes are expected",
         dump.id, obj->klass()->external_name(), dump.fields_data.size(), unfilled_bytes);
  assert(dump_offset < dump.fields_data.size(),
         "object " HDID_FORMAT " has more non-static fields' data dumped than needed by its class %s and its super classes: "
         UINT32_FORMAT " bytes dumped, but only " UINT32_FORMAT " expected",
         dump.id, obj->klass()->external_name(), dump.fields_data.size(), dump_offset);
#endif // ASSERT
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
  FieldStream fs(ik, true, true); // Iterates only over fields declared in this class/interface directly
  u2 static_i = 0;
  while (!fs.eos() && static_i < dump.static_fields.size()) {
    if (!fs.access_flags().is_static()) {
      fs.next();
      continue;
    }

    const HeapDump::ClassDump::Field &field = dump.static_fields[static_i++];
    const Symbol *field_name = _heap_dump.get_symbol(field.info.name_id);

    // HeapDumper includes constant pool's resolved references as static fields
    if (field_name == vmSymbols::resolved_references_name()) {
      guarantee(field.info.type == HPROF_ARRAY_OBJECT,
                "resolved references must be stored in an array "
                "but they are dumped as a %s in class %s (ID " HDID_FORMAT ") as static field #%i",
                type2name(HeapDump::htype2btype(field.info.type)), ik->external_name(), dump.id, static_i);
      // Restore the references if they aren't pre-created
      if (!ik->is_linked() /*pre-linked*/ && !(ik->is_rewritten() && ik->is_shared() /*pre-rewritten*/)) {
        const Handle restored = restore_object(field.value.as_object_id, CHECK);
        set_resolved_references(ik, restored);
      }
      continue; // No fs.next() because there is no actual field for this
    }

    guarantee(fs.name() == field_name && Signature::basic_type(fs.signature()) == HeapDump::htype2btype(field.info.type),
              "expected static field #%i of class %s (ID " HDID_FORMAT ") to be %s %s but it is %s %s in the dump",
              static_i, ik->external_name(), dump.id,
              type2name(Signature::basic_type(fs.signature())), fs.name()->as_C_string(),
              type2name(HeapDump::htype2btype(field.info.type)), field_name->as_C_string());
    set_field(mirror, fs, field.value, CHECK);

    fs.next();
  }

#ifdef ASSERT
  u2 unfilled_fields_num = 0;
  for (; !fs.eos(); fs.next()) {
    if (fs.access_flags().is_static()) {
      unfilled_fields_num++;
    }
  }
  assert(unfilled_fields_num == 0 || static_i == dump.static_fields.size(), "must be");
  assert(unfilled_fields_num == 0,
         "class %s (ID " HDID_FORMAT ") has not enough static fields dumped: expected %i more",
         ik->external_name(), dump.id, unfilled_fields_num);
  assert(static_i == dump.static_fields.size(),
         "class %s (ID " HDID_FORMAT ") has too many static fields dumped: expected %i, got %i",
         ik->external_name(), dump.id, static_i, dump.static_fields.size());
#endif // ASSERT
}

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
    const oop mirror_obj = get_object_when_present(id);
    assert(mirror_obj->is_instance(), "mirrors are instances");
    mirror = instanceHandle(Thread::current(), static_cast<instanceOop>(mirror_obj));
  }

  // Side-effect: finishes restoration of the class loader if only prepared
  const HeapDump::InstanceDump &mirror_dump = _heap_dump.get_instance_dump(id);
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
  const jobject *ready = _objects.get(id);
  if (ready != nullptr) {
    return {Thread::current(), JNIHandles::resolve(*ready)};
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

instanceHandle CracHeapRestorer::restore_instance(const HeapDump::InstanceDump &dump, TRAPS) {
  assert(!_objects.contains(dump.id), "use restore_object() instead");
  log_trace(crac)("Restoring instance " HDID_FORMAT, dump.id);

  InstanceKlass &ik = get_instance_class(dump.class_id);
  guarantee(!ik.is_mirror_instance_klass(), "unrecorded class mirror " HDID_FORMAT, dump.id);
  guarantee(!ik.should_be_initialized(),
            "object " HDID_FORMAT " is an instance of uninitialized class %s (" HDID_FORMAT ")",
            dump.id, ik.external_name(), dump.class_id);
  ik.check_valid_for_instantiation(true, CHECK_({}));

  const instanceHandle obj = ik.allocate_instance_handle(CHECK_({}));
  put_object_when_absent(dump.id, obj); // Record first to be able to find in case of circular references
  restore_instance_fields(obj, dump, CHECK_({}));

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

  typeArrayOop array = oopFactory::new_typeArray_nozero(elem_type, length, CHECK_({}));
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
