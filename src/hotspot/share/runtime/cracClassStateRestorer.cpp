#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/systemDictionary.hpp"
#include "interpreter/linkResolver.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/array.hpp"
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
#include "prims/methodHandles.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"
#include "utilities/constantTag.hpp"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/heapDumpParser.hpp"
#include "utilities/macros.hpp"

#ifdef ASSERT

#include "oops/fieldStreams.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "utilities/globalDefinitions.hpp"

static int count_overpasses(const Array<Method *> &methods) {
  int num_overpasses = 0;
  for (int i = 0; i < methods.length(); i++) {
    if (methods.at(i)->is_overpass()) {
      num_overpasses++;
    }
  }
  return num_overpasses;
}

static void assert_constants_match(const ConstantPool &cp1, const ConstantPool &cp2) {
  assert(cp1.length() == cp2.length(), "number of constants differs: %i != %i", cp1.length(), cp2.length());

  // Constant pool consists of two parts: the first one comes from the class
  // file while the second one is appended when generating overpass methods. We
  // can only compare the first one because the second is not portable: the
  // order in which overpasses are generated, and thus in which the their
  // constants are appended, depends on methods in supers and interfaces which
  // depends on the layout of method name symbols in memory).
  const int num_overpasses = count_overpasses(*cp1.pool_holder()->methods());
  assert(num_overpasses == count_overpasses(*cp2.pool_holder()->methods()), "number of overpass methods differ");
  // An overpass method may need up to this many new constants:
  // 1. 2 for method's name and type.
  // 2. 8 for method's code (see BytecodeAssembler::assemble_method_error()):
  //     new           <error's Class>           +2: UTF8 for class'es name, Class itself
  //     dup
  //     ldc(_w)       <error's msg String>      +2: UTF8, String
  //     invokespecial <error's init Methodref>  +4: 2 UTF8s for init's name and type, NameAndType, Methodref (its Class is the one already added by new)
  //     athrow
  // In the worst case the 1st overpass will add all entries listed above:
  static constexpr int max_cp_entries_first_overpass = 10;
  // All overpasses use <init>(Ljava/lang/String;)V in invoke special, hence
  // its 2 UTF8s and a NameAndType will only be added by the 1st overpass:
  static constexpr int max_cp_entries_second_overpass = 7;
  // There are only two error classes used in overpasses -- their UTF8 names,
  // Class entries and init Methodref entries are already accounted for by the
  // 1st and 2nd overpasses above:
  static constexpr int max_cp_entries_other_overpass = 4;
  // This is a conservative estimation of the length of the comparable part: its
  // actual length is not less than this
  const int comparable_cp_length = MAX2(0, cp1.length() - (num_overpasses >= 1 ? max_cp_entries_first_overpass : 0)
                                                        - (num_overpasses >= 2 ? max_cp_entries_second_overpass : 0)
                                                        - MAX2(0, num_overpasses - 2) * max_cp_entries_other_overpass);

  for (int i = 1; i < comparable_cp_length; i++) {
    // Compare resolved and unresolved versions of the same tag as equal since
    // the version re-created from the dump may have more entries resolved than
    // the pre-defined one (or vise versa)
    assert(cp1.tag_at(i).external_value() == cp2.tag_at(i).external_value(),
           "incompatible constant pool tags at slot #%i: %s and %s",
           i, cp1.tag_at(i).internal_name(), cp2.tag_at(i).internal_name());
  }
}

static void assert_fields_match(const InstanceKlass &ik1, const InstanceKlass &ik2) {
  AllFieldStream fs1(&ik1);
  AllFieldStream fs2(&ik2);
  assert(fs1.num_total_fields() == fs2.num_total_fields(), "number of fields differs: %i != %i",
         fs1.num_total_fields(), fs2.num_total_fields());
  for (; !fs1.done() && !fs2.done(); fs1.next(), fs2.next()) {
    assert(fs1.index() == fs2.index(), "must be");
    assert(fs1.name() == fs2.name() && fs1.signature() == fs2.signature(), "field %i differs: %s %s and %s %s",
           fs1.index(), fs1.signature()->as_C_string(), fs1.name()->as_C_string(), fs2.signature()->as_C_string(), fs2.name()->as_C_string());
    assert(fs1.access_flags().get_flags() == fs2.access_flags().get_flags(), "different access flags of field %i: " INT32_FORMAT_X " != " INT32_FORMAT_X,
           fs1.index(), fs1.access_flags().get_flags(), fs2.access_flags().get_flags());
    assert(fs1.field_flags().as_uint() == fs2.field_flags().as_uint(), "different internal flags of field %i (%s %s): " UINT32_FORMAT_X " != " UINT32_FORMAT_X,
           fs1.index(), fs1.signature()->as_C_string(), fs1.name()->as_C_string(), fs1.field_flags().as_uint(), fs2.field_flags().as_uint());
  }
  postcond(fs1.done() && fs2.done());
}

static void assert_methods_match(const InstanceKlass &ik1, const InstanceKlass &ik2) {
  const Array<Method *> &methods1 = *ik1.methods();
  const Array<Method *> &methods2 = *ik2.methods();
  assert(methods1.length() == methods2.length(), "number of methods differs: %i != %i",
         methods1.length(), methods2.length());
  for (int i = 0; i < methods1.length(); i++) {
    const Method &method1 = *methods1.at(i);
    // Cannot just get by index because the order of methods with the same name may differ
    const Method *method2 = ik2.find_local_method(method1.name(), method1.signature(),
                                                  method1.is_overpass() ? Klass::OverpassLookupMode::find : Klass::OverpassLookupMode::skip,
                                                  method1.is_static() ? Klass::StaticLookupMode::find : Klass::StaticLookupMode::skip,
                                                  Klass::PrivateLookupMode::find);
    assert(method2 != nullptr, "%s not found in the second class", method1.name_and_sig_as_C_string());
    assert(method1.access_flags().get_flags() == method2->access_flags().get_flags(), "different flags of method %s: " INT32_FORMAT_X " != " INT32_FORMAT_X,
           method1.name_and_sig_as_C_string(), method1.access_flags().get_flags(), method2->access_flags().get_flags());
  }
}

