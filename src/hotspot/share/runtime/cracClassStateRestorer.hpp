#ifndef SHARE_UTILITIES_CRACCLASSSTATERESTORER_HPP
#define SHARE_UTILITIES_CRACCLASSSTATERESTORER_HPP

#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "oops/arrayKlass.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/handles.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/heapDumpParser.hpp"

// Class may reference other classes and while the class dump format guarantees
// that some of such references (class loader class, super class, etc.) will be
// created before the class itself, there is no such guarantee for all class
// references (since there may be cycles). Thus some interclass references can
// only be filled-in after all classes have been created.
struct InterclassRefs : public ResourceObj {
  // Restoring just the constant pool reference to the nest host is insufficient
  // if it is a dynamic nest host which does not come from the constant pool
  HeapDump::ID dynamic_nest_host;

  struct ClassRef {
    u2 index;                     // Contents depend on the context (see below)
    HeapDump::ID class_id;
  };
  struct MethodRefs {
    int cache_index;
    bool f1_is_method;
    HeapDump::ID f1_class_id;     // Null ID if unset
    u2 f1_method_idnum;           // Undefined if f1_is_method == false
    HeapDump::ID f2_class_id;     // Null ID if unset
    u2 f2_method_idnum;           // Undefined if f2_class_id is unset
  };
  struct IndyAdapterRef {
    int indy_index;
    HeapDump::ID holder_id;
    u2 method_idnum;
  };

  // Constant pool class references. Index is the constant pool index.
  GrowableArray<ClassRef> *cp_class_refs = new GrowableArray<ClassRef>();
  // Holders of resolved fields. Index is the resolved fields index.
  GrowableArray<ClassRef> *field_refs = new GrowableArray<ClassRef>();
  // Class/method references from resolved methods.
  GrowableArray<MethodRefs> *method_refs = new GrowableArray<MethodRefs>();
  // Adapter method references from resolved invokedynamics.
  GrowableArray<IndyAdapterRef> *indy_refs = new GrowableArray<IndyAdapterRef>();
};

struct CracClassStateRestorer : public AllStatic {
  // Defines the created class and makes the current thread hold its init state
  // if needed. Returns the defined class which may differ from the created one
  // iff the class has been pre-defined.
  static InstanceKlass *define_created_class(InstanceKlass *created_ik, InstanceKlass::ClassState target_state, TRAPS);

  static void fill_interclass_references(InstanceKlass *ik,
                                         const HeapDumpTable<InstanceKlass *, AnyObj::C_HEAP> &iks,
                                         const HeapDumpTable<ArrayKlass *, AnyObj::C_HEAP> &aks,
                                         const InterclassRefs &refs);

  static void apply_init_state(InstanceKlass *ik, InstanceKlass::ClassState state, Handle init_error);

  // Checks that initialization state of this class is consistent with the
  // states of its super class and implemented interfaces.
  static void assert_hierarchy_init_states_are_consistent(const InstanceKlass &ik) NOT_DEBUG_RETURN;
};

#endif // SHARE_UTILITIES_CRACCLASSSTATERESTORER_HPP
