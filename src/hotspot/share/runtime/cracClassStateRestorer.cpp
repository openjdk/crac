#include "precompiled.hpp"
#include "classfile/systemDictionary.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/cpCache.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp"
#include "oops/klassVtable.hpp"
#include "oops/method.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"

#ifdef ASSERT
struct ExtendedClassState {
  InstanceKlass::ClassState state;
  bool is_interface;
  bool is_rewritten;
  bool has_nonstatic_concrete_methods;
};

static void assert_has_consistent_state(const InstanceKlass &super_or_interface, const ExtendedClassState &child_state) {
  switch (child_state.state) {
    case InstanceKlass::loaded:
      if (!child_state.is_rewritten) {
        assert(super_or_interface.is_loaded(), "supers/interfaces of loaded class/interface must be loaded, but %s is not",
               super_or_interface.external_name());
        return;
      } else { // Intermediate state between loaded and linked
        assert(super_or_interface.is_linked(), "supers/interfaces of rewritten class/interface must be loaded, but %s is not",
               super_or_interface.external_name());
        return;
      }
    case InstanceKlass::linked:
      assert(super_or_interface.is_linked(), "supers/interfaces of linked class/interface must be linked, but %s is not",
             super_or_interface.external_name());
      return;
    case InstanceKlass::fully_initialized:
      if (!child_state.is_interface && (!super_or_interface.is_interface() || child_state.has_nonstatic_concrete_methods)) {
        assert(super_or_interface.is_initialized(), "supers and interfaces with default methods of initialized class must be initialized, but %s is not",
               super_or_interface.external_name());
        return;
      } else {
        assert(super_or_interface.is_linked(), "supers/interfaces of initialized class/interface must be linked, but %s is not",
               super_or_interface.external_name());
        return;
      }
    case InstanceKlass::initialization_error:
      if (!child_state.is_interface && (!super_or_interface.is_interface() || child_state.has_nonstatic_concrete_methods)) {
        assert(super_or_interface.is_initialized() || super_or_interface.is_in_error_state(),
               "supers and interfaces with default methods of class that attempted initialization must also have attempted initialization, but %s has not",
               super_or_interface.external_name());
        return;
      } else {
        assert(super_or_interface.is_linked(), "supers/interfaces of class/interface that attempted initialization must be linked, but %s is not",
               super_or_interface.external_name());
        return;
      }
    case InstanceKlass::allocated:         // Too young, not dumped
    case InstanceKlass::being_linked:      // Restoring these is not yet implemented (thus not dumped)
    case InstanceKlass::being_initialized: // Restoring these is not yet implemented (thus not dumped)
    default:
      assert(false, "illegal class state: %i", child_state.state);
      return;
  }
}

static void check_hirarchy_state_consistency(const InstanceKlass *super, const Array<InstanceKlass *> &local_interfaces,
                                             ExtendedClassState child_state) {
  if (super != nullptr) {
    assert_has_consistent_state(*super, child_state);
  }
  for (int i = 0; i < local_interfaces.length(); i++) {
    const InstanceKlass &interface = *local_interfaces.at(i);
    assert_has_consistent_state(interface, child_state);
  }
}
#endif // ASSERT

// TODO the code below does not take concurrency into account (see TODOs below)
void CracClassStateRestorer::apply_state(InstanceKlass *ik, InstanceKlass::ClassState state, TRAPS) {
  precond(ik->is_being_restored());
  precond(!ik->is_loaded());
  assert(state >= InstanceKlass::ClassState::loaded, "too young, checked before");
  DEBUG_ONLY(check_hirarchy_state_consistency(ik->java_super(), *ik->local_interfaces(),
                                              {state, ik->is_rewritten(), ik->has_nonstatic_concrete_methods()}));

  // TODO insure the class has not been loaded/defined concurrently -- need to
  //  follow the complex class loading and definition procedures implemented in
  //  SystemDictionary and not mark the class as being restored if someone else
  //  beats us.
  SystemDictionary::define_recreated_instance_class(ik, CHECK);
  postcond(ik->is_loaded());

  JavaThread *const thread = JavaThread::current();

  assert(state != InstanceKlass::ClassState::being_linked, "not supported, checked before");
  if (state >= InstanceKlass::ClassState::linked) {
    assert(ik->is_rewritten(), "checked before");                   // Protects non-repeatable linkage steps
    assert(!ik->is_shared(), "classes just created aren't shared"); // vtable/itable won't be initialized for shared classes
    // Do what InstanceKlass::link_class() does after rewriting for
    // non-shared classes, ommiting:
    // 1. vtable/itable constraint checks because they should've been done
    //    already by the dumping VM, so we hope everything is correct.
    // 2. Posting "class prepare" event for JVM TI because we want to make
    //    it seem like the class was already loaded.
    InstanceKlass::LockLinkState init_lock(ik, thread);
    guarantee(ik->is_being_linked() && ik->is_init_thread(thread), "concurrent usage of the class being restored");
    ik->link_methods(CHECK);
    ik->vtable().initialize_vtable(nullptr);
    ik->itable().initialize_itable(nullptr);
    DEBUG_ONLY(ik->vtable().verify(tty, true));
    ik->set_initialization_state_and_notify(InstanceKlass::ClassState::linked, thread);
    postcond(ik->is_linked());
  }

  assert(state != InstanceKlass::ClassState::being_initialized, "not supported, checked before");
  if (state >= InstanceKlass::ClassState::fully_initialized) {
    precond(ik->is_linked());
    // Fields and initialization errors will be filled later, after all dumped
    // classes have been created and objects can be allocated. We just set the
    // state to hand it over to the rest of the restoration code together
    // with the created class.
    {
      MutexLocker ml(thread, ik->init_monitor());
      guarantee(ik->init_state() == InstanceKlass::ClassState::linked, "concurrent usage of the class being restored");
      ik->set_init_state(InstanceKlass::ClassState::being_initialized);
      ik->set_init_thread(thread);
    }
    ik->set_initialization_state_and_notify(state, thread); // Either initialized or errored
  }

  // TODO must ensure the class won't be used concurrently until it's fully
  //  restored -- need to hold init locks of all classes being restored?
}

