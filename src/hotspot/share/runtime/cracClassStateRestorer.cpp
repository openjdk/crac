#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/systemDictionary.hpp"
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
#include "runtime/cracClassDumpParser.hpp"
#include "runtime/cracClassDumper.hpp"
#include "runtime/cracClassStateRestorer.hpp"
#include "runtime/handles.hpp"
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
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "interpreter/bytecodes.hpp"
#include "oops/fieldInfo.hpp"
#include "oops/fieldStreams.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "prims/methodHandles.hpp"
#include "utilities/globalDefinitions.hpp"
#endif // ASSERT

#ifdef ASSERT

// Asserts to compare the newly re-created and a pre-defined versions of the
// same class. Note that they are run before the newly re-created class is
// linked and before its CP cache is restored, so the related data is not
// compared.

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
    assert(fs1.offset() == fs2.offset(), "different offset of field %i (%s %s): %i != %i",
           fs1.index(), fs1.signature()->as_C_string(), fs1.name()->as_C_string(), fs1.offset(), fs2.offset());
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

    assert(method1.result_type() == method2->result_type(), "different result type of method %s: %s != %s",
           method1.name_and_sig_as_C_string(), type2name(method1.result_type()), type2name(method2->result_type()));
    assert(method1.size_of_parameters() == method2->size_of_parameters(), "different word-size of parameters of method %s: %i != %i",
           method1.name_and_sig_as_C_string(), method1.size_of_parameters(), method2->size_of_parameters());
    assert(method1.num_stack_arg_slots() == method2->num_stack_arg_slots(), "different number of stack slots for arguments of method %s: %i != %i",
           method1.name_and_sig_as_C_string(), method1.num_stack_arg_slots(), method2->num_stack_arg_slots());
    assert(method1.constMethod()->fingerprint() == method2->constMethod()->fingerprint(),
           "different fingerprint of method %s: " UINT64_FORMAT_X " != " UINT64_FORMAT_X,
           method1.name_and_sig_as_C_string(), method1.constMethod()->fingerprint(), method2->constMethod()->fingerprint());

    assert(method1.access_flags().get_flags() == method2->access_flags().get_flags(), "different flags of method %s: " INT32_FORMAT_X " != " INT32_FORMAT_X,
           method1.name_and_sig_as_C_string(), method1.access_flags().get_flags(), method2->access_flags().get_flags());
    assert(method1.constMethod()->flags() == method2->constMethod()->flags(), "different cmethod flags of method %s: " INT32_FORMAT_X " != " INT32_FORMAT_X,
           method1.name_and_sig_as_C_string(), method1.constMethod()->flags(), method2->constMethod()->flags());

    assert(method1.max_stack() == method2->max_stack() && method1.max_locals() == method2->max_locals(),
           "different max stack values of method %s: max stack %i and %i, max locals %i and %i",
           method1.name_and_sig_as_C_string(), method1.max_stack(), method2->max_stack(), method1.max_locals(), method2->max_locals());
    assert(ik1.is_rewritten() != ik2.is_rewritten() /* code may change upon rewriting */ || method1.code_size() == method2->code_size(),
           "different code size of method %s: %i != %i", method1.name_and_sig_as_C_string(), method1.code_size(), method2->code_size());
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

  const bool predefined = defined_ik != created_ik;
  assert(!(predefined && defined_ik->is_being_restored()), "pre-defined class must be unmarked");

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
      if ((created_ik->is_rewritten() && !(predefined && defined_ik->is_rewritten())) ||
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

  if (predefined) {
    if (log_is_enabled(Debug, crac, class)) {
      ResourceMark rm;
      const char *current_state_name = (defined_ik->is_rewritten() && !defined_ik->is_linked())             ? "rewritten" : defined_ik->init_state_name();
      const char *target_state_name =  (created_ik->is_rewritten() && target_state < InstanceKlass::linked) ? "rewritten" : InstanceKlass::state_name(target_state);
      log_debug(crac, class)("Using pre-defined %s (current state = %s, target state = %s) - defined by %s",
                             defined_ik->external_name(), current_state_name, target_state_name, defined_ik->class_loader_data()->loader_name_and_id());
    }
    assert(created_ik->access_flags().as_int() == defined_ik->access_flags().as_int(),
           "pre-defined %s has different access flags: " INT32_FORMAT_X " (dumped) != " INT32_FORMAT_X " (pre-defined)",
           created_ik->external_name(), created_ik->access_flags().as_int(), defined_ik->access_flags().as_int());
    DEBUG_ONLY(assert_constants_match(*created_ik->constants(), *defined_ik->constants()));
    DEBUG_ONLY(assert_fields_match(*created_ik, *defined_ik));
    DEBUG_ONLY(assert_methods_match(*created_ik, *defined_ik));

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
  } else if (log_is_enabled(Debug, crac, class)) {
    ResourceMark rm;
    const char *current_state_name = (defined_ik->is_rewritten() && !defined_ik->is_linked())             ? "rewritten" : defined_ik->init_state_name();
    const char *target_state_name =  (created_ik->is_rewritten() && target_state < InstanceKlass::linked) ? "rewritten" : InstanceKlass::state_name(target_state);
    log_debug(crac, class)("Using newly defined %s (current state = %s, target state = %s) - defined by %s",
                           defined_ik->external_name(), current_state_name, target_state_name, defined_ik->class_loader_data()->loader_name_and_id());
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

#ifdef ASSERT

static FieldInfo get_field_info(const InstanceKlass &holder, int index) {
  for (AllFieldStream fs(&holder); !fs.done(); fs.next()) {
    if (fs.index() == index) {
      return fs.to_FieldInfo();
    }
  }
  assert(false, "no field with index %i in %s", index, holder.external_name());
  return {};
}

static void assert_correctly_resolved_field(const InstanceKlass &cache_holder, int resolved_info_i) {
  const ResolvedFieldEntry &resolved_info = *cache_holder.constants()->cache()->resolved_field_entry_at(resolved_info_i);
  assert(resolved_info.get_code() != 0, "unresolved");

  const InstanceKlass &field_holder = *resolved_info.field_holder();
  const FieldInfo field_info = get_field_info(field_holder, resolved_info.field_index());

  // Referenced as:
  const u2 ref_holder_i = cache_holder.constants()->uncached_klass_ref_index_at(resolved_info.constant_pool_index());
  const Klass &ref_holder = *cache_holder.constants()->resolved_klass_at(ref_holder_i);
  const Symbol *ref_name = cache_holder.constants()->uncached_name_ref_at(resolved_info.constant_pool_index());
  const Symbol *ref_sig = cache_holder.constants()->uncached_signature_ref_at(resolved_info.constant_pool_index());
  // Resolved as:
  const Symbol *name = field_info.name(field_holder.constants());
  const Symbol *sig = field_info.signature(field_holder.constants());
  // Must match
  assert(ref_name == name && ref_sig == sig, "class %s, field entry #%i: referenced %s %s != resolved %s %s",
         cache_holder.external_name(), resolved_info_i, ref_sig->as_C_string(), ref_name->as_C_string(), sig->as_C_string(), name->as_C_string());
  assert(ref_holder.is_subtype_of(const_cast<InstanceKlass *>(&field_holder)),
         "class %s, field entry #%i (%s %s): referenced holder %s must subtype resolved holder %s",
         cache_holder.external_name(), resolved_info_i, sig->as_C_string(), name->as_C_string(),
         ref_holder.external_name(), field_holder.external_name());

  // Cached data
  assert(checked_cast<u4>(resolved_info.field_offset()) == field_info.offset(), "class %s, field entry #%i (%s %s::%s): %i != " UINT32_FORMAT,
         cache_holder.external_name(), resolved_info_i, sig->as_C_string(), field_holder.external_name(), name->as_C_string(),
         resolved_info.field_offset(), field_info.offset());
  assert(resolved_info.tos_state() == as_TosState(Signature::basic_type(sig)), "class %s, field entry #%i (%s %s::%s): %i != %i",
         cache_holder.external_name(), resolved_info_i, sig->as_C_string(), field_holder.external_name(), name->as_C_string(),
         resolved_info.tos_state(), as_TosState(Signature::basic_type(sig)));
  assert(resolved_info.is_volatile() == field_info.access_flags().is_volatile(), "class %s, field entry #%i (%s %s::%s): %s != %s",
         cache_holder.external_name(), resolved_info_i, sig->as_C_string(), field_holder.external_name(), name->as_C_string(),
         BOOL_TO_STR(resolved_info.is_volatile()), BOOL_TO_STR(field_info.access_flags().is_volatile()));
  assert(resolved_info.is_final() == field_info.access_flags().is_final(), "class %s, field entry #%i (%s %s::%s): %s != %s",
         cache_holder.external_name(), resolved_info_i, sig->as_C_string(), field_holder.external_name(), name->as_C_string(),
         BOOL_TO_STR(resolved_info.is_final()), BOOL_TO_STR(field_info.access_flags().is_final()));
}

static void assert_correctly_resolved_method(const InstanceKlass &cache_holder, int resolved_info_i) {
  const ConstantPoolCacheEntry &resolved_info = *cache_holder.constants()->cache()->entry_at(resolved_info_i);
  assert(resolved_info.bytecode_1() != 0 || resolved_info.bytecode_2() != 0, "unresolved");
  assert(resolved_info.is_method_entry(), "must be");

  Method *const method = resolved_info.method_if_resolved(constantPoolHandle(Thread::current(), cache_holder.constants()));
  assert(method != nullptr, "class %s, cache entry #%i: cannot obtain the resolved method",
         cache_holder.external_name(), resolved_info_i);

  // Referenced as:
  const u2 ref_holder_i = cache_holder.constants()->uncached_klass_ref_index_at(resolved_info.constant_pool_index());
  Klass *const ref_holder = cache_holder.constants()->resolved_klass_at(ref_holder_i);
  Symbol *const ref_name = cache_holder.constants()->uncached_name_ref_at(resolved_info.constant_pool_index());
  Symbol *const ref_sig = cache_holder.constants()->uncached_signature_ref_at(resolved_info.constant_pool_index());
  if (resolved_info.bytecode_1() != Bytecodes::_invokehandle) {
    // Resolved as:
    InstanceKlass *const holder = method->method_holder();
    const Symbol *name = method->name();
    const Symbol *sig = method->signature();
    // Must match
    assert(ref_name == name, "class %s, cache entry #%i: referenced %s != resolved %s",
           cache_holder.external_name(), resolved_info_i,
           Method::external_name(ref_holder, ref_name, ref_sig), method->external_name());
    if (!MethodHandles::is_signature_polymorphic_name(ref_holder, ref_name)) {
      assert(ref_sig == sig, "class %s, cache entry #%i: referenced %s != resolved %s",
             cache_holder.external_name(), resolved_info_i,
             Method::external_name(ref_holder, ref_name, ref_sig), method->external_name());
      assert(ref_holder->is_subtype_of(holder), "class %s, cache entry #%i (%s%s): referenced holder %s must subtype resolved holder %s",
             cache_holder.external_name(), resolved_info_i, name->as_C_string(), sig->as_C_string(),
             ref_holder->external_name(), holder->external_name());
    } else {
      assert(ref_holder == holder, "class %s, cache entry #%i (%s%s): holders %s and %s must match for a signature polymorhic method",
             cache_holder.external_name(), resolved_info_i, name->as_C_string(), sig->as_C_string(),
             ref_holder->external_name(), holder->external_name());
    }
  } else {
    assert(MethodHandles::is_signature_polymorphic_name(ref_holder, ref_name),
           "class %s, cache entry #%i: %s.%s(...) resolved as signature polymorphic",
           cache_holder.external_name(), resolved_info_i, ref_holder->external_name(), ref_name->as_C_string());
    if (MethodHandles::is_signature_polymorphic_intrinsic_name(ref_holder, ref_name)) {
      assert(MethodHandles::is_signature_polymorphic_method(method) &&
             MethodHandles::is_signature_polymorphic_intrinsic(method->intrinsic_id()),
             "class %s, cache entry #%i: %s.%s(...) resolved to %s",
             cache_holder.external_name(), resolved_info_i, ref_holder->external_name(), ref_name->as_C_string(),
             method->external_name());
    } else {
      // Generic case, resolves to a generated adapter
    }
  }

  assert(resolved_info.flag_state() == as_TosState(method->result_type()), "class %s, cache entry #%i (%s): %i != %i",
         cache_holder.external_name(), resolved_info_i, method->external_name(),
         resolved_info.flag_state(), as_TosState(method->result_type()));
  assert(resolved_info.parameter_size() == method->size_of_parameters(), "class %s, cache entry #%i (%s): %i != %i",
         cache_holder.external_name(), resolved_info_i, method->external_name(),
         resolved_info.parameter_size(), method->size_of_parameters());
  assert(!resolved_info.is_final() || method->is_final_method(), "class %s, cache entry #%i (%s): non-final cached as final",
         cache_holder.external_name(), resolved_info_i, method->external_name());
}

static void assert_correct_unresolved_method(const InstanceKlass &cache_holder, int unresolved_info_i) {
  const ConstantPoolCacheEntry &unresolved_info = *cache_holder.constants()->cache()->entry_at(unresolved_info_i);
  assert(unresolved_info.bytecode_1() == 0 && unresolved_info.bytecode_2() == 0, "resolved");

  assert(unresolved_info.flags_ord() == 0, "class %s, cache entry #%i: unresolved entry has flags = " INTX_FORMAT_X,
         cache_holder.external_name(), unresolved_info_i, unresolved_info.flags_ord());
  assert(unresolved_info.is_f1_null(), "class %s, cache entry #%i: unresolved entry has f1 = " PTR_FORMAT,
         cache_holder.external_name(), unresolved_info_i, p2i(unresolved_info.f1_ord()));

  // invokehandle entries have f2 set to appendix index even when unresolved
  const intx f2 = unresolved_info.f2_ord();
  if (f2 == 0) {
    return; // Note: this can still be an invokehandle entry
  }

  // Must be an invokehandle entry, i.e. reference a signature polymorpic method
  Symbol *const ref_holder_name = cache_holder.constants()->uncached_klass_ref_at_noresolve(unresolved_info.constant_pool_index());
  Symbol *const ref_name = cache_holder.constants()->uncached_name_ref_at(unresolved_info.constant_pool_index());
  InstanceKlass *ref_holder = nullptr;
  if (ref_holder_name == vmSymbols::java_lang_invoke_MethodHandle()) {
    if (!vmClasses::MethodHandle_klass_is_loaded()) {
      return;
    }
    ref_holder = vmClasses::MethodHandle_klass();
  } else if (ref_holder_name == vmSymbols::java_lang_invoke_VarHandle()) {
    if (!vmClasses::VarHandle_klass_is_loaded()) {
      return;
    }
    ref_holder = vmClasses::VarHandle_klass();
  }
  assert(ref_holder != nullptr, "class %s, cache entry #%i: %s.%s(...) resolved as signature polymorphic",
         cache_holder.external_name(), unresolved_info_i, ref_holder->external_name(), ref_name->as_C_string());
  assert(MethodHandles::is_signature_polymorphic_name(ref_holder, ref_name),
         "class %s, cache entry #%i: %s.%s(...) resolved as signature polymorphic",
         cache_holder.external_name(), unresolved_info_i, ref_holder->external_name(), ref_name->as_C_string());

  // Must be an index into resolved references array
  assert(f2 >= 0, "class %s, cache entry #%i: unresolved invokehandle entry has f2 = " PTR_FORMAT,
         cache_holder.external_name(), unresolved_info_i, p2i(unresolved_info.f1_ord()));
}

static void assert_correctly_resolved_indy(const InstanceKlass &cache_holder, int resolved_info_i) {
  const ResolvedIndyEntry &resolved_info = *cache_holder.constants()->cache()->resolved_indy_entry_at(resolved_info_i);

  const Method *adapter = resolved_info.method();

  assert(resolved_info.return_type() == as_TosState(adapter->result_type()), "class %s, indy entry #%i (adapter %s): %i != %i",
         cache_holder.external_name(), resolved_info_i, adapter->external_name(),
         resolved_info.return_type(), as_TosState(adapter->result_type()));
  assert(resolved_info.num_parameters() == adapter->size_of_parameters(), "class %s, indy entry #%i (adapter %s): %i != %i",
         cache_holder.external_name(), resolved_info_i, adapter->external_name(),
         resolved_info.num_parameters(), adapter->size_of_parameters());
}

#endif // ASSERT

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
  DEBUG_ONLY(ik->constants()->verify_on(nullptr));

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
    ResolvedFieldEntry &field_entry = *cp_cache.resolved_field_entry_at(field_ref.index);
    InstanceKlass **const holder = iks.get(field_ref.class_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved field entry #%i of %s",
              field_ref.class_id, field_ref.index, ik->external_name());
    assert(field_entry.field_index() < (*holder)->total_fields_count(),
           "class %s, field entry #%i: field holder %s, field index %i >= amount of fields in holder %i",
           ik->external_name(), field_ref.index, (*holder)->external_name(), field_entry.field_index(), (*holder)->total_fields_count());
    field_entry.fill_in_unportable(*holder);
    postcond(field_entry.field_holder() == *holder);
  }
  for (const InterclassRefs::MethodRef &method_ref : *refs.method_refs) {
    ConstantPoolCacheEntry &cache_entry = *cp_cache.entry_at(method_ref.cache_index);
    if (method_ref.f1_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const klass = iks.get(method_ref.f1_class_id);
      guarantee(klass != nullptr, "unknown class " HDID_FORMAT " referenced by f1 in resolved method entry #%i of %s",
                method_ref.f1_class_id, method_ref.cache_index, ik->external_name());
      if (method_ref.f1_is_method) {
        Symbol *const name = heap_dump.get_symbol(method_ref.f1_method_desc.name_id);
        Symbol *const sig = heap_dump.get_symbol(method_ref.f1_method_desc.sig_id);
        Method *const method = CracClassDumpParser::find_method(*klass, name, sig, method_ref.f1_method_desc.kind, true, CHECK);
        guarantee(method != nullptr, "class %s has a resolved method entry #%i with f1 referencing %s method %s that cannot be found",
                  ik->external_name(), method_ref.cache_index, MethodKind::name(method_ref.f1_method_desc.kind),
                  Method::name_and_sig_as_C_string(*klass, name, sig));
        cache_entry.set_f1(method);
        postcond(cache_entry.f1_as_method() == method);
      } else {
        cache_entry.set_f1(*klass);
        postcond(cache_entry.f1_as_klass() == *klass);
      }
    }
    if (method_ref.f2_class_id != HeapDump::NULL_ID) {
      InstanceKlass **const holder = iks.get(method_ref.f2_class_id);
      guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by f2 in resolved method entry #%i of %s",
                method_ref.f2_class_id, method_ref.cache_index, ik->external_name());

      Symbol *const name = heap_dump.get_symbol(method_ref.f2_method_desc.name_id);
      Symbol *const sig = heap_dump.get_symbol(method_ref.f2_method_desc.sig_id);
      Method *const method = CracClassDumpParser::find_method(*holder, name, sig, method_ref.f2_method_desc.kind, false, CHECK);
      guarantee(method != nullptr, "class %s has a resolved method entry #%i with f2 referencing %s method %s that cannot be found",
                ik->external_name(), method_ref.cache_index, MethodKind::name(method_ref.f2_method_desc.kind),
                Method::name_and_sig_as_C_string(*holder, name, sig));

#ifdef ASSERT
      if (!cache_entry.is_vfinal()) {
        assert((*holder)->is_interface(), "class %s, cache entry #%i, f2 as interface method: holder %s is not an interface",
               ik->external_name(), method_ref.cache_index, (*holder)->external_name());
        assert(!cache_entry.is_f1_null() && cache_entry.f1_as_klass()->is_klass() && cache_entry.f1_as_klass()->is_subtype_of(*holder),
               "class %s, cache entry #%i, f2 as interface method: f1 contains class %s which does not implement f2's method's holder %s",
               ik->external_name(), method_ref.cache_index,
               cache_entry.is_f1_null() ? "<null>" : (cache_entry.f1_as_klass()->is_klass() ? cache_entry.f1_as_klass()->external_name() : "<not a class>"),
               (*holder)->external_name());
      }
#endif // ASSERT

      cache_entry.set_f2(reinterpret_cast<intx>(method));
      postcond((cache_entry.is_vfinal() ? cache_entry.f2_as_vfinal_method() : cache_entry.f2_as_interface_method()) == method);
    }
  }
  for (const InterclassRefs::IndyAdapterRef &indy_ref : *refs.indy_refs) {
    ResolvedIndyEntry &indy_entry = *cp_cache.resolved_indy_entry_at(indy_ref.indy_index);
    precond(!indy_entry.resolution_failed());

    InstanceKlass **const holder = iks.get(indy_ref.holder_id);
    guarantee(holder != nullptr, "unknown class " HDID_FORMAT " referenced by resolved invokedynamic entry #%i of %s",
              indy_ref.holder_id, indy_ref.indy_index, ik->external_name());

    Symbol *const name = heap_dump.get_symbol(indy_ref.method_desc.name_id);
    Symbol *const sig = heap_dump.get_symbol(indy_ref.method_desc.sig_id);
    Method *const method = CracClassDumpParser::find_method(*holder, name, sig, indy_ref.method_desc.kind, false, CHECK);
    guarantee(method != nullptr, "class %s has a resolved invokedynamic entry #%i referencing %s method %s that cannot be found",
              ik->external_name(), indy_ref.indy_index, MethodKind::name(indy_ref.method_desc.kind),
              Method::name_and_sig_as_C_string(*holder, name, sig));

    indy_entry.adjust_method_entry(method);
    postcond(indy_entry.is_resolved() && indy_entry.method() == method);
  }

