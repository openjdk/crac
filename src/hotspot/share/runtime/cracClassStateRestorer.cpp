#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/systemDictionary.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/constantPool.hpp"
#include "oops/cpCache.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/oopsHierarchy.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"
#include "utilities/constantTag.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"

static void move_constant_pool_cache(ConstantPool *from, ConstantPool *to) {
  guarantee(to->cache() == nullptr, "destination class already has a constant pool cache");
  guarantee(from->length() == to->length(), "not the same class");
#ifdef ASSERT
  for (int i = 1; i < from->length(); i++) {
    assert(from->tag_at(i).external_value() == to->tag_at(i).external_value(),
           "incompatible constant pool tags at slot #%i: %s and %s",
           i, from->tag_at(i).internal_name(), to->tag_at(i).internal_name());
  }
#endif // ASSERT
  ConstantPoolCache *const cache = from->cache();
  to->set_cache(cache);
  cache->set_constant_pool(to);
  from->set_cache(nullptr);
}

static void swap_methods(InstanceKlass *ik1, InstanceKlass *ik2) {
  guarantee(ik1->methods()->length() == ik2->methods()->length(), "not the same class");
  auto *const methods1 = ik1->methods();
  auto *const methods2 = ik2->methods();
  ik1->set_methods(methods2);
  ik2->set_methods(methods1);
  for (int i = 0; i < methods1->length(); i++) {
    Method *const method1 = methods1->at(i); // Moving from ik1 into ik2
    Method *const method2 = methods2->at(i); // Moving from ik2 into ik1
    guarantee(method1->name_index() == method2->name_index() && method1->signature_index() == method2->signature_index(),
              "not the same method: %s and %s", method1->name_and_sig_as_C_string(), method2->name_and_sig_as_C_string());
    assert(method1->name() == method2->name() && method1->signature() == method2->signature(), // Checks the actual CP contents
           "not the same method: %s and %s", method1->name_and_sig_as_C_string(), method2->name_and_sig_as_C_string());
    method1->set_constants(ik2->constants());
    method2->set_constants(ik1->constants());
  }
}

InstanceKlass *CracClassStateRestorer::define_created_class(InstanceKlass *created_ik, InstanceKlass::ClassState target_state, TRAPS) {
  precond(created_ik != nullptr && created_ik->is_being_restored() && !created_ik->is_loaded());

  // May get another class if one has been defined already:
  // - created_ik -- what we have parsed from the dump
  // - defined_ik -- what we should use
  // If created_ik != defined_ik the former will be deallocated.
  InstanceKlass *const defined_ik = SystemDictionary::find_or_define_recreated_class(created_ik, CHECK_NULL);
  postcond(defined_ik->is_loaded());

  const bool was_predefined = defined_ik != created_ik;
  assert(!(was_predefined && defined_ik->is_being_restored()), "pre-defined classes must be unmarked");
  // TODO We assume the pre-defined class was created from the same class file
  //  as the freshly created class was which may not be true. E.g. it could've
  //  been redefined or just loaded from a different class file.

  // Ensure the class won't be used by other threads until it is restored. We do
  // this even if the class was only loaded at the dump time to be able to set
  // resolved class references which may appear during verification (even if it
  // failed in the end). In higher dumped states this also saves other threads
  // from using unfilled CP cache entries, unrestored resolved references array
  // and unrestored static fields. But if the pre-defined class is has already
  // attempted initialization, this won't save from anything.
  JavaThread *const thread = JavaThread::current();
  {
    MonitorLocker ml(defined_ik->init_monitor());
    const bool want_to_initialize = target_state >= InstanceKlass::fully_initialized;
    while (defined_ik->is_being_linked() || defined_ik->is_being_initialized()) {
      if (want_to_initialize) {
        thread->set_class_to_be_initialized(defined_ik);
      }
      ml.wait();
      if (want_to_initialize) {
        thread->set_class_to_be_initialized(nullptr);
      }
    }
    if (defined_ik->init_state() < InstanceKlass::fully_initialized) {
      defined_ik->set_is_being_restored(true);
      if ((created_ik->is_rewritten() && !(was_predefined && defined_ik->is_rewritten())) ||
          (target_state >= InstanceKlass::linked && !defined_ik->is_linked())) {
        defined_ik->set_init_state(InstanceKlass::being_linked);
        defined_ik->set_init_thread(thread);
      } else if (want_to_initialize && defined_ik->init_state() < InstanceKlass::fully_initialized) {
        defined_ik->set_init_state(InstanceKlass::being_initialized);
        defined_ik->set_init_thread(thread);
      }
    }
  }
  postcond(!defined_ik->is_init_thread(thread) || defined_ik->is_being_restored());
  postcond(!defined_ik->is_init_thread(thread) || defined_ik->init_state() < InstanceKlass::fully_initialized);

  if (was_predefined) {
    if (created_ik->is_rewritten() && !defined_ik->is_rewritten()) {
      precond(defined_ik->is_init_thread(thread));
      // Apply the rewritten state:
      // 1. Save the constant pool cache created by us to restore it later.
      move_constant_pool_cache(created_ik->constants(), defined_ik->constants());
      // 2. Save the rewritten methods, deallocate the non-rewritten ones.
      swap_methods(created_ik, defined_ik);
      defined_ik->set_rewritten();
      if (log_is_enabled(Debug, crac, class)) {
        ResourceMark rm;
        log_debug(crac, class)("Moved dumped rewritten state into pre-defined %s", defined_ik->external_name());
      }
    }
    created_ik->class_loader_data()->add_to_deallocate_list(created_ik);
  }

  if (target_state < InstanceKlass::linked) {
    assert(target_state != InstanceKlass::being_linked, "not supported, shouldn't be dumped");
    return defined_ik;
  }
  postcond(defined_ik->is_rewritten());
  if (!defined_ik->is_linked()) {
    precond(defined_ik->is_being_linked() && defined_ik->is_init_thread(thread));
    // Omitting vtable/itable constraints check since it was done before the dump
    defined_ik->finish_linking(false, CHECK_NULL);
  }

  if (target_state < InstanceKlass::fully_initialized) {
    assert(target_state != InstanceKlass::being_initialized, "not supported, shouldn't be dumped");
    precond(!defined_ik->is_being_initialized());
    return defined_ik;
  }
  precond(defined_ik->init_state() >= InstanceKlass::fully_initialized || defined_ik->is_init_thread(thread));
  guarantee(!(target_state == InstanceKlass::fully_initialized && defined_ik->is_in_error_state()) &&
            !(target_state == InstanceKlass::initialization_error && defined_ik->is_initialized()),
            "%s is dumped %s, but its initialization has already been re-attempted and %s",
            target_state == InstanceKlass::fully_initialized ? "as successfully initialized" : "with an initialization error",
            defined_ik->is_initialized() ? "succeeded" : "failed", defined_ik->external_name());
  // Static fields and resolution exception object will be set during heap restoration
  return defined_ik;
}