void CracClassStateRestorer::fill_interclass_references(InstanceKlass *ik,
                                                        const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &iks,
                                                        const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &aks,
                                                        const InterclassRefs &refs) {
  if (refs.dynamic_nest_host != HeapDump::NULL_ID) {
    InstanceKlass **const host = iks.get(refs.dynamic_nest_host);
    guarantee(host != nullptr, "unknown class " HDID_FORMAT " referenced as a dynamic nest host of %s",
              refs.dynamic_nest_host, ik->external_name());
    ik->set_nest_host(*host);
  }

  ConstantPool *const cp = ik->constants();
  for (const InterclassRefs::ClassRef &class_ref : *refs.cp_class_refs) {
    Klass *k;
    InstanceKlass **const ik_ptr = iks.get(class_ref.class_id);
    if (ik_ptr != nullptr) {
      k = *ik_ptr;
    } else {
      ArrayKlass **const ak_ptr = aks.get(class_ref.class_id);
      guarantee(ak_ptr != nullptr, "unknown class " HDID_FORMAT " referenced by Class constant pool entry #%i of %s",
                class_ref.class_id, class_ref.index, ik->external_name());
      k = *ak_ptr;
    }
    cp->klass_at_put(class_ref.index, k);
  }

  ConstantPoolCache *const cp_cache = cp->cache();
  if (cp_cache == nullptr) {
    assert(refs.field_refs->is_empty() && refs.method_refs->is_empty() && refs.indy_refs->is_empty(),
          "class %s has unfilled references for its absent constant pool cache", ik->external_name());
    return;
  }

  for (const InterclassRefs::ClassRef &field_ref : *refs.field_refs) {
    InstanceKlass **const holder = iks.get(field_ref.class_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved field entry #%i of %s",
              field_ref.class_id, field_ref.index, ik->external_name());
    ResolvedFieldEntry &field_entry = *cp_cache->resolved_field_entry_at(field_ref.index);
    field_entry.fill_in_holder(*holder);
  }

  for (const InterclassRefs::MethodRefs &method_ref : *refs.method_refs) {
    ConstantPoolCacheEntry &cache_entry = *cp_cache->entry_at(method_ref.cache_index);
    if (method_ref.f1_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const holder = iks.get(method_ref.f1_class_id);
      guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by field 1 of resolved method entry #%i of %s",
                method_ref.f1_class_id, method_ref.cache_index, ik->external_name());
      if (method_ref.f1_is_method) {
        Method *const method = (*holder)->method_with_idnum(method_ref.f1_method_idnum);
        guarantee(method != nullptr, "class %s has resolved method entry #%i with field 1 referencing method with ID %i of %s "
                  "but the latter does not have such method", ik->external_name(), method_ref.cache_index,
                  method_ref.f1_method_idnum, (*holder)->external_name());
        cache_entry.set_f1(method);
      } else {
        cache_entry.set_f1(*holder);
      }
    }
    if (method_ref.f2_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const holder = iks.get(method_ref.f2_class_id);
      guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by field 2 of resolved method entry #%i of %s",
                method_ref.f2_class_id, method_ref.cache_index, ik->external_name());
      Method *const method = (*holder)->method_with_idnum(method_ref.f2_method_idnum);
      guarantee(method != nullptr, "class %s has resolved method entry #%i with field 2 referencing method with ID %i of %s "
                "but the latter does not have such method", ik->external_name(), method_ref.cache_index,
                method_ref.f2_method_idnum, (*holder)->external_name());
      cache_entry.set_f2(reinterpret_cast<intx>(method));
    }
  }

  for (const InterclassRefs::IndyAdapterRef &indy_ref : *refs.indy_refs) {
    InstanceKlass **const holder = iks.get(indy_ref.holder_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved invokedynamic entry #%i of %s",
              indy_ref.holder_id, indy_ref.indy_index, ik->external_name());
    Method *const method = (*holder)->method_with_idnum(indy_ref.method_idnum);
    guarantee(method != nullptr, "class %s has resolved invokedynamic entry #%i referencing method with ID %i of %s "
              "but the latter does not have such method", ik->external_name(), indy_ref.indy_index,
              indy_ref.method_idnum, (*holder)->external_name());
    ResolvedIndyEntry &indy_entry = *cp_cache->resolved_indy_entry_at(indy_ref.indy_index);
    indy_entry.adjust_method_entry(method);
  }
}

void CracClassStateRestorer::fill_initialization_error(InstanceKlass *ik, Handle error) {
  precond(ik->get_initialization_error() == nullptr);
  ik->put_initializetion_error(JavaThread::current(), error);
}