#ifdef ASSERT
  for (int i = 0; i < cp_cache.resolved_field_entries_length(); i++) {
    const ResolvedFieldEntry &field_entry = *cp_cache.resolved_field_entry_at(i);
    if (field_entry.get_code() != 0) {
      assert_correctly_resolved_field(*ik, i);
    }
  }
  for (int i = 0; i < cp_cache.length(); i++) {
    const ConstantPoolCacheEntry &cache_entry = *cp_cache.entry_at(i);
    if (cache_entry.bytecode_1() != 0 || cache_entry.bytecode_2() != 0) {
      assert_correctly_resolved_method(*ik, i);
    } else {
      assert_correct_unresolved_method(*ik, i);
    }
  }
  for (int i = 0; i < cp_cache.resolved_indy_entries_length(); i++) {
    const ResolvedIndyEntry &indy_entry = *cp_cache.resolved_indy_entry_at(i);
    if (indy_entry.is_resolved()) {
      assert_correctly_resolved_indy(*ik, i);
    }
  }
#endif // ASSERT
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

static void assert_interfaces_attempted_initialization(const InstanceKlass &initial, const InstanceKlass &current) {
  precond(initial.is_initialized() || initial.is_in_error_state());
  precond(current.has_nonstatic_concrete_methods());
  for (int i = 0; i < current.local_interfaces()->length(); i++) {
    const InstanceKlass &interface = *current.local_interfaces()->at(i);
    if (interface.declares_nonstatic_concrete_methods()) {
      assert(interface.is_initialized() || (current.is_in_error_state() && interface.is_in_error_state()),
             "%s %s %s but its implemented interface with non-static non-abstract methods %s %s",
             initial.is_interface() ? "interface" : "class", initial.external_name(),
             initial.is_initialized() ? "is initialized" : "has failed to initialize",
             interface.external_name(), initial.is_initialized() ? "is not" : "has not attempted to initialize");
    }
    if (interface.has_nonstatic_concrete_methods()) {
      assert_interfaces_attempted_initialization(initial, interface);
    }
  }
}