void CracClassStateRestorer::fill_interclass_references(InstanceKlass *ik,
                                                        const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &iks,
                                                        const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &aks,
                                                        const InterclassRefs &refs) {
  if (log_is_enabled(Trace, crac, class)) {
    ResourceMark rm;
    log_trace(crac, class)("Filling interclass references of %s", ik->external_name());
  }

  if (refs.dynamic_nest_host != HeapDump::NULL_ID) {
    assert(ik->is_being_restored() && !ik->is_linked(),
           "only hidden classes have dynamic nest hosts and for now we re-create them all");
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
    // Put the class ensuring we don't overwrite a pre-resolved class/error
    const Klass *k_set = cp->klass_at_put_and_get(class_ref.index, k);
    if (k_set != k) {
      if (k_set == nullptr) {
        guarantee(false, "incompatible state of pre-defined class %s: its constant pool slot #%i has class resolution error, "
                  "but it was successfully resolved to %s at class dump time", ik->external_name(), class_ref.index, k->external_name());
      } else {
        guarantee(false, "incompatible state of pre-defined class %s: its constant pool slot #%i is resolved to class %s when %s was expected",
                  ik->external_name(), class_ref.index, k_set->external_name(), k->external_name());
      }
    }
  }

  // Restore constant pool cache only if it was created by us because unresolved
  // entries are expected to be partially filled
  // TODO restore constant pool cache even if it was pre-created: check the
  //  resolved entries have the expected values, fill the unresolved ones
  if (ik->is_linked() /*pre-linked*/ || (ik->is_shared() && ik->is_rewritten() /*pre-rewritten*/)) {
    return;
  }
  guarantee(ik->is_being_restored(), "all uninitialized classes being restored must be marked");

  // Non-rewritten classes don't have a constant pool cache to restore
  if (!ik->is_rewritten()) {
    assert(!ik->is_init_thread(JavaThread::current()), "no need for this");
    assert(refs.field_refs->is_empty() && refs.method_refs->is_empty() && refs.indy_refs->is_empty(),
           "class %s has unfilled references for its absent constant pool cache", ik->external_name());
    return;
  }
  assert(ik->is_being_linked() && ik->is_init_thread(JavaThread::current()), "must be rewriting the class");

  ConstantPoolCache &cp_cache = *cp->cache();
  for (const InterclassRefs::ClassRef &field_ref : *refs.field_refs) {
    InstanceKlass **const holder = iks.get(field_ref.class_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved field entry #%i of %s",
              field_ref.class_id, field_ref.index, ik->external_name());
    ResolvedFieldEntry &field_entry = *cp_cache.resolved_field_entry_at(field_ref.index);
    field_entry.fill_in_holder(*holder);
  }
  for (const InterclassRefs::MethodRefs &method_ref : *refs.method_refs) {
    ConstantPoolCacheEntry &cache_entry = *cp_cache.entry_at(method_ref.cache_index);
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
    ResolvedIndyEntry &indy_entry = *cp_cache.resolved_indy_entry_at(indy_ref.indy_index);
    indy_entry.adjust_method_entry(method);
  }
}