#endif // ASSERT

static void move_constant_pool_cache(ConstantPool *from, ConstantPool *to) {
  guarantee(to->cache() == nullptr, "destination class already has a constant pool cache");
  guarantee(from->length() == to->length(), "not the same class");
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
    // Can only compare names because methods with equal names can be reordered
    assert(method1->name() == method2->name(), "method #%i of %s has different names: %s and %s",
           i, ik1->external_name(), method1->name()->as_C_string(), method2->name()->as_C_string());
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
  if (was_predefined) {
    if (log_is_enabled(Debug, crac, class)) {
      ResourceMark rm;
      log_debug(crac, class)("Using pre-defined version of %s", defined_ik->external_name());
    }
    assert(created_ik->access_flags().as_int() == defined_ik->access_flags().as_int(),
           "pre-defined %s has different access flags: " INT32_FORMAT_X " (dumped) != " INT32_FORMAT_X " (pre-defined)",
           created_ik->external_name(), created_ik->access_flags().as_int(), defined_ik->access_flags().as_int());
    DEBUG_ONLY(assert_constants_match(*created_ik->constants(), *defined_ik->constants()));
    DEBUG_ONLY(assert_fields_match(*created_ik, *defined_ik));
    DEBUG_ONLY(assert_methods_match(*created_ik, *defined_ik));
  }

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

static Method *find_possibly_sig_poly_method(InstanceKlass *holder, Symbol *name, Symbol *signature, CracClassDump::MethodKind kind, TRAPS) {
  precond(holder != nullptr);
  if (MethodHandles::is_signature_polymorphic_intrinsic_name(holder, name)) {
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

void CracClassStateRestorer::fill_interclass_references(InstanceKlass *ik,
                                                        const ParsedHeapDump &heap_dump,
                                                        const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &iks,
                                                        const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &aks,
                                                        const InterclassRefs &refs, TRAPS) {
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
  for (const InterclassRefs::MethodRef &method_ref : *refs.method_refs) {
    ConstantPoolCacheEntry &cache_entry = *cp_cache.entry_at(method_ref.cache_index);
    if (method_ref.f1_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const holder = iks.get(method_ref.f1_class_id);
      guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by f1 in resolved method entry #%i of %s",
                method_ref.f1_class_id, method_ref.cache_index, ik->external_name());
      if (method_ref.f1_is_method) {
        Symbol *const name = heap_dump.get_symbol(method_ref.f1_method_desc.name_id);
        Symbol *const sig = heap_dump.get_symbol(method_ref.f1_method_desc.sig_id);
        Method *const method = find_possibly_sig_poly_method(*holder, name, sig, method_ref.f1_method_desc.kind, CHECK);
        guarantee(method != nullptr, "class %s has a resolved method entry #%i with f1 referencing %s method %s that cannot be found",
                  ik->external_name(), method_ref.cache_index, CracClassDump::method_kind_name(method_ref.f1_method_desc.kind),
                  Method::name_and_sig_as_C_string(*holder, name, sig));
        cache_entry.set_f1(method);
      } else {
        cache_entry.set_f1(*holder);
      }
    }
    if (method_ref.f2_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const holder = iks.get(method_ref.f2_class_id);
      guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by f2 in resolved method entry #%i of %s",
                method_ref.f2_class_id, method_ref.cache_index, ik->external_name());
      Symbol *const name = heap_dump.get_symbol(method_ref.f2_method_desc.name_id);
      Symbol *const sig = heap_dump.get_symbol(method_ref.f2_method_desc.sig_id);
      Method *const method = find_possibly_sig_poly_method(*holder, name, sig, method_ref.f2_method_desc.kind, CHECK);
      guarantee(method != nullptr, "class %s has a resolved method entry #%i with f2 referencing %s method %s that cannot be found",
                ik->external_name(), method_ref.cache_index, CracClassDump::method_kind_name(method_ref.f2_method_desc.kind),
                Method::name_and_sig_as_C_string(*holder, name, sig));
      cache_entry.set_f2(reinterpret_cast<intx>(method));
    }
  }
  for (const InterclassRefs::IndyAdapterRef &indy_ref : *refs.indy_refs) {
    InstanceKlass **const holder = iks.get(indy_ref.holder_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved invokedynamic entry #%i of %s",
              indy_ref.holder_id, indy_ref.indy_index, ik->external_name());
    Symbol *const name = heap_dump.get_symbol(indy_ref.method_desc.name_id);
    Symbol *const sig = heap_dump.get_symbol(indy_ref.method_desc.sig_id);
    Method *const method = (*holder)->find_local_method(name, sig,
                                                        CracClassDump::as_overpass_lookup_mode(indy_ref.method_desc.kind),
                                                        CracClassDump::as_static_lookup_mode(indy_ref.method_desc.kind),
                                                        Klass::PrivateLookupMode::find);
    guarantee(method != nullptr, "class %s has a resolved invokedynamic entry #%i referencing %s method %s that cannot be found",
              ik->external_name(), indy_ref.indy_index, CracClassDump::method_kind_name(indy_ref.method_desc.kind),
              Method::name_and_sig_as_C_string(*holder, name, sig));
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