void CracClassStateRestorer::assert_hierarchy_init_states_are_consistent(const InstanceKlass &ik) {
  precond(!ik.is_being_restored());
  switch (ik.init_state()) {
    case InstanceKlass::allocated:
      ShouldNotReachHere(); // Too young
    case InstanceKlass::being_linked:
      // In case some other thread picked up the class after it has been restored
      precond(!const_cast<InstanceKlass &>(ik).is_init_thread(JavaThread::current()));
    case InstanceKlass::loaded:
      // If the class/interface is rewritten but not linked then either:
      // 1) it has failed its linkage in which case its super classes and
      //    interfaces must be linked, or
      // 2) it was loaded by CDS as rewritten right away in which case no
      //    linking has been attempted yet and its super classes and interfaces
      //    must also be rewritten (they should also be loaded by CDS).
      // We don't fully check (1) because it is a stricter check and for classes
      // restored from dump these two cases are indifferentiable (they are not
      // marked as CDS-loaded even if they were in the original VM).
      if (ik.is_rewritten()) {
        if (ik.java_super() != nullptr) {
          assert(ik.java_super()->is_rewritten(), "%s is rewritten but its super class %s is not",
                 ik.external_name(), ik.java_super()->external_name());
        }
        for (int i = 0; i < ik.local_interfaces()->length(); i++) {
          const InstanceKlass &interface = *ik.local_interfaces()->at(i);
          assert(ik.java_super()->is_rewritten(), "%s is rewritten but its implemented interface %s is not",
                 ik.external_name(), interface.external_name());
        }
      }
      return;
    case InstanceKlass::being_initialized:
      // In case some other thread picked up the class after it has been restored
      precond(!const_cast<InstanceKlass &>(ik).is_init_thread(JavaThread::current()));
    case InstanceKlass::linked:
      // Supers and interfaces of linked class/interface must be linked
      if (ik.java_super() != nullptr) {
        assert(ik.java_super()->is_linked(), "%s is linked but its super class %s is not",
                ik.external_name(), ik.java_super()->external_name());
      }
      for (int i = 0; i < ik.local_interfaces()->length(); i++) {
        const InstanceKlass &interface = *ik.local_interfaces()->at(i);
        assert(ik.java_super()->is_linked(), "%s is linked but its implemented interface %s is not",
                ik.external_name(), interface.external_name());
      }
      return;
    case InstanceKlass::fully_initialized:
    case InstanceKlass::initialization_error:
      // If this is a class (not interface) that has attempted initialization
      // then supers and interfaces with non-static non-abstract (aka default)
      // methods must have also attempted it (and succeeded, if the class has)
      if (!ik.is_interface()) {
        if (ik.java_super() != nullptr) {
          assert(ik.java_super()->is_initialized() || (ik.is_in_error_state() && ik.java_super()->is_in_error_state()),
                 "class %s %s but its super class %s %s",
                 ik.external_name(), ik.is_initialized() ? "is initialized" : "has failed to initialize",
                 ik.java_super()->external_name(), ik.is_initialized() ? "is not" : "has not attempted to initialize");
        }
        if (ik.has_nonstatic_concrete_methods()) {
          // Need to recursively check all interfaces because of situations like
          // "this class implements interface I1 w/o default methods which
          // implements interface I2 w/ default methods" -- I1 can will be
          // uninitialized but we should check I2 is initialized
          assert_interfaces_attempted_initialization(ik, ik);
        }
      }
      return;
    default:
      ShouldNotReachHere();
  }
}

#endif // ASSERT