void CracClassStateRestorer::apply_init_state(InstanceKlass *ik, InstanceKlass::ClassState state, Handle init_error) {
  precond(ik->is_loaded() && ik->is_being_restored());
  precond(init_error.is_null() || state == InstanceKlass::initialization_error);
  ik->set_is_being_restored(false); // Other threads will remain waiting for the state change if needed

  JavaThread *const thread = JavaThread::current();
  if (!ik->is_init_thread(thread)) {
    return;
  }
  postcond(ik->is_rewritten());

  if (ik->is_being_linked()) {
    if (state == InstanceKlass::loaded) {
      // We've rewritten the class but don't want to finish linking it
      ik->set_initialization_state_and_notify(InstanceKlass::loaded, thread);
      return;
    }
    if (state == InstanceKlass::linked) {
      // We've linked the class
      ik->set_initialization_state_and_notify(InstanceKlass::linked, thread);
      return;
    }
    precond(state == InstanceKlass::fully_initialized || state == InstanceKlass::initialization_error);
    // We've linked the class but also initialized it
    ik->set_linked_to_be_initialized_state_and_notify(thread);
  }
  postcond(ik->is_linked());

  precond(ik->is_being_initialized() && ik->is_init_thread(thread));
  if (state == InstanceKlass::initialization_error) {
    ik->put_initializetion_error(thread, init_error);
  }
  ik->set_initialization_state_and_notify(state, thread);
  postcond(ik->is_initialized() || ik->is_in_error_state());
}

#ifdef ASSERT

static void assert_has_consistent_state(const InstanceKlass &base, const InstanceKlass &derived) {
  switch (derived.init_state()) {
    case InstanceKlass::allocated:
      ShouldNotReachHere(); // Too young
      return;
    case InstanceKlass::loaded:
      if (!derived.is_rewritten()) {
        assert(base.is_loaded(), "supers/interfaces of loaded class/interface must be loaded, but %s is not",
               base.external_name());
        return;
      } else { // Intermediate state between loaded and linked
        assert(base.is_linked(), "supers/interfaces of rewritten class/interface must be loaded, but %s is not",
               base.external_name());
        return;
      }
    case InstanceKlass::being_linked:
      assert(const_cast<InstanceKlass &>(derived).is_init_thread(JavaThread::current()), "restoring thread must hold init states of classes being restored");
    case InstanceKlass::linked:
      assert(base.is_linked(), "supers/interfaces of linked class/interface must be linked, but %s is not",
             base.external_name());
      return;
    case InstanceKlass::being_initialized:
      assert(const_cast<InstanceKlass &>(derived).is_init_thread(JavaThread::current()), "restoring thread must hold init states of classes being restored");
    case InstanceKlass::fully_initialized:
      if (!derived.is_interface() && (!base.is_interface() || derived.has_nonstatic_concrete_methods())) {
        assert(base.is_initialized(), "supers and interfaces with default methods of initialized class must be initialized, but %s is not",
               base.external_name());
        return;
      } else {
        assert(base.is_linked(), "supers/interfaces of initialized class/interface must be linked, but %s is not",
               base.external_name());
        return;
      }
    case InstanceKlass::initialization_error:
      if (!derived.is_interface() && (!base.is_interface() || derived.has_nonstatic_concrete_methods())) {
        assert(base.is_initialized() || base.is_in_error_state(),
               "supers and interfaces with default methods of class that attempted initialization must also have attempted initialization, but %s has not",
               base.external_name());
        return;
      } else {
        assert(base.is_linked(), "supers/interfaces of class/interface that attempted initialization must be linked, but %s is not",
               base.external_name());
        return;
      }
    default:
      ShouldNotReachHere();
  }
}

void CracClassStateRestorer::assert_hierarchy_init_states_are_consistent(const InstanceKlass &ik) {
  if (ik.java_super() != nullptr) {
    assert_has_consistent_state(*ik.java_super(), ik);
  }
  for (int i = 0; i < ik.local_interfaces()->length(); i++) {
    const InstanceKlass &interface = *ik.local_interfaces()->at(i);
    assert_has_consistent_state(interface, ik);
  }
}

#endif // ASSERT
