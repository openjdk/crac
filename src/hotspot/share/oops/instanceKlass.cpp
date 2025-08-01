/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "cds/aotClassInitializer.hpp"
#include "cds/archiveUtils.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsEnumKlass.hpp"
#include "cds/classListWriter.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "classfile/classFileParser.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/verifier.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/dependencyContext.hpp"
#include "compiler/compilationPolicy.hpp"
#include "compiler/compileBroker.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/oopMapCache.hpp"
#include "interpreter/rewriter.hpp"
#include "jvm.h"
#include "jvmtifiles/jvmti.h"
#include "logging/log.hpp"
#include "klass.inline.hpp"
#include "logging/logMessage.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/fieldStreams.inline.hpp"
#include "oops/constantPool.hpp"
#include "oops/instanceClassLoaderKlass.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/instanceOop.hpp"
#include "oops/instanceStackChunkKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "oops/recordComponent.hpp"
#include "oops/symbol.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "prims/methodComparator.hpp"
#include "runtime/arguments.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/atomic.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/reflection.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/threads.hpp"
#include "services/classLoadingService.hpp"
#include "services/finalizerService.hpp"
#include "services/threadService.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/events.hpp"
#include "utilities/macros.hpp"
#include "utilities/nativeStackPrinter.hpp"
#include "utilities/stringUtils.hpp"
#ifdef COMPILER1
#include "c1/c1_Compiler.hpp"
#endif
#if INCLUDE_JFR
#include "jfr/jfrEvents.hpp"
#endif

#ifdef DTRACE_ENABLED


#define HOTSPOT_CLASS_INITIALIZATION_required HOTSPOT_CLASS_INITIALIZATION_REQUIRED
#define HOTSPOT_CLASS_INITIALIZATION_recursive HOTSPOT_CLASS_INITIALIZATION_RECURSIVE
#define HOTSPOT_CLASS_INITIALIZATION_concurrent HOTSPOT_CLASS_INITIALIZATION_CONCURRENT
#define HOTSPOT_CLASS_INITIALIZATION_erroneous HOTSPOT_CLASS_INITIALIZATION_ERRONEOUS
#define HOTSPOT_CLASS_INITIALIZATION_super__failed HOTSPOT_CLASS_INITIALIZATION_SUPER_FAILED
#define HOTSPOT_CLASS_INITIALIZATION_clinit HOTSPOT_CLASS_INITIALIZATION_CLINIT
#define HOTSPOT_CLASS_INITIALIZATION_error HOTSPOT_CLASS_INITIALIZATION_ERROR
#define HOTSPOT_CLASS_INITIALIZATION_end HOTSPOT_CLASS_INITIALIZATION_END
#define DTRACE_CLASSINIT_PROBE(type, thread_type)                \
  {                                                              \
    char* data = nullptr;                                        \
    int len = 0;                                                 \
    Symbol* clss_name = name();                                  \
    if (clss_name != nullptr) {                                  \
      data = (char*)clss_name->bytes();                          \
      len = clss_name->utf8_length();                            \
    }                                                            \
    HOTSPOT_CLASS_INITIALIZATION_##type(                         \
      data, len, (void*)class_loader(), thread_type);            \
  }

#define DTRACE_CLASSINIT_PROBE_WAIT(type, thread_type, wait)     \
  {                                                              \
    char* data = nullptr;                                        \
    int len = 0;                                                 \
    Symbol* clss_name = name();                                  \
    if (clss_name != nullptr) {                                  \
      data = (char*)clss_name->bytes();                          \
      len = clss_name->utf8_length();                            \
    }                                                            \
    HOTSPOT_CLASS_INITIALIZATION_##type(                         \
      data, len, (void*)class_loader(), thread_type, wait);      \
  }

#else //  ndef DTRACE_ENABLED

#define DTRACE_CLASSINIT_PROBE(type, thread_type)
#define DTRACE_CLASSINIT_PROBE_WAIT(type, thread_type, wait)

#endif //  ndef DTRACE_ENABLED

bool InstanceKlass::_finalization_enabled = true;

static inline bool is_class_loader(const Symbol* class_name,
                                   const ClassFileParser& parser) {
  assert(class_name != nullptr, "invariant");

  if (class_name == vmSymbols::java_lang_ClassLoader()) {
    return true;
  }

  if (vmClasses::ClassLoader_klass_loaded()) {
    const Klass* const super_klass = parser.super_klass();
    if (super_klass != nullptr) {
      if (super_klass->is_subtype_of(vmClasses::ClassLoader_klass())) {
        return true;
      }
    }
  }
  return false;
}

static inline bool is_stack_chunk_class(const Symbol* class_name,
                                        const ClassLoaderData* loader_data) {
  return (class_name == vmSymbols::jdk_internal_vm_StackChunk() &&
          loader_data->is_the_null_class_loader_data());
}

// private: called to verify that k is a static member of this nest.
// We know that k is an instance class in the same package and hence the
// same classloader.
bool InstanceKlass::has_nest_member(JavaThread* current, InstanceKlass* k) const {
  assert(!is_hidden(), "unexpected hidden class");
  if (_nest_members == nullptr || _nest_members == Universe::the_empty_short_array()) {
    if (log_is_enabled(Trace, class, nestmates)) {
      ResourceMark rm(current);
      log_trace(class, nestmates)("Checked nest membership of %s in non-nest-host class %s",
                                  k->external_name(), this->external_name());
    }
    return false;
  }

  if (log_is_enabled(Trace, class, nestmates)) {
    ResourceMark rm(current);
    log_trace(class, nestmates)("Checking nest membership of %s in %s",
                                k->external_name(), this->external_name());
  }

  // Check for the named class in _nest_members.
  // We don't resolve, or load, any classes.
  for (int i = 0; i < _nest_members->length(); i++) {
    int cp_index = _nest_members->at(i);
    Symbol* name = _constants->klass_name_at(cp_index);
    if (name == k->name()) {
      log_trace(class, nestmates)("- named class found at nest_members[%d] => cp[%d]", i, cp_index);
      return true;
    }
  }
  log_trace(class, nestmates)("- class is NOT a nest member!");
  return false;
}

// Called to verify that k is a permitted subclass of this class.
// The incoming stringStream is used to format the messages for error logging and for the caller
// to use for exception throwing.
bool InstanceKlass::has_as_permitted_subclass(const InstanceKlass* k, stringStream& ss) const {
  Thread* current = Thread::current();
  assert(k != nullptr, "sanity check");
  assert(_permitted_subclasses != nullptr && _permitted_subclasses != Universe::the_empty_short_array(),
         "unexpected empty _permitted_subclasses array");

  if (log_is_enabled(Trace, class, sealed)) {
    ResourceMark rm(current);
    log_trace(class, sealed)("Checking for permitted subclass %s in %s",
                             k->external_name(), this->external_name());
  }

  // Check that the class and its super are in the same module.
  if (k->module() != this->module()) {
    ss.print("Failed same module check: subclass %s is in module '%s' with loader %s, "
             "and sealed class %s is in module '%s' with loader %s",
             k->external_name(),
             k->module()->name_as_C_string(),
             k->module()->loader_data()->loader_name_and_id(),
             this->external_name(),
             this->module()->name_as_C_string(),
             this->module()->loader_data()->loader_name_and_id());
    log_trace(class, sealed)(" - %s", ss.as_string());
    return false;
  }

  if (!k->is_public() && !is_same_class_package(k)) {
    ss.print("Failed same package check: non-public subclass %s is in package '%s' with classloader %s, "
             "and sealed class %s is in package '%s' with classloader %s",
             k->external_name(),
             k->package() != nullptr ? k->package()->name()->as_C_string() : "unnamed",
             k->module()->loader_data()->loader_name_and_id(),
             this->external_name(),
             this->package() != nullptr ? this->package()->name()->as_C_string() : "unnamed",
             this->module()->loader_data()->loader_name_and_id());
    log_trace(class, sealed)(" - %s", ss.as_string());
    return false;
  }

  for (int i = 0; i < _permitted_subclasses->length(); i++) {
    int cp_index = _permitted_subclasses->at(i);
    Symbol* name = _constants->klass_name_at(cp_index);
    if (name == k->name()) {
      log_trace(class, sealed)("- Found it at permitted_subclasses[%d] => cp[%d]", i, cp_index);
      return true;
    }
  }

  ss.print("Failed listed permitted subclass check: class %s is not a permitted subclass of %s",
           k->external_name(), this->external_name());
  log_trace(class, sealed)(" - %s", ss.as_string());
  return false;
}

// Return nest-host class, resolving, validating and saving it if needed.
// In cases where this is called from a thread that cannot do classloading
// (such as a native JIT thread) then we simply return null, which in turn
// causes the access check to return false. Such code will retry the access
// from a more suitable environment later. Otherwise the _nest_host is always
// set once this method returns.
// Any errors from nest-host resolution must be preserved so they can be queried
// from higher-level access checking code, and reported as part of access checking
// exceptions.
// VirtualMachineErrors are propagated with a null return.
// Under any conditions where the _nest_host can be set to non-null the resulting
// value of it and, if applicable, the nest host resolution/validation error,
// are idempotent.
InstanceKlass* InstanceKlass::nest_host(TRAPS) {
  InstanceKlass* nest_host_k = _nest_host;
  if (nest_host_k != nullptr) {
    return nest_host_k;
  }

  ResourceMark rm(THREAD);

  // need to resolve and save our nest-host class.
  if (_nest_host_index != 0) { // we have a real nest_host
    // Before trying to resolve check if we're in a suitable context
    bool can_resolve = THREAD->can_call_java();
    if (!can_resolve && !_constants->tag_at(_nest_host_index).is_klass()) {
      log_trace(class, nestmates)("Rejected resolution of nest-host of %s in unsuitable thread",
                                  this->external_name());
      return nullptr; // sentinel to say "try again from a different context"
    }

    log_trace(class, nestmates)("Resolving nest-host of %s using cp entry for %s",
                                this->external_name(),
                                _constants->klass_name_at(_nest_host_index)->as_C_string());

    Klass* k = _constants->klass_at(_nest_host_index, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      if (PENDING_EXCEPTION->is_a(vmClasses::VirtualMachineError_klass())) {
        return nullptr; // propagate VMEs
      }
      stringStream ss;
      char* target_host_class = _constants->klass_name_at(_nest_host_index)->as_C_string();
      ss.print("Nest host resolution of %s with host %s failed: ",
               this->external_name(), target_host_class);
      java_lang_Throwable::print(PENDING_EXCEPTION, &ss);
      const char* msg = ss.as_string(true /* on C-heap */);
      constantPoolHandle cph(THREAD, constants());
      SystemDictionary::add_nest_host_error(cph, _nest_host_index, msg);
      CLEAR_PENDING_EXCEPTION;

      log_trace(class, nestmates)("%s", msg);
    } else {
      // A valid nest-host is an instance class in the current package that lists this
      // class as a nest member. If any of these conditions are not met the class is
      // its own nest-host.
      const char* error = nullptr;

      // JVMS 5.4.4 indicates package check comes first
      if (is_same_class_package(k)) {
        // Now check actual membership. We can't be a member if our "host" is
        // not an instance class.
        if (k->is_instance_klass()) {
          nest_host_k = InstanceKlass::cast(k);
          bool is_member = nest_host_k->has_nest_member(THREAD, this);
          if (is_member) {
            _nest_host = nest_host_k; // save resolved nest-host value

            log_trace(class, nestmates)("Resolved nest-host of %s to %s",
                                        this->external_name(), k->external_name());
            return nest_host_k;
          } else {
            error = "current type is not listed as a nest member";
          }
        } else {
          error = "host is not an instance class";
        }
      } else {
        error = "types are in different packages";
      }

      // something went wrong, so record what and log it
      {
        stringStream ss;
        ss.print("Type %s (loader: %s) is not a nest member of type %s (loader: %s): %s",
                 this->external_name(),
                 this->class_loader_data()->loader_name_and_id(),
                 k->external_name(),
                 k->class_loader_data()->loader_name_and_id(),
                 error);
        const char* msg = ss.as_string(true /* on C-heap */);
        constantPoolHandle cph(THREAD, constants());
        SystemDictionary::add_nest_host_error(cph, _nest_host_index, msg);
        log_trace(class, nestmates)("%s", msg);
      }
    }
  } else {
    log_trace(class, nestmates)("Type %s is not part of a nest: setting nest-host to self",
                                this->external_name());
  }

  // Either not in an explicit nest, or else an error occurred, so
  // the nest-host is set to `this`. Any thread that sees this assignment
  // will also see any setting of nest_host_error(), if applicable.
  return (_nest_host = this);
}

// Dynamic nest member support: set this class's nest host to the given class.
// This occurs as part of the class definition, as soon as the instanceKlass
// has been created and doesn't require further resolution. The code:
//    lookup().defineHiddenClass(bytes_for_X, NESTMATE);
// results in:
//    class_of_X.set_nest_host(lookup().lookupClass().getNestHost())
// If it has an explicit _nest_host_index or _nest_members, these will be ignored.
// We also know the "host" is a valid nest-host in the same package so we can
// assert some of those facts.
void InstanceKlass::set_nest_host(InstanceKlass* host) {
  assert(is_hidden(), "must be a hidden class");
  assert(host != nullptr, "null nest host specified");
  assert(_nest_host == nullptr, "current class has resolved nest-host");
  assert(nest_host_error() == nullptr, "unexpected nest host resolution error exists: %s",
         nest_host_error());
  assert((host->_nest_host == nullptr && host->_nest_host_index == 0) ||
         (host->_nest_host == host), "proposed host is not a valid nest-host");
  // Can't assert this as package is not set yet:
  // assert(is_same_class_package(host), "proposed host is in wrong package");

  if (log_is_enabled(Trace, class, nestmates)) {
    ResourceMark rm;
    const char* msg = "";
    // a hidden class does not expect a statically defined nest-host
    if (_nest_host_index > 0) {
      msg = "(the NestHost attribute in the current class is ignored)";
    } else if (_nest_members != nullptr && _nest_members != Universe::the_empty_short_array()) {
      msg = "(the NestMembers attribute in the current class is ignored)";
    }
    log_trace(class, nestmates)("Injected type %s into the nest of %s %s",
                                this->external_name(),
                                host->external_name(),
                                msg);
  }
  // set dynamic nest host
  _nest_host = host;
  // Record dependency to keep nest host from being unloaded before this class.
  ClassLoaderData* this_key = class_loader_data();
  assert(this_key != nullptr, "sanity");
  this_key->record_dependency(host);
}

// check if 'this' and k are nestmates (same nest_host), or k is our nest_host,
// or we are k's nest_host - all of which is covered by comparing the two
// resolved_nest_hosts.
// Any exceptions (i.e. VMEs) are propagated.
bool InstanceKlass::has_nestmate_access_to(InstanceKlass* k, TRAPS) {

  assert(this != k, "this should be handled by higher-level code");

  // Per JVMS 5.4.4 we first resolve and validate the current class, then
  // the target class k.

  InstanceKlass* cur_host = nest_host(CHECK_false);
  if (cur_host == nullptr) {
    return false;
  }

  Klass* k_nest_host = k->nest_host(CHECK_false);
  if (k_nest_host == nullptr) {
    return false;
  }

  bool access = (cur_host == k_nest_host);

  ResourceMark rm(THREAD);
  log_trace(class, nestmates)("Class %s does %shave nestmate access to %s",
                              this->external_name(),
                              access ? "" : "NOT ",
                              k->external_name());
  return access;
}

const char* InstanceKlass::nest_host_error() {
  if (_nest_host_index == 0) {
    return nullptr;
  } else {
    constantPoolHandle cph(Thread::current(), constants());
    return SystemDictionary::find_nest_host_error(cph, (int)_nest_host_index);
  }
}

void* InstanceKlass::operator new(size_t size, ClassLoaderData* loader_data, size_t word_size,
                                  bool use_class_space, TRAPS) throw() {
  return Metaspace::allocate(loader_data, word_size, ClassType, use_class_space, THREAD);
}

InstanceKlass* InstanceKlass::allocate_instance_klass(const ClassFileParser& parser, TRAPS) {
  const int size = InstanceKlass::size(parser.vtable_size(),
                                       parser.itable_size(),
                                       nonstatic_oop_map_size(parser.total_oop_map_count()),
                                       parser.is_interface());

  const Symbol* const class_name = parser.class_name();
  assert(class_name != nullptr, "invariant");
  ClassLoaderData* loader_data = parser.loader_data();
  assert(loader_data != nullptr, "invariant");

  InstanceKlass* ik;
  const bool use_class_space = parser.klass_needs_narrow_id();

  // Allocation
  if (parser.is_instance_ref_klass()) {
    // java.lang.ref.Reference
    ik = new (loader_data, size, use_class_space, THREAD) InstanceRefKlass(parser);
  } else if (class_name == vmSymbols::java_lang_Class()) {
    // mirror - java.lang.Class
    ik = new (loader_data, size, use_class_space, THREAD) InstanceMirrorKlass(parser);
  } else if (is_stack_chunk_class(class_name, loader_data)) {
    // stack chunk
    ik = new (loader_data, size, use_class_space, THREAD) InstanceStackChunkKlass(parser);
  } else if (is_class_loader(class_name, parser)) {
    // class loader - java.lang.ClassLoader
    ik = new (loader_data, size, use_class_space, THREAD) InstanceClassLoaderKlass(parser);
  } else {
    // normal
    ik = new (loader_data, size, use_class_space, THREAD) InstanceKlass(parser);
  }

  if (ik != nullptr && UseCompressedClassPointers && use_class_space) {
    assert(CompressedKlassPointers::is_encodable(ik),
           "Klass " PTR_FORMAT "needs a narrow Klass ID, but is not encodable", p2i(ik));
  }

  // Check for pending exception before adding to the loader data and incrementing
  // class count.  Can get OOM here.
  if (HAS_PENDING_EXCEPTION) {
    return nullptr;
  }

  return ik;
}


// copy method ordering from resource area to Metaspace
void InstanceKlass::copy_method_ordering(const intArray* m, TRAPS) {
  if (m != nullptr) {
    // allocate a new array and copy contents (memcpy?)
    _method_ordering = MetadataFactory::new_array<int>(class_loader_data(), m->length(), CHECK);
    for (int i = 0; i < m->length(); i++) {
      _method_ordering->at_put(i, m->at(i));
    }
  } else {
    _method_ordering = Universe::the_empty_int_array();
  }
}

// create a new array of vtable_indices for default methods
Array<int>* InstanceKlass::create_new_default_vtable_indices(int len, TRAPS) {
  Array<int>* vtable_indices = MetadataFactory::new_array<int>(class_loader_data(), len, CHECK_NULL);
  assert(default_vtable_indices() == nullptr, "only create once");
  set_default_vtable_indices(vtable_indices);
  return vtable_indices;
}


InstanceKlass::InstanceKlass() {
  assert(CDSConfig::is_dumping_static_archive() || CDSConfig::is_using_archive(), "only for CDS");
}

InstanceKlass::InstanceKlass(const ClassFileParser& parser, KlassKind kind, ReferenceType reference_type) :
  Klass(kind),
  _nest_members(nullptr),
  _nest_host(nullptr),
  _permitted_subclasses(nullptr),
  _record_components(nullptr),
  _static_field_size(parser.static_field_size()),
  _nonstatic_oop_map_size(nonstatic_oop_map_size(parser.total_oop_map_count())),
  _itable_len(parser.itable_size()),
  _nest_host_index(0),
  _init_state(allocated),
  _reference_type(reference_type),
  _init_thread(nullptr)
{
  set_vtable_length(parser.vtable_size());
  set_access_flags(parser.access_flags());
  if (parser.is_hidden()) set_is_hidden();
  set_layout_helper(Klass::instance_layout_helper(parser.layout_size(),
                                                    false));

  assert(nullptr == _methods, "underlying memory not zeroed?");
  assert(is_instance_klass(), "is layout incorrect?");
  assert(size_helper() == parser.layout_size(), "incorrect size_helper?");
}

void InstanceKlass::deallocate_methods(ClassLoaderData* loader_data,
                                       Array<Method*>* methods) {
  if (methods != nullptr && methods != Universe::the_empty_method_array() &&
      !methods->is_shared()) {
    for (int i = 0; i < methods->length(); i++) {
      Method* method = methods->at(i);
      if (method == nullptr) continue;  // maybe null if error processing
      // Only want to delete methods that are not executing for RedefineClasses.
      // The previous version will point to them so they're not totally dangling
      assert (!method->on_stack(), "shouldn't be called with methods on stack");
      MetadataFactory::free_metadata(loader_data, method);
    }
    MetadataFactory::free_array<Method*>(loader_data, methods);
  }
}

void InstanceKlass::deallocate_interfaces(ClassLoaderData* loader_data,
                                          const Klass* super_klass,
                                          Array<InstanceKlass*>* local_interfaces,
                                          Array<InstanceKlass*>* transitive_interfaces) {
  // Only deallocate transitive interfaces if not empty, same as super class
  // or same as local interfaces.  See code in parseClassFile.
  Array<InstanceKlass*>* ti = transitive_interfaces;
  if (ti != Universe::the_empty_instance_klass_array() && ti != local_interfaces) {
    // check that the interfaces don't come from super class
    Array<InstanceKlass*>* sti = (super_klass == nullptr) ? nullptr :
                    InstanceKlass::cast(super_klass)->transitive_interfaces();
    if (ti != sti && ti != nullptr && !ti->is_shared()) {
      MetadataFactory::free_array<InstanceKlass*>(loader_data, ti);
    }
  }

  // local interfaces can be empty
  if (local_interfaces != Universe::the_empty_instance_klass_array() &&
      local_interfaces != nullptr && !local_interfaces->is_shared()) {
    MetadataFactory::free_array<InstanceKlass*>(loader_data, local_interfaces);
  }
}

void InstanceKlass::deallocate_record_components(ClassLoaderData* loader_data,
                                                 Array<RecordComponent*>* record_components) {
  if (record_components != nullptr && !record_components->is_shared()) {
    for (int i = 0; i < record_components->length(); i++) {
      RecordComponent* record_component = record_components->at(i);
      MetadataFactory::free_metadata(loader_data, record_component);
    }
    MetadataFactory::free_array<RecordComponent*>(loader_data, record_components);
  }
}

// This function deallocates the metadata and C heap pointers that the
// InstanceKlass points to.
void InstanceKlass::deallocate_contents(ClassLoaderData* loader_data) {
  // Orphan the mirror first, CMS thinks it's still live.
  if (java_mirror() != nullptr) {
    java_lang_Class::set_klass(java_mirror(), nullptr);
  }

  // Also remove mirror from handles
  loader_data->remove_handle(_java_mirror);

  // Need to take this class off the class loader data list.
  loader_data->remove_class(this);

  // The array_klass for this class is created later, after error handling.
  // For class redefinition, we keep the original class so this scratch class
  // doesn't have an array class.  Either way, assert that there is nothing
  // to deallocate.
  assert(array_klasses() == nullptr, "array classes shouldn't be created for this class yet");

  // Release C heap allocated data that this points to, which includes
  // reference counting symbol names.
  // Can't release the constant pool or MethodData C heap data here because the constant
  // pool can be deallocated separately from the InstanceKlass for default methods and
  // redefine classes.  MethodData can also be released separately.
  release_C_heap_structures(/* release_sub_metadata */ false);

  deallocate_methods(loader_data, methods());
  set_methods(nullptr);

  deallocate_record_components(loader_data, record_components());
  set_record_components(nullptr);

  if (method_ordering() != nullptr &&
      method_ordering() != Universe::the_empty_int_array() &&
      !method_ordering()->is_shared()) {
    MetadataFactory::free_array<int>(loader_data, method_ordering());
  }
  set_method_ordering(nullptr);

  // default methods can be empty
  if (default_methods() != nullptr &&
      default_methods() != Universe::the_empty_method_array() &&
      !default_methods()->is_shared()) {
    MetadataFactory::free_array<Method*>(loader_data, default_methods());
  }
  // Do NOT deallocate the default methods, they are owned by superinterfaces.
  set_default_methods(nullptr);

  // default methods vtable indices can be empty
  if (default_vtable_indices() != nullptr &&
      !default_vtable_indices()->is_shared()) {
    MetadataFactory::free_array<int>(loader_data, default_vtable_indices());
  }
  set_default_vtable_indices(nullptr);


  // This array is in Klass, but remove it with the InstanceKlass since
  // this place would be the only caller and it can share memory with transitive
  // interfaces.
  if (secondary_supers() != nullptr &&
      secondary_supers() != Universe::the_empty_klass_array() &&
      // see comments in compute_secondary_supers about the following cast
      (address)(secondary_supers()) != (address)(transitive_interfaces()) &&
      !secondary_supers()->is_shared()) {
    MetadataFactory::free_array<Klass*>(loader_data, secondary_supers());
  }
  set_secondary_supers(nullptr, SECONDARY_SUPERS_BITMAP_EMPTY);

  deallocate_interfaces(loader_data, super(), local_interfaces(), transitive_interfaces());
  set_transitive_interfaces(nullptr);
  set_local_interfaces(nullptr);

  if (fieldinfo_stream() != nullptr && !fieldinfo_stream()->is_shared()) {
    MetadataFactory::free_array<u1>(loader_data, fieldinfo_stream());
  }
  set_fieldinfo_stream(nullptr);

  if (fields_status() != nullptr && !fields_status()->is_shared()) {
    MetadataFactory::free_array<FieldStatus>(loader_data, fields_status());
  }
  set_fields_status(nullptr);

  // If a method from a redefined class is using this constant pool, don't
  // delete it, yet.  The new class's previous version will point to this.
  if (constants() != nullptr) {
    assert (!constants()->on_stack(), "shouldn't be called if anything is onstack");
    if (!constants()->is_shared()) {
      MetadataFactory::free_metadata(loader_data, constants());
    }
    // Delete any cached resolution errors for the constant pool
    SystemDictionary::delete_resolution_error(constants());

    set_constants(nullptr);
  }

  if (inner_classes() != nullptr &&
      inner_classes() != Universe::the_empty_short_array() &&
      !inner_classes()->is_shared()) {
    MetadataFactory::free_array<jushort>(loader_data, inner_classes());
  }
  set_inner_classes(nullptr);

  if (nest_members() != nullptr &&
      nest_members() != Universe::the_empty_short_array() &&
      !nest_members()->is_shared()) {
    MetadataFactory::free_array<jushort>(loader_data, nest_members());
  }
  set_nest_members(nullptr);

  if (permitted_subclasses() != nullptr &&
      permitted_subclasses() != Universe::the_empty_short_array() &&
      !permitted_subclasses()->is_shared()) {
    MetadataFactory::free_array<jushort>(loader_data, permitted_subclasses());
  }
  set_permitted_subclasses(nullptr);

  // We should deallocate the Annotations instance if it's not in shared spaces.
  if (annotations() != nullptr && !annotations()->is_shared()) {
    MetadataFactory::free_metadata(loader_data, annotations());
  }
  set_annotations(nullptr);

  SystemDictionaryShared::handle_class_unloading(this);

#if INCLUDE_CDS_JAVA_HEAP
  if (CDSConfig::is_dumping_heap()) {
    HeapShared::remove_scratch_objects(this);
  }
#endif
}

bool InstanceKlass::is_record() const {
  return _record_components != nullptr &&
         is_final() &&
         java_super() == vmClasses::Record_klass();
}

bool InstanceKlass::is_sealed() const {
  return _permitted_subclasses != nullptr &&
         _permitted_subclasses != Universe::the_empty_short_array();
}

// JLS 8.9: An enum class is either implicitly final and derives
// from java.lang.Enum, or else is implicitly sealed to its
// anonymous subclasses. This query detects both kinds.
// It does not validate the finality or
// sealing conditions: it merely checks for a super of Enum.
// This is sufficient for recognizing well-formed enums.
bool InstanceKlass::is_enum_subclass() const {
  InstanceKlass* s = java_super();
  return (s == vmClasses::Enum_klass() ||
          (s != nullptr && s->java_super() == vmClasses::Enum_klass()));
}

bool InstanceKlass::should_be_initialized() const {
  return !is_initialized();
}

klassItable InstanceKlass::itable() const {
  return klassItable(const_cast<InstanceKlass*>(this));
}

// JVMTI spec thinks there are signers and protection domain in the
// instanceKlass.  These accessors pretend these fields are there.
// The hprof specification also thinks these fields are in InstanceKlass.
oop InstanceKlass::protection_domain() const {
  // return the protection_domain from the mirror
  return java_lang_Class::protection_domain(java_mirror());
}

objArrayOop InstanceKlass::signers() const {
  // return the signers from the mirror
  return java_lang_Class::signers(java_mirror());
}

oop InstanceKlass::init_lock() const {
  // return the init lock from the mirror
  oop lock = java_lang_Class::init_lock(java_mirror());
  // Prevent reordering with any access of initialization state
  OrderAccess::loadload();
  assert(lock != nullptr || !is_not_initialized(), // initialized or in_error state
         "only fully initialized state can have a null lock");
  return lock;
}

// Set the initialization lock to null so the object can be GC'ed.  Any racing
// threads to get this lock will see a null lock and will not lock.
// That's okay because they all check for initialized state after getting
// the lock and return.
void InstanceKlass::fence_and_clear_init_lock() {
  // make sure previous stores are all done, notably the init_state.
  OrderAccess::storestore();
  java_lang_Class::clear_init_lock(java_mirror());
  assert(!is_not_initialized(), "class must be initialized now");
}


// See "The Virtual Machine Specification" section 2.16.5 for a detailed explanation of the class initialization
// process. The step comments refers to the procedure described in that section.
// Note: implementation moved to static method to expose the this pointer.
void InstanceKlass::initialize(TRAPS) {
  if (this->should_be_initialized()) {
    initialize_impl(CHECK);
    // Note: at this point the class may be initialized
    //       OR it may be in the state of being initialized
    //       in case of recursive initialization!
  } else {
    assert(is_initialized(), "sanity check");
  }
}

#ifdef ASSERT
void InstanceKlass::assert_no_clinit_will_run_for_aot_initialized_class() const {
  assert(has_aot_initialized_mirror(), "must be");

  InstanceKlass* s = java_super();
  if (s != nullptr) {
    DEBUG_ONLY(ResourceMark rm);
    assert(s->is_initialized(), "super class %s of aot-inited class %s must have been initialized",
           s->external_name(), external_name());
    s->assert_no_clinit_will_run_for_aot_initialized_class();
  }

  Array<InstanceKlass*>* interfaces = local_interfaces();
  int len = interfaces->length();
  for (int i = 0; i < len; i++) {
    InstanceKlass* intf = interfaces->at(i);
    if (!intf->is_initialized()) {
      ResourceMark rm;
      // Note: an interface needs to be marked as is_initialized() only if
      // - it has a <clinit>
      // - it has declared a default method.
      assert(!intf->interface_needs_clinit_execution_as_super(/*also_check_supers*/false),
             "uninitialized super interface %s of aot-inited class %s must not have <clinit>",
             intf->external_name(), external_name());
    }
  }
}
#endif

#if INCLUDE_CDS
void InstanceKlass::initialize_with_aot_initialized_mirror(TRAPS) {
  assert(has_aot_initialized_mirror(), "must be");
  assert(CDSConfig::is_loading_heap(), "must be");
  assert(CDSConfig::is_using_aot_linked_classes(), "must be");
  assert_no_clinit_will_run_for_aot_initialized_class();

  if (is_initialized()) {
    return;
  }

  if (is_runtime_setup_required()) {
    // Need to take the slow path, which will call the runtimeSetup() function instead
    // of <clinit>
    initialize(CHECK);
    return;
  }
  if (log_is_enabled(Info, aot, init)) {
    ResourceMark rm;
    log_info(aot, init)("%s (aot-inited)", external_name());
  }

  link_class(CHECK);

#ifdef ASSERT
  {
    Handle h_init_lock(THREAD, init_lock());
    ObjectLocker ol(h_init_lock, THREAD);
    assert(!is_initialized(), "sanity");
    assert(!is_being_initialized(), "sanity");
    assert(!is_in_error_state(), "sanity");
  }
#endif

  set_init_thread(THREAD);
  set_initialization_state_and_notify(fully_initialized, CHECK);
}
#endif

bool InstanceKlass::verify_code(TRAPS) {
  // 1) Verify the bytecodes
  return Verifier::verify(this, should_verify_class(), THREAD);
}

void InstanceKlass::link_class(TRAPS) {
  assert(is_loaded(), "must be loaded");
  if (!is_linked()) {
    link_class_impl(CHECK);
  }
}

// Called to verify that a class can link during initialization, without
// throwing a VerifyError.
bool InstanceKlass::link_class_or_fail(TRAPS) {
  assert(is_loaded(), "must be loaded");
  if (!is_linked()) {
    link_class_impl(CHECK_false);
  }
  return is_linked();
}

bool InstanceKlass::link_class_impl(TRAPS) {
  if (CDSConfig::is_dumping_static_archive() && SystemDictionaryShared::has_class_failed_verification(this)) {
    // This is for CDS static dump only -- we use the in_error_state to indicate that
    // the class has failed verification. Throwing the NoClassDefFoundError here is just
    // a convenient way to stop repeat attempts to verify the same (bad) class.
    //
    // Note that the NoClassDefFoundError is not part of the JLS, and should not be thrown
    // if we are executing Java code. This is not a problem for CDS dumping phase since
    // it doesn't execute any Java code.
    ResourceMark rm(THREAD);
    // Names are all known to be < 64k so we know this formatted message is not excessively large.
    Exceptions::fthrow(THREAD_AND_LOCATION,
                       vmSymbols::java_lang_NoClassDefFoundError(),
                       "Class %s, or one of its supertypes, failed class initialization",
                       external_name());
    return false;
  }
  // return if already verified
  if (is_linked()) {
    return true;
  }

  // Timing
  // timer handles recursion
  JavaThread* jt = THREAD;

  // link super class before linking this class
  Klass* super_klass = super();
  if (super_klass != nullptr) {
    if (super_klass->is_interface()) {  // check if super class is an interface
      ResourceMark rm(THREAD);
      // Names are all known to be < 64k so we know this formatted message is not excessively large.
      Exceptions::fthrow(
        THREAD_AND_LOCATION,
        vmSymbols::java_lang_IncompatibleClassChangeError(),
        "class %s has interface %s as super class",
        external_name(),
        super_klass->external_name()
      );
      return false;
    }

    InstanceKlass* ik_super = InstanceKlass::cast(super_klass);
    ik_super->link_class_impl(CHECK_false);
  }

  // link all interfaces implemented by this class before linking this class
  Array<InstanceKlass*>* interfaces = local_interfaces();
  int num_interfaces = interfaces->length();
  for (int index = 0; index < num_interfaces; index++) {
    InstanceKlass* interk = interfaces->at(index);
    interk->link_class_impl(CHECK_false);
  }

  // in case the class is linked in the process of linking its superclasses
  if (is_linked()) {
    return true;
  }

  // trace only the link time for this klass that includes
  // the verification time
  PerfClassTraceTime vmtimer(ClassLoader::perf_class_link_time(),
                             ClassLoader::perf_class_link_selftime(),
                             ClassLoader::perf_classes_linked(),
                             jt->get_thread_stat()->perf_recursion_counts_addr(),
                             jt->get_thread_stat()->perf_timers_addr(),
                             PerfClassTraceTime::CLASS_LINK);

  // verification & rewriting
  {
    HandleMark hm(THREAD);
    Handle h_init_lock(THREAD, init_lock());
    ObjectLocker ol(h_init_lock, jt);
    // rewritten will have been set if loader constraint error found
    // on an earlier link attempt
    // don't verify or rewrite if already rewritten
    //

    if (!is_linked()) {
      if (!is_rewritten()) {
        if (is_shared()) {
          assert(!verified_at_dump_time(), "must be");
        }
        {
          bool verify_ok = verify_code(THREAD);
          if (!verify_ok) {
            return false;
          }
        }

        // Just in case a side-effect of verify linked this class already
        // (which can sometimes happen since the verifier loads classes
        // using custom class loaders, which are free to initialize things)
        if (is_linked()) {
          return true;
        }

        // also sets rewritten
        rewrite_class(CHECK_false);
      } else if (is_shared()) {
        SystemDictionaryShared::check_verification_constraints(this, CHECK_false);
      }

      // relocate jsrs and link methods after they are all rewritten
      link_methods(CHECK_false);

      // Initialize the vtable and interface table after
      // methods have been rewritten since rewrite may
      // fabricate new Method*s.
      // also does loader constraint checking
      //
      // initialize_vtable and initialize_itable need to be rerun
      // for a shared class if
      // 1) the class is loaded by custom class loader or
      // 2) the class is loaded by built-in class loader but failed to add archived loader constraints or
      // 3) the class was not verified during dump time
      bool need_init_table = true;
      if (is_shared() && verified_at_dump_time() &&
          SystemDictionaryShared::check_linking_constraints(THREAD, this)) {
        need_init_table = false;
      }
      if (need_init_table) {
        vtable().initialize_vtable_and_check_constraints(CHECK_false);
        itable().initialize_itable_and_check_constraints(CHECK_false);
      }
#ifdef ASSERT
      vtable().verify(tty, true);
      // In case itable verification is ever added.
      // itable().verify(tty, true);
#endif
      if (Universe::is_fully_initialized()) {
        DeoptimizationScope deopt_scope;
        {
          // Now mark all code that assumes the class is not linked.
          // Set state under the Compile_lock also.
          MutexLocker ml(THREAD, Compile_lock);

          set_init_state(linked);
          CodeCache::mark_dependents_on(&deopt_scope, this);
        }
        // Perform the deopt handshake outside Compile_lock.
        deopt_scope.deoptimize_marked();
      } else {
        set_init_state(linked);
      }
      if (JvmtiExport::should_post_class_prepare()) {
        JvmtiExport::post_class_prepare(THREAD, this);
      }
    }
  }
  return true;
}

// Rewrite the byte codes of all of the methods of a class.
// The rewriter must be called exactly once. Rewriting must happen after
// verification but before the first method of the class is executed.
void InstanceKlass::rewrite_class(TRAPS) {
  assert(is_loaded(), "must be loaded");
  if (is_rewritten()) {
    assert(is_shared(), "rewriting an unshared class?");
    return;
  }
  Rewriter::rewrite(this, CHECK);
  set_rewritten();
}

// Now relocate and link method entry points after class is rewritten.
// This is outside is_rewritten flag. In case of an exception, it can be
// executed more than once.
void InstanceKlass::link_methods(TRAPS) {
  PerfTraceTime timer(ClassLoader::perf_ik_link_methods_time());

  int len = methods()->length();
  for (int i = len-1; i >= 0; i--) {
    methodHandle m(THREAD, methods()->at(i));

    // Set up method entry points for compiler and interpreter    .
    m->link_method(m, CHECK);
  }
}

// Eagerly initialize superinterfaces that declare default methods (concrete instance: any access)
void InstanceKlass::initialize_super_interfaces(TRAPS) {
  assert (has_nonstatic_concrete_methods(), "caller should have checked this");
  for (int i = 0; i < local_interfaces()->length(); ++i) {
    InstanceKlass* ik = local_interfaces()->at(i);

    // Initialization is depth first search ie. we start with top of the inheritance tree
    // has_nonstatic_concrete_methods drives searching superinterfaces since it
    // means has_nonstatic_concrete_methods in its superinterface hierarchy
    if (ik->has_nonstatic_concrete_methods()) {
      ik->initialize_super_interfaces(CHECK);
    }

    // Only initialize() interfaces that "declare" concrete methods.
    if (ik->should_be_initialized() && ik->declares_nonstatic_concrete_methods()) {
      ik->initialize(CHECK);
    }
  }
}

using InitializationErrorTable = ResourceHashtable<const InstanceKlass*, OopHandle, 107, AnyObj::C_HEAP, mtClass>;
static InitializationErrorTable* _initialization_error_table;

void InstanceKlass::add_initialization_error(JavaThread* current, Handle exception) {
  // Create the same exception with a message indicating the thread name,
  // and the StackTraceElements.
  Handle init_error = java_lang_Throwable::create_initialization_error(current, exception);
  ResourceMark rm(current);
  if (init_error.is_null()) {
    log_trace(class, init)("Unable to create the desired initialization error for class %s", external_name());

    // We failed to create the new exception, most likely due to either out-of-memory or
    // a stackoverflow error. If the original exception was either of those then we save
    // the shared, pre-allocated, stackless, instance of that exception.
    if (exception->klass() == vmClasses::StackOverflowError_klass()) {
      log_debug(class, init)("Using shared StackOverflowError as initialization error for class %s", external_name());
      init_error = Handle(current, Universe::class_init_stack_overflow_error());
    } else if (exception->klass() == vmClasses::OutOfMemoryError_klass()) {
      log_debug(class, init)("Using shared OutOfMemoryError as initialization error for class %s", external_name());
      init_error = Handle(current, Universe::class_init_out_of_memory_error());
    } else {
      return;
    }
  }

  MutexLocker ml(current, ClassInitError_lock);
  OopHandle elem = OopHandle(Universe::vm_global(), init_error());
  bool created;
  if (_initialization_error_table == nullptr) {
    _initialization_error_table = new (mtClass) InitializationErrorTable();
  }
  _initialization_error_table->put_if_absent(this, elem, &created);
  assert(created, "Initialization is single threaded");
  log_trace(class, init)("Initialization error added for class %s", external_name());
}

oop InstanceKlass::get_initialization_error(JavaThread* current) {
  MutexLocker ml(current, ClassInitError_lock);
  if (_initialization_error_table == nullptr) {
    return nullptr;
  }
  OopHandle* h = _initialization_error_table->get(this);
  return (h != nullptr) ? h->resolve() : nullptr;
}

// Need to remove entries for unloaded classes.
void InstanceKlass::clean_initialization_error_table() {
  struct InitErrorTableCleaner {
    bool do_entry(const InstanceKlass* ik, OopHandle h) {
      if (!ik->is_loader_alive()) {
        h.release(Universe::vm_global());
        return true;
      } else {
        return false;
      }
    }
  };

  assert_locked_or_safepoint(ClassInitError_lock);
  InitErrorTableCleaner cleaner;
  if (_initialization_error_table != nullptr) {
    _initialization_error_table->unlink(&cleaner);
  }
}

void InstanceKlass::initialize_impl(TRAPS) {
  HandleMark hm(THREAD);

  // Make sure klass is linked (verified) before initialization
  // A class could already be verified, since it has been reflected upon.
  link_class(CHECK);

  DTRACE_CLASSINIT_PROBE(required, -1);

  bool wait = false;

  JavaThread* jt = THREAD;

  bool debug_logging_enabled = log_is_enabled(Debug, class, init);

  // refer to the JVM book page 47 for description of steps
  // Step 1
  {
    Handle h_init_lock(THREAD, init_lock());
    ObjectLocker ol(h_init_lock, jt);

    // Step 2
    // If we were to use wait() instead of waitInterruptibly() then
    // we might end up throwing IE from link/symbol resolution sites
    // that aren't expected to throw.  This would wreak havoc.  See 6320309.
    while (is_being_initialized() && !is_reentrant_initialization(jt)) {
      if (debug_logging_enabled) {
        ResourceMark rm(jt);
        log_debug(class, init)("Thread \"%s\" waiting for initialization of %s by thread \"%s\"",
                               jt->name(), external_name(), init_thread_name());
      }
      wait = true;
      jt->set_class_to_be_initialized(this);
      ol.wait_uninterruptibly(jt);
      jt->set_class_to_be_initialized(nullptr);
    }

    // Step 3
    if (is_being_initialized() && is_reentrant_initialization(jt)) {
      if (debug_logging_enabled) {
        ResourceMark rm(jt);
        log_debug(class, init)("Thread \"%s\" recursively initializing %s",
                               jt->name(), external_name());
      }
      DTRACE_CLASSINIT_PROBE_WAIT(recursive, -1, wait);
      return;
    }

    // Step 4
    if (is_initialized()) {
      if (debug_logging_enabled) {
        ResourceMark rm(jt);
        log_debug(class, init)("Thread \"%s\" found %s already initialized",
                               jt->name(), external_name());
      }
      DTRACE_CLASSINIT_PROBE_WAIT(concurrent, -1, wait);
      return;
    }

    // Step 5
    if (is_in_error_state()) {
      if (debug_logging_enabled) {
        ResourceMark rm(jt);
        log_debug(class, init)("Thread \"%s\" found %s is in error state",
                               jt->name(), external_name());
      }

      DTRACE_CLASSINIT_PROBE_WAIT(erroneous, -1, wait);
      ResourceMark rm(THREAD);
      Handle cause(THREAD, get_initialization_error(THREAD));

      stringStream ss;
      ss.print("Could not initialize class %s", external_name());
      if (cause.is_null()) {
        THROW_MSG(vmSymbols::java_lang_NoClassDefFoundError(), ss.as_string());
      } else {
        THROW_MSG_CAUSE(vmSymbols::java_lang_NoClassDefFoundError(),
                        ss.as_string(), cause);
      }
    } else {

      // Step 6
      set_init_state(being_initialized);
      set_init_thread(jt);
      if (debug_logging_enabled) {
        ResourceMark rm(jt);
        log_debug(class, init)("Thread \"%s\" is initializing %s",
                               jt->name(), external_name());
      }
    }
  }

  // Step 7
  // Next, if C is a class rather than an interface, initialize it's super class and super
  // interfaces.
  if (!is_interface()) {
    Klass* super_klass = super();
    if (super_klass != nullptr && super_klass->should_be_initialized()) {
      super_klass->initialize(THREAD);
    }
    // If C implements any interface that declares a non-static, concrete method,
    // the initialization of C triggers initialization of its super interfaces.
    // Only need to recurse if has_nonstatic_concrete_methods which includes declaring and
    // having a superinterface that declares, non-static, concrete methods
    if (!HAS_PENDING_EXCEPTION && has_nonstatic_concrete_methods()) {
      initialize_super_interfaces(THREAD);
    }

    // If any exceptions, complete abruptly, throwing the same exception as above.
    if (HAS_PENDING_EXCEPTION) {
      Handle e(THREAD, PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      {
        EXCEPTION_MARK;
        add_initialization_error(THREAD, e);
        // Locks object, set state, and notify all waiting threads
        set_initialization_state_and_notify(initialization_error, THREAD);
        CLEAR_PENDING_EXCEPTION;
      }
      DTRACE_CLASSINIT_PROBE_WAIT(super__failed, -1, wait);
      THROW_OOP(e());
    }
  }


  // Step 8
  {
    DTRACE_CLASSINIT_PROBE_WAIT(clinit, -1, wait);
    if (class_initializer() != nullptr) {
      // Timer includes any side effects of class initialization (resolution,
      // etc), but not recursive entry into call_class_initializer().
      PerfClassTraceTime timer(ClassLoader::perf_class_init_time(),
                               ClassLoader::perf_class_init_selftime(),
                               ClassLoader::perf_classes_inited(),
                               jt->get_thread_stat()->perf_recursion_counts_addr(),
                               jt->get_thread_stat()->perf_timers_addr(),
                               PerfClassTraceTime::CLASS_CLINIT);
      call_class_initializer(THREAD);
    } else {
      // The elapsed time is so small it's not worth counting.
      if (UsePerfData) {
        ClassLoader::perf_classes_inited()->inc();
      }
      call_class_initializer(THREAD);
    }
  }

  // Step 9
  if (!HAS_PENDING_EXCEPTION) {
    set_initialization_state_and_notify(fully_initialized, CHECK);
    DEBUG_ONLY(vtable().verify(tty, true);)
    CompilationPolicy::replay_training_at_init(this, THREAD);
  }
  else {
    // Step 10 and 11
    Handle e(THREAD, PENDING_EXCEPTION);
    CLEAR_PENDING_EXCEPTION;
    // JVMTI has already reported the pending exception
    // JVMTI internal flag reset is needed in order to report ExceptionInInitializerError
    JvmtiExport::clear_detected_exception(jt);
    {
      EXCEPTION_MARK;
      add_initialization_error(THREAD, e);
      set_initialization_state_and_notify(initialization_error, THREAD);
      CLEAR_PENDING_EXCEPTION;   // ignore any exception thrown, class initialization error is thrown below
      // JVMTI has already reported the pending exception
      // JVMTI internal flag reset is needed in order to report ExceptionInInitializerError
      JvmtiExport::clear_detected_exception(jt);
    }
    DTRACE_CLASSINIT_PROBE_WAIT(error, -1, wait);
    if (e->is_a(vmClasses::Error_klass())) {
      THROW_OOP(e());
    } else {
      JavaCallArguments args(e);
      THROW_ARG(vmSymbols::java_lang_ExceptionInInitializerError(),
                vmSymbols::throwable_void_signature(),
                &args);
    }
  }
  DTRACE_CLASSINIT_PROBE_WAIT(end, -1, wait);
}


void InstanceKlass::set_initialization_state_and_notify(ClassState state, TRAPS) {
  Handle h_init_lock(THREAD, init_lock());
  if (h_init_lock() != nullptr) {
    ObjectLocker ol(h_init_lock, THREAD);
    set_init_thread(nullptr); // reset _init_thread before changing _init_state
    set_init_state(state);
    fence_and_clear_init_lock();
    ol.notify_all(CHECK);
  } else {
    assert(h_init_lock() != nullptr, "The initialization state should never be set twice");
    set_init_thread(nullptr); // reset _init_thread before changing _init_state
    set_init_state(state);
  }
}

// Update hierarchy. This is done before the new klass has been added to the SystemDictionary. The Compile_lock
// is grabbed, to ensure that the compiler is not using the class hierarchy.
void InstanceKlass::add_to_hierarchy(JavaThread* current) {
  assert(!SafepointSynchronize::is_at_safepoint(), "must NOT be at safepoint");

  DeoptimizationScope deopt_scope;
  {
    MutexLocker ml(current, Compile_lock);

    set_init_state(InstanceKlass::loaded);
    // make sure init_state store is already done.
    // The compiler reads the hierarchy outside of the Compile_lock.
    // Access ordering is used to add to hierarchy.

    // Link into hierarchy.
    append_to_sibling_list();                    // add to superklass/sibling list
    process_interfaces();                        // handle all "implements" declarations

    // Now mark all code that depended on old class hierarchy.
    // Note: must be done *after* linking k into the hierarchy (was bug 12/9/97)
    if (Universe::is_fully_initialized()) {
      CodeCache::mark_dependents_on(&deopt_scope, this);
    }
  }
  // Perform the deopt handshake outside Compile_lock.
  deopt_scope.deoptimize_marked();
}


InstanceKlass* InstanceKlass::implementor() const {
  InstanceKlass* volatile* ik = adr_implementor();
  if (ik == nullptr) {
    return nullptr;
  } else {
    // This load races with inserts, and therefore needs acquire.
    InstanceKlass* ikls = Atomic::load_acquire(ik);
    if (ikls != nullptr && !ikls->is_loader_alive()) {
      return nullptr;  // don't return unloaded class
    } else {
      return ikls;
    }
  }
}


void InstanceKlass::set_implementor(InstanceKlass* ik) {
  assert_locked_or_safepoint(Compile_lock);
  assert(is_interface(), "not interface");
  InstanceKlass* volatile* addr = adr_implementor();
  assert(addr != nullptr, "null addr");
  if (addr != nullptr) {
    Atomic::release_store(addr, ik);
  }
}

int  InstanceKlass::nof_implementors() const {
  InstanceKlass* ik = implementor();
  if (ik == nullptr) {
    return 0;
  } else if (ik != this) {
    return 1;
  } else {
    return 2;
  }
}

// The embedded _implementor field can only record one implementor.
// When there are more than one implementors, the _implementor field
// is set to the interface Klass* itself. Following are the possible
// values for the _implementor field:
//   null                  - no implementor
//   implementor Klass*    - one implementor
//   self                  - more than one implementor
//
// The _implementor field only exists for interfaces.
void InstanceKlass::add_implementor(InstanceKlass* ik) {
  if (Universe::is_fully_initialized()) {
    assert_lock_strong(Compile_lock);
  }
  assert(is_interface(), "not interface");
  // Filter out my subinterfaces.
  // (Note: Interfaces are never on the subklass list.)
  if (ik->is_interface()) return;

  // Filter out subclasses whose supers already implement me.
  // (Note: CHA must walk subclasses of direct implementors
  // in order to locate indirect implementors.)
  InstanceKlass* super_ik = ik->java_super();
  if (super_ik != nullptr && super_ik->implements_interface(this))
    // We only need to check one immediate superclass, since the
    // implements_interface query looks at transitive_interfaces.
    // Any supers of the super have the same (or fewer) transitive_interfaces.
    return;

  InstanceKlass* iklass = implementor();
  if (iklass == nullptr) {
    set_implementor(ik);
  } else if (iklass != this && iklass != ik) {
    // There is already an implementor. Use itself as an indicator of
    // more than one implementors.
    set_implementor(this);
  }

  // The implementor also implements the transitive_interfaces
  for (int index = 0; index < local_interfaces()->length(); index++) {
    local_interfaces()->at(index)->add_implementor(ik);
  }
}

void InstanceKlass::init_implementor() {
  if (is_interface()) {
    set_implementor(nullptr);
  }
}


void InstanceKlass::process_interfaces() {
  // link this class into the implementors list of every interface it implements
  for (int i = local_interfaces()->length() - 1; i >= 0; i--) {
    assert(local_interfaces()->at(i)->is_klass(), "must be a klass");
    InstanceKlass* interf = local_interfaces()->at(i);
    assert(interf->is_interface(), "expected interface");
    interf->add_implementor(this);
  }
}

bool InstanceKlass::can_be_primary_super_slow() const {
  if (is_interface())
    return false;
  else
    return Klass::can_be_primary_super_slow();
}

GrowableArray<Klass*>* InstanceKlass::compute_secondary_supers(int num_extra_slots,
                                                               Array<InstanceKlass*>* transitive_interfaces) {
  // The secondaries are the implemented interfaces.
  // We need the cast because Array<Klass*> is NOT a supertype of Array<InstanceKlass*>,
  // (but it's safe to do here because we won't write into _secondary_supers from this point on).
  Array<Klass*>* interfaces = (Array<Klass*>*)(address)transitive_interfaces;
  int num_secondaries = num_extra_slots + interfaces->length();
  if (num_secondaries == 0) {
    // Must share this for correct bootstrapping!
    set_secondary_supers(Universe::the_empty_klass_array(), Universe::the_empty_klass_bitmap());
    return nullptr;
  } else if (num_extra_slots == 0 && interfaces->length() <= 1) {
    // We will reuse the transitive interfaces list if we're certain
    // it's in hash order.
    uintx bitmap = compute_secondary_supers_bitmap(interfaces);
    set_secondary_supers(interfaces, bitmap);
    return nullptr;
  }
  // Copy transitive interfaces to a temporary growable array to be constructed
  // into the secondary super list with extra slots.
  GrowableArray<Klass*>* secondaries = new GrowableArray<Klass*>(interfaces->length());
  for (int i = 0; i < interfaces->length(); i++) {
    secondaries->push(interfaces->at(i));
  }
  return secondaries;
}

bool InstanceKlass::implements_interface(Klass* k) const {
  if (this == k) return true;
  assert(k->is_interface(), "should be an interface class");
  for (int i = 0; i < transitive_interfaces()->length(); i++) {
    if (transitive_interfaces()->at(i) == k) {
      return true;
    }
  }
  return false;
}

bool InstanceKlass::is_same_or_direct_interface(Klass *k) const {
  // Verify direct super interface
  if (this == k) return true;
  assert(k->is_interface(), "should be an interface class");
  for (int i = 0; i < local_interfaces()->length(); i++) {
    if (local_interfaces()->at(i) == k) {
      return true;
    }
  }
  return false;
}

objArrayOop InstanceKlass::allocate_objArray(int n, int length, TRAPS) {
  check_array_allocation_length(length, arrayOopDesc::max_array_length(T_OBJECT), CHECK_NULL);
  size_t size = objArrayOopDesc::object_size(length);
  ArrayKlass* ak = array_klass(n, CHECK_NULL);
  objArrayOop o = (objArrayOop)Universe::heap()->array_allocate(ak, size, length,
                                                                /* do_zero */ true, CHECK_NULL);
  return o;
}

instanceOop InstanceKlass::register_finalizer(instanceOop i, TRAPS) {
  if (TraceFinalizerRegistration) {
    tty->print("Registered ");
    i->print_value_on(tty);
    tty->print_cr(" (" PTR_FORMAT ") as finalizable", p2i(i));
  }
  instanceHandle h_i(THREAD, i);
  // Pass the handle as argument, JavaCalls::call expects oop as jobjects
  JavaValue result(T_VOID);
  JavaCallArguments args(h_i);
  methodHandle mh(THREAD, Universe::finalizer_register_method());
  JavaCalls::call(&result, mh, &args, CHECK_NULL);
  MANAGEMENT_ONLY(FinalizerService::on_register(h_i(), THREAD);)
  return h_i();
}

instanceOop InstanceKlass::allocate_instance(TRAPS) {
  assert(!is_abstract() && !is_interface(), "Should not create this object");
  size_t size = size_helper();  // Query before forming handle.
  return (instanceOop)Universe::heap()->obj_allocate(this, size, CHECK_NULL);
}

instanceOop InstanceKlass::allocate_instance(oop java_class, TRAPS) {
  Klass* k = java_lang_Class::as_Klass(java_class);
  if (k == nullptr) {
    ResourceMark rm(THREAD);
    THROW_(vmSymbols::java_lang_InstantiationException(), nullptr);
  }
  InstanceKlass* ik = cast(k);
  ik->check_valid_for_instantiation(false, CHECK_NULL);
  ik->initialize(CHECK_NULL);
  return ik->allocate_instance(THREAD);
}

instanceHandle InstanceKlass::allocate_instance_handle(TRAPS) {
  return instanceHandle(THREAD, allocate_instance(THREAD));
}

void InstanceKlass::check_valid_for_instantiation(bool throwError, TRAPS) {
  if (is_interface() || is_abstract()) {
    ResourceMark rm(THREAD);
    THROW_MSG(throwError ? vmSymbols::java_lang_InstantiationError()
              : vmSymbols::java_lang_InstantiationException(), external_name());
  }
  if (this == vmClasses::Class_klass()) {
    ResourceMark rm(THREAD);
    THROW_MSG(throwError ? vmSymbols::java_lang_IllegalAccessError()
              : vmSymbols::java_lang_IllegalAccessException(), external_name());
  }
}

ArrayKlass* InstanceKlass::array_klass(int n, TRAPS) {
  // Need load-acquire for lock-free read
  if (array_klasses_acquire() == nullptr) {

    // Recursively lock array allocation
    RecursiveLocker rl(MultiArray_lock, THREAD);

    // Check if another thread created the array klass while we were waiting for the lock.
    if (array_klasses() == nullptr) {
      ObjArrayKlass* k = ObjArrayKlass::allocate_objArray_klass(class_loader_data(), 1, this, CHECK_NULL);
      // use 'release' to pair with lock-free load
      release_set_array_klasses(k);
    }
  }

  // array_klasses() will always be set at this point
  ObjArrayKlass* ak = array_klasses();
  assert(ak != nullptr, "should be set");
  return ak->array_klass(n, THREAD);
}

ArrayKlass* InstanceKlass::array_klass_or_null(int n) {
  // Need load-acquire for lock-free read
  ObjArrayKlass* oak = array_klasses_acquire();
  if (oak == nullptr) {
    return nullptr;
  } else {
    return oak->array_klass_or_null(n);
  }
}

ArrayKlass* InstanceKlass::array_klass(TRAPS) {
  return array_klass(1, THREAD);
}

ArrayKlass* InstanceKlass::array_klass_or_null() {
  return array_klass_or_null(1);
}

static int call_class_initializer_counter = 0;   // for debugging

Method* InstanceKlass::class_initializer() const {
  Method* clinit = find_method(
      vmSymbols::class_initializer_name(), vmSymbols::void_method_signature());
  if (clinit != nullptr && clinit->has_valid_initializer_flags()) {
    return clinit;
  }
  return nullptr;
}

void InstanceKlass::call_class_initializer(TRAPS) {
  if (ReplayCompiles &&
      (ReplaySuppressInitializers == 1 ||
       (ReplaySuppressInitializers >= 2 && class_loader() != nullptr))) {
    // Hide the existence of the initializer for the purpose of replaying the compile
    return;
  }

#if INCLUDE_CDS
  // This is needed to ensure the consistency of the archived heap objects.
  if (has_aot_initialized_mirror() && CDSConfig::is_loading_heap()) {
    AOTClassInitializer::call_runtime_setup(THREAD, this);
    return;
  } else if (has_archived_enum_objs()) {
    assert(is_shared(), "must be");
    bool initialized = CDSEnumKlass::initialize_enum_klass(this, CHECK);
    if (initialized) {
      return;
    }
  }
#endif

  methodHandle h_method(THREAD, class_initializer());
  assert(!is_initialized(), "we cannot initialize twice");
  LogTarget(Info, class, init) lt;
  if (lt.is_enabled()) {
    ResourceMark rm(THREAD);
    LogStream ls(lt);
    ls.print("%d Initializing ", call_class_initializer_counter++);
    name()->print_value_on(&ls);
    ls.print_cr("%s (" PTR_FORMAT ") by thread \"%s\"",
                h_method() == nullptr ? "(no method)" : "", p2i(this),
                THREAD->name());
  }
  if (h_method() != nullptr) {
    ThreadInClassInitializer ticl(THREAD, this); // Track class being initialized
    JavaCallArguments args; // No arguments
    JavaValue result(T_VOID);
    JavaCalls::call(&result, h_method, &args, CHECK); // Static call (no args)
  }
}

// If a class that implements this interface is initialized, is the JVM required
// to first execute a <clinit> method declared in this interface,
// or (if also_check_supers==true) any of the super types of this interface?
//
// JVMS 5.5. Initialization, step 7: Next, if C is a class rather than
// an interface, then let SC be its superclass and let SI1, ..., SIn
// be all superinterfaces of C (whether direct or indirect) that
// declare at least one non-abstract, non-static method.
//
// So when an interface is initialized, it does not look at its
// supers. But a proper class will ensure that all of its supers have
// run their <clinit> methods, except that it disregards interfaces
// that lack a non-static concrete method (i.e., a default method).
// Therefore, you should probably call this method only when the
// current class is a super of some proper class, not an interface.
bool InstanceKlass::interface_needs_clinit_execution_as_super(bool also_check_supers) const {
  assert(is_interface(), "must be");

  if (!has_nonstatic_concrete_methods()) {
    // quick check: no nonstatic concrete methods are declared by this or any super interfaces
    return false;
  }

  // JVMS 5.5. Initialization
  // ...If C is an interface that declares a non-abstract,
  // non-static method, the initialization of a class that
  // implements C directly or indirectly.
  if (declares_nonstatic_concrete_methods() && class_initializer() != nullptr) {
    return true;
  }
  if (also_check_supers) {
    Array<InstanceKlass*>* all_ifs = transitive_interfaces();
    for (int i = 0; i < all_ifs->length(); ++i) {
      InstanceKlass* super_intf = all_ifs->at(i);
      if (super_intf->declares_nonstatic_concrete_methods() && super_intf->class_initializer() != nullptr) {
        return true;
      }
    }
  }
  return false;
}

void InstanceKlass::mask_for(const methodHandle& method, int bci,
  InterpreterOopMap* entry_for) {
  // Lazily create the _oop_map_cache at first request.
  // Load_acquire is needed to safely get instance published with CAS by another thread.
  OopMapCache* oop_map_cache = Atomic::load_acquire(&_oop_map_cache);
  if (oop_map_cache == nullptr) {
    // Try to install new instance atomically.
    oop_map_cache = new OopMapCache();
    OopMapCache* other = Atomic::cmpxchg(&_oop_map_cache, (OopMapCache*)nullptr, oop_map_cache);
    if (other != nullptr) {
      // Someone else managed to install before us, ditch local copy and use the existing one.
      delete oop_map_cache;
      oop_map_cache = other;
    }
  }
  // _oop_map_cache is constant after init; lookup below does its own locking.
  oop_map_cache->lookup(method, bci, entry_for);
}

bool InstanceKlass::contains_field_offset(int offset) {
  fieldDescriptor fd;
  return find_field_from_offset(offset, false, &fd);
}

FieldInfo InstanceKlass::field(int index) const {
  for (AllFieldStream fs(this); !fs.done(); fs.next()) {
    if (fs.index() == index) {
      return fs.to_FieldInfo();
    }
  }
  fatal("Field not found");
  return FieldInfo();
}

bool InstanceKlass::find_local_field(Symbol* name, Symbol* sig, fieldDescriptor* fd) const {
  for (JavaFieldStream fs(this); !fs.done(); fs.next()) {
    Symbol* f_name = fs.name();
    Symbol* f_sig  = fs.signature();
    if (f_name == name && f_sig == sig) {
      fd->reinitialize(const_cast<InstanceKlass*>(this), fs.to_FieldInfo());
      return true;
    }
  }
  return false;
}


Klass* InstanceKlass::find_interface_field(Symbol* name, Symbol* sig, fieldDescriptor* fd) const {
  const int n = local_interfaces()->length();
  for (int i = 0; i < n; i++) {
    Klass* intf1 = local_interfaces()->at(i);
    assert(intf1->is_interface(), "just checking type");
    // search for field in current interface
    if (InstanceKlass::cast(intf1)->find_local_field(name, sig, fd)) {
      assert(fd->is_static(), "interface field must be static");
      return intf1;
    }
    // search for field in direct superinterfaces
    Klass* intf2 = InstanceKlass::cast(intf1)->find_interface_field(name, sig, fd);
    if (intf2 != nullptr) return intf2;
  }
  // otherwise field lookup fails
  return nullptr;
}


Klass* InstanceKlass::find_field(Symbol* name, Symbol* sig, fieldDescriptor* fd) const {
  // search order according to newest JVM spec (5.4.3.2, p.167).
  // 1) search for field in current klass
  if (find_local_field(name, sig, fd)) {
    return const_cast<InstanceKlass*>(this);
  }
  // 2) search for field recursively in direct superinterfaces
  { Klass* intf = find_interface_field(name, sig, fd);
    if (intf != nullptr) return intf;
  }
  // 3) apply field lookup recursively if superclass exists
  { Klass* supr = super();
    if (supr != nullptr) return InstanceKlass::cast(supr)->find_field(name, sig, fd);
  }
  // 4) otherwise field lookup fails
  return nullptr;
}


Klass* InstanceKlass::find_field(Symbol* name, Symbol* sig, bool is_static, fieldDescriptor* fd) const {
  // search order according to newest JVM spec (5.4.3.2, p.167).
  // 1) search for field in current klass
  if (find_local_field(name, sig, fd)) {
    if (fd->is_static() == is_static) return const_cast<InstanceKlass*>(this);
  }
  // 2) search for field recursively in direct superinterfaces
  if (is_static) {
    Klass* intf = find_interface_field(name, sig, fd);
    if (intf != nullptr) return intf;
  }
  // 3) apply field lookup recursively if superclass exists
  { Klass* supr = super();
    if (supr != nullptr) return InstanceKlass::cast(supr)->find_field(name, sig, is_static, fd);
  }
  // 4) otherwise field lookup fails
  return nullptr;
}


bool InstanceKlass::find_local_field_from_offset(int offset, bool is_static, fieldDescriptor* fd) const {
  for (JavaFieldStream fs(this); !fs.done(); fs.next()) {
    if (fs.offset() == offset) {
      fd->reinitialize(const_cast<InstanceKlass*>(this), fs.to_FieldInfo());
      if (fd->is_static() == is_static) return true;
    }
  }
  return false;
}


bool InstanceKlass::find_field_from_offset(int offset, bool is_static, fieldDescriptor* fd) const {
  Klass* klass = const_cast<InstanceKlass*>(this);
  while (klass != nullptr) {
    if (InstanceKlass::cast(klass)->find_local_field_from_offset(offset, is_static, fd)) {
      return true;
    }
    klass = klass->super();
  }
  return false;
}


void InstanceKlass::methods_do(void f(Method* method)) {
  // Methods aren't stable until they are loaded.  This can be read outside
  // a lock through the ClassLoaderData for profiling
  // Redefined scratch classes are on the list and need to be cleaned
  if (!is_loaded() && !is_scratch_class()) {
    return;
  }

  int len = methods()->length();
  for (int index = 0; index < len; index++) {
    Method* m = methods()->at(index);
    assert(m->is_method(), "must be method");
    f(m);
  }
}


void InstanceKlass::do_local_static_fields(FieldClosure* cl) {
  for (JavaFieldStream fs(this); !fs.done(); fs.next()) {
    if (fs.access_flags().is_static()) {
      fieldDescriptor& fd = fs.field_descriptor();
      cl->do_field(&fd);
    }
  }
}


void InstanceKlass::do_local_static_fields(void f(fieldDescriptor*, Handle, TRAPS), Handle mirror, TRAPS) {
  for (JavaFieldStream fs(this); !fs.done(); fs.next()) {
    if (fs.access_flags().is_static()) {
      fieldDescriptor& fd = fs.field_descriptor();
      f(&fd, mirror, CHECK);
    }
  }
}

void InstanceKlass::do_nonstatic_fields(FieldClosure* cl) {
  InstanceKlass* super = superklass();
  if (super != nullptr) {
    super->do_nonstatic_fields(cl);
  }
  for (JavaFieldStream fs(this); !fs.done(); fs.next()) {
    fieldDescriptor& fd = fs.field_descriptor();
    if (!fd.is_static()) {
      cl->do_field(&fd);
    }
  }
}

static int compare_fields_by_offset(FieldInfo* a, FieldInfo* b) {
  return a->offset() - b->offset();
}

void InstanceKlass::print_nonstatic_fields(FieldClosure* cl) {
  InstanceKlass* super = superklass();
  if (super != nullptr) {
    super->print_nonstatic_fields(cl);
  }
  ResourceMark rm;
  // In DebugInfo nonstatic fields are sorted by offset.
  GrowableArray<FieldInfo> fields_sorted;
  for (AllFieldStream fs(this); !fs.done(); fs.next()) {
    if (!fs.access_flags().is_static()) {
      fields_sorted.push(fs.to_FieldInfo());
    }
  }
  int length = fields_sorted.length();
  if (length > 0) {
    fields_sorted.sort(compare_fields_by_offset);
    fieldDescriptor fd;
    for (int i = 0; i < length; i++) {
      fd.reinitialize(this, fields_sorted.at(i));
      assert(!fd.is_static() && fd.offset() == checked_cast<int>(fields_sorted.at(i).offset()), "only nonstatic fields");
      cl->do_field(&fd);
    }
  }
}

#ifdef ASSERT
static int linear_search(const Array<Method*>* methods,
                         const Symbol* name,
                         const Symbol* signature) {
  const int len = methods->length();
  for (int index = 0; index < len; index++) {
    const Method* const m = methods->at(index);
    assert(m->is_method(), "must be method");
    if (m->signature() == signature && m->name() == name) {
       return index;
    }
  }
  return -1;
}
#endif

bool InstanceKlass::_disable_method_binary_search = false;

NOINLINE int linear_search(const Array<Method*>* methods, const Symbol* name) {
  int len = methods->length();
  int l = 0;
  int h = len - 1;
  while (l <= h) {
    Method* m = methods->at(l);
    if (m->name() == name) {
      return l;
    }
    l++;
  }
  return -1;
}

inline int InstanceKlass::quick_search(const Array<Method*>* methods, const Symbol* name) {
  if (_disable_method_binary_search) {
    assert(CDSConfig::is_dumping_dynamic_archive(), "must be");
    // At the final stage of dynamic dumping, the methods array may not be sorted
    // by ascending addresses of their names, so we can't use binary search anymore.
    // However, methods with the same name are still laid out consecutively inside the
    // methods array, so let's look for the first one that matches.
    return linear_search(methods, name);
  }

  int len = methods->length();
  int l = 0;
  int h = len - 1;

  // methods are sorted by ascending addresses of their names, so do binary search
  while (l <= h) {
    int mid = (l + h) >> 1;
    Method* m = methods->at(mid);
    assert(m->is_method(), "must be method");
    int res = m->name()->fast_compare(name);
    if (res == 0) {
      return mid;
    } else if (res < 0) {
      l = mid + 1;
    } else {
      h = mid - 1;
    }
  }
  return -1;
}

// find_method looks up the name/signature in the local methods array
Method* InstanceKlass::find_method(const Symbol* name,
                                   const Symbol* signature) const {
  return find_method_impl(name, signature,
                          OverpassLookupMode::find,
                          StaticLookupMode::find,
                          PrivateLookupMode::find);
}

Method* InstanceKlass::find_method_impl(const Symbol* name,
                                        const Symbol* signature,
                                        OverpassLookupMode overpass_mode,
                                        StaticLookupMode static_mode,
                                        PrivateLookupMode private_mode) const {
  return InstanceKlass::find_method_impl(methods(),
                                         name,
                                         signature,
                                         overpass_mode,
                                         static_mode,
                                         private_mode);
}

// find_instance_method looks up the name/signature in the local methods array
// and skips over static methods
Method* InstanceKlass::find_instance_method(const Array<Method*>* methods,
                                            const Symbol* name,
                                            const Symbol* signature,
                                            PrivateLookupMode private_mode) {
  Method* const meth = InstanceKlass::find_method_impl(methods,
                                                 name,
                                                 signature,
                                                 OverpassLookupMode::find,
                                                 StaticLookupMode::skip,
                                                 private_mode);
  assert(((meth == nullptr) || !meth->is_static()),
    "find_instance_method should have skipped statics");
  return meth;
}

// find_instance_method looks up the name/signature in the local methods array
// and skips over static methods
Method* InstanceKlass::find_instance_method(const Symbol* name,
                                            const Symbol* signature,
                                            PrivateLookupMode private_mode) const {
  return InstanceKlass::find_instance_method(methods(), name, signature, private_mode);
}

// Find looks up the name/signature in the local methods array
// and filters on the overpass, static and private flags
// This returns the first one found
// note that the local methods array can have up to one overpass, one static
// and one instance (private or not) with the same name/signature
Method* InstanceKlass::find_local_method(const Symbol* name,
                                         const Symbol* signature,
                                         OverpassLookupMode overpass_mode,
                                         StaticLookupMode static_mode,
                                         PrivateLookupMode private_mode) const {
  return InstanceKlass::find_method_impl(methods(),
                                         name,
                                         signature,
                                         overpass_mode,
                                         static_mode,
                                         private_mode);
}

// Find looks up the name/signature in the local methods array
// and filters on the overpass, static and private flags
// This returns the first one found
// note that the local methods array can have up to one overpass, one static
// and one instance (private or not) with the same name/signature
Method* InstanceKlass::find_local_method(const Array<Method*>* methods,
                                         const Symbol* name,
                                         const Symbol* signature,
                                         OverpassLookupMode overpass_mode,
                                         StaticLookupMode static_mode,
                                         PrivateLookupMode private_mode) {
  return InstanceKlass::find_method_impl(methods,
                                         name,
                                         signature,
                                         overpass_mode,
                                         static_mode,
                                         private_mode);
}

Method* InstanceKlass::find_method(const Array<Method*>* methods,
                                   const Symbol* name,
                                   const Symbol* signature) {
  return InstanceKlass::find_method_impl(methods,
                                         name,
                                         signature,
                                         OverpassLookupMode::find,
                                         StaticLookupMode::find,
                                         PrivateLookupMode::find);
}

Method* InstanceKlass::find_method_impl(const Array<Method*>* methods,
                                        const Symbol* name,
                                        const Symbol* signature,
                                        OverpassLookupMode overpass_mode,
                                        StaticLookupMode static_mode,
                                        PrivateLookupMode private_mode) {
  int hit = find_method_index(methods, name, signature, overpass_mode, static_mode, private_mode);
  return hit >= 0 ? methods->at(hit): nullptr;
}

// true if method matches signature and conforms to skipping_X conditions.
static bool method_matches(const Method* m,
                           const Symbol* signature,
                           bool skipping_overpass,
                           bool skipping_static,
                           bool skipping_private) {
  return ((m->signature() == signature) &&
    (!skipping_overpass || !m->is_overpass()) &&
    (!skipping_static || !m->is_static()) &&
    (!skipping_private || !m->is_private()));
}

// Used directly for default_methods to find the index into the
// default_vtable_indices, and indirectly by find_method
// find_method_index looks in the local methods array to return the index
// of the matching name/signature. If, overpass methods are being ignored,
// the search continues to find a potential non-overpass match.  This capability
// is important during method resolution to prefer a static method, for example,
// over an overpass method.
// There is the possibility in any _method's array to have the same name/signature
// for a static method, an overpass method and a local instance method
// To correctly catch a given method, the search criteria may need
// to explicitly skip the other two. For local instance methods, it
// is often necessary to skip private methods
int InstanceKlass::find_method_index(const Array<Method*>* methods,
                                     const Symbol* name,
                                     const Symbol* signature,
                                     OverpassLookupMode overpass_mode,
                                     StaticLookupMode static_mode,
                                     PrivateLookupMode private_mode) {
  const bool skipping_overpass = (overpass_mode == OverpassLookupMode::skip);
  const bool skipping_static = (static_mode == StaticLookupMode::skip);
  const bool skipping_private = (private_mode == PrivateLookupMode::skip);
  const int hit = quick_search(methods, name);
  if (hit != -1) {
    const Method* const m = methods->at(hit);

    // Do linear search to find matching signature.  First, quick check
    // for common case, ignoring overpasses if requested.
    if (method_matches(m, signature, skipping_overpass, skipping_static, skipping_private)) {
      return hit;
    }

    // search downwards through overloaded methods
    int i;
    for (i = hit - 1; i >= 0; --i) {
        const Method* const m = methods->at(i);
        assert(m->is_method(), "must be method");
        if (m->name() != name) {
          break;
        }
        if (method_matches(m, signature, skipping_overpass, skipping_static, skipping_private)) {
          return i;
        }
    }
    // search upwards
    for (i = hit + 1; i < methods->length(); ++i) {
        const Method* const m = methods->at(i);
        assert(m->is_method(), "must be method");
        if (m->name() != name) {
          break;
        }
        if (method_matches(m, signature, skipping_overpass, skipping_static, skipping_private)) {
          return i;
        }
    }
    // not found
#ifdef ASSERT
    const int index = (skipping_overpass || skipping_static || skipping_private) ? -1 :
      linear_search(methods, name, signature);
    assert(-1 == index, "binary search should have found entry %d", index);
#endif
  }
  return -1;
}

int InstanceKlass::find_method_by_name(const Symbol* name, int* end) const {
  return find_method_by_name(methods(), name, end);
}

int InstanceKlass::find_method_by_name(const Array<Method*>* methods,
                                       const Symbol* name,
                                       int* end_ptr) {
  assert(end_ptr != nullptr, "just checking");
  int start = quick_search(methods, name);
  int end = start + 1;
  if (start != -1) {
    while (start - 1 >= 0 && (methods->at(start - 1))->name() == name) --start;
    while (end < methods->length() && (methods->at(end))->name() == name) ++end;
    *end_ptr = end;
    return start;
  }
  return -1;
}

// uncached_lookup_method searches both the local class methods array and all
// superclasses methods arrays, skipping any overpass methods in superclasses,
// and possibly skipping private methods.
Method* InstanceKlass::uncached_lookup_method(const Symbol* name,
                                              const Symbol* signature,
                                              OverpassLookupMode overpass_mode,
                                              PrivateLookupMode private_mode) const {
  OverpassLookupMode overpass_local_mode = overpass_mode;
  const Klass* klass = this;
  while (klass != nullptr) {
    Method* const method = InstanceKlass::cast(klass)->find_method_impl(name,
                                                                        signature,
                                                                        overpass_local_mode,
                                                                        StaticLookupMode::find,
                                                                        private_mode);
    if (method != nullptr) {
      return method;
    }
    klass = klass->super();
    overpass_local_mode = OverpassLookupMode::skip;   // Always ignore overpass methods in superclasses
  }
  return nullptr;
}

#ifdef ASSERT
// search through class hierarchy and return true if this class or
// one of the superclasses was redefined
bool InstanceKlass::has_redefined_this_or_super() const {
  const Klass* klass = this;
  while (klass != nullptr) {
    if (InstanceKlass::cast(klass)->has_been_redefined()) {
      return true;
    }
    klass = klass->super();
  }
  return false;
}
#endif

// lookup a method in the default methods list then in all transitive interfaces
// Do NOT return private or static methods
Method* InstanceKlass::lookup_method_in_ordered_interfaces(Symbol* name,
                                                         Symbol* signature) const {
  Method* m = nullptr;
  if (default_methods() != nullptr) {
    m = find_method(default_methods(), name, signature);
  }
  // Look up interfaces
  if (m == nullptr) {
    m = lookup_method_in_all_interfaces(name, signature, DefaultsLookupMode::find);
  }
  return m;
}

// lookup a method in all the interfaces that this class implements
// Do NOT return private or static methods, new in JDK8 which are not externally visible
// They should only be found in the initial InterfaceMethodRef
Method* InstanceKlass::lookup_method_in_all_interfaces(Symbol* name,
                                                       Symbol* signature,
                                                       DefaultsLookupMode defaults_mode) const {
  Array<InstanceKlass*>* all_ifs = transitive_interfaces();
  int num_ifs = all_ifs->length();
  InstanceKlass *ik = nullptr;
  for (int i = 0; i < num_ifs; i++) {
    ik = all_ifs->at(i);
    Method* m = ik->lookup_method(name, signature);
    if (m != nullptr && m->is_public() && !m->is_static() &&
        ((defaults_mode != DefaultsLookupMode::skip) || !m->is_default_method())) {
      return m;
    }
  }
  return nullptr;
}

PrintClassClosure::PrintClassClosure(outputStream* st, bool verbose)
  :_st(st), _verbose(verbose) {
  ResourceMark rm;
  _st->print("%-18s  ", "KlassAddr");
  _st->print("%-4s  ", "Size");
  _st->print("%-20s  ", "State");
  _st->print("%-7s  ", "Flags");
  _st->print("%-5s  ", "ClassName");
  _st->cr();
}

void PrintClassClosure::do_klass(Klass* k)  {
  ResourceMark rm;
  // klass pointer
  _st->print(PTR_FORMAT "  ", p2i(k));
  // klass size
  _st->print("%4d  ", k->size());
  // initialization state
  if (k->is_instance_klass()) {
    _st->print("%-20s  ",InstanceKlass::cast(k)->init_state_name());
  } else {
    _st->print("%-20s  ","");
  }
  // misc flags(Changes should synced with ClassesDCmd::ClassesDCmd help doc)
  char buf[10];
  int i = 0;
  if (k->has_finalizer()) buf[i++] = 'F';
  if (k->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(k);
    if (ik->has_final_method()) buf[i++] = 'f';
    if (ik->is_rewritten()) buf[i++] = 'W';
    if (ik->is_contended()) buf[i++] = 'C';
    if (ik->has_been_redefined()) buf[i++] = 'R';
    if (ik->is_shared()) buf[i++] = 'S';
  }
  buf[i++] = '\0';
  _st->print("%-7s  ", buf);
  // klass name
  _st->print("%-5s  ", k->external_name());
  // end
  _st->cr();
  if (_verbose) {
    k->print_on(_st);
  }
}

/* jni_id_for for jfieldIds only */
JNIid* InstanceKlass::jni_id_for(int offset) {
  MutexLocker ml(JfieldIdCreation_lock);
  JNIid* probe = jni_ids() == nullptr ? nullptr : jni_ids()->find(offset);
  if (probe == nullptr) {
    // Allocate new static field identifier
    probe = new JNIid(this, offset, jni_ids());
    set_jni_ids(probe);
  }
  return probe;
}

u2 InstanceKlass::enclosing_method_data(int offset) const {
  const Array<jushort>* const inner_class_list = inner_classes();
  if (inner_class_list == nullptr) {
    return 0;
  }
  const int length = inner_class_list->length();
  if (length % inner_class_next_offset == 0) {
    return 0;
  }
  const int index = length - enclosing_method_attribute_size;
  assert(offset < enclosing_method_attribute_size, "invalid offset");
  return inner_class_list->at(index + offset);
}

void InstanceKlass::set_enclosing_method_indices(u2 class_index,
                                                 u2 method_index) {
  Array<jushort>* inner_class_list = inner_classes();
  assert (inner_class_list != nullptr, "_inner_classes list is not set up");
  int length = inner_class_list->length();
  if (length % inner_class_next_offset == enclosing_method_attribute_size) {
    int index = length - enclosing_method_attribute_size;
    inner_class_list->at_put(
      index + enclosing_method_class_index_offset, class_index);
    inner_class_list->at_put(
      index + enclosing_method_method_index_offset, method_index);
  }
}

jmethodID InstanceKlass::update_jmethod_id(jmethodID* jmeths, Method* method, int idnum) {
  if (method->is_old() && !method->is_obsolete()) {
    // If the method passed in is old (but not obsolete), use the current version.
    method = method_with_idnum((int)idnum);
    assert(method != nullptr, "old and but not obsolete, so should exist");
  }
  jmethodID new_id = Method::make_jmethod_id(class_loader_data(), method);
  Atomic::release_store(&jmeths[idnum + 1], new_id);
  return new_id;
}

// Lookup or create a jmethodID.
// This code is called by the VMThread and JavaThreads so the
// locking has to be done very carefully to avoid deadlocks
// and/or other cache consistency problems.
//
jmethodID InstanceKlass::get_jmethod_id(const methodHandle& method_h) {
  Method* method = method_h();
  int idnum = method->method_idnum();
  jmethodID* jmeths = methods_jmethod_ids_acquire();

  // We use a double-check locking idiom here because this cache is
  // performance sensitive. In the normal system, this cache only
  // transitions from null to non-null which is safe because we use
  // release_set_methods_jmethod_ids() to advertise the new cache.
  // A partially constructed cache should never be seen by a racing
  // thread. We also use release_store() to save a new jmethodID
  // in the cache so a partially constructed jmethodID should never be
  // seen either. Cache reads of existing jmethodIDs proceed without a
  // lock, but cache writes of a new jmethodID requires uniqueness and
  // creation of the cache itself requires no leaks so a lock is
  // acquired in those two cases.
  //
  // If the RedefineClasses() API has been used, then this cache grows
  // in the redefinition safepoint.

  if (jmeths == nullptr) {
    MutexLocker ml(JmethodIdCreation_lock, Mutex::_no_safepoint_check_flag);
    jmeths = methods_jmethod_ids_acquire();
    // Still null?
    if (jmeths == nullptr) {
      size_t size = idnum_allocated_count();
      assert(size > (size_t)idnum, "should already have space");
      jmeths = NEW_C_HEAP_ARRAY(jmethodID, size + 1, mtClass);
      memset(jmeths, 0, (size + 1) * sizeof(jmethodID));
      // cache size is stored in element[0], other elements offset by one
      jmeths[0] = (jmethodID)size;
      jmethodID new_id = update_jmethod_id(jmeths, method, idnum);

      // publish jmeths
      release_set_methods_jmethod_ids(jmeths);
      return new_id;
    }
  }

  jmethodID id = Atomic::load_acquire(&jmeths[idnum + 1]);
  if (id == nullptr) {
    MutexLocker ml(JmethodIdCreation_lock, Mutex::_no_safepoint_check_flag);
    id = jmeths[idnum + 1];
    // Still null?
    if (id == nullptr) {
      return update_jmethod_id(jmeths, method, idnum);
    }
  }
  return id;
}

void InstanceKlass::update_methods_jmethod_cache() {
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
  jmethodID* cache = _methods_jmethod_ids;
  if (cache != nullptr) {
    size_t size = idnum_allocated_count();
    size_t old_size = (size_t)cache[0];
    if (old_size < size + 1) {
      // Allocate a larger one and copy entries to the new one.
      // They've already been updated to point to new methods where applicable (i.e., not obsolete).
      jmethodID* new_cache = NEW_C_HEAP_ARRAY(jmethodID, size + 1, mtClass);
      memset(new_cache, 0, (size + 1) * sizeof(jmethodID));
      // The cache size is stored in element[0]; the other elements are offset by one.
      new_cache[0] = (jmethodID)size;

      for (int i = 1; i <= (int)old_size; i++) {
        new_cache[i] = cache[i];
      }
      _methods_jmethod_ids = new_cache;
      FREE_C_HEAP_ARRAY(jmethodID, cache);
    }
  }
}

// Figure out how many jmethodIDs haven't been allocated, and make
// sure space for them is pre-allocated.  This makes getting all
// method ids much, much faster with classes with more than 8
// methods, and has a *substantial* effect on performance with jvmti
// code that loads all jmethodIDs for all classes.
void InstanceKlass::ensure_space_for_methodids(int start_offset) {
  int new_jmeths = 0;
  int length = methods()->length();
  for (int index = start_offset; index < length; index++) {
    Method* m = methods()->at(index);
    jmethodID id = m->find_jmethod_id_or_null();
    if (id == nullptr) {
      new_jmeths++;
    }
  }
  if (new_jmeths != 0) {
    Method::ensure_jmethod_ids(class_loader_data(), new_jmeths);
  }
}

// Lookup a jmethodID, null if not found.  Do no blocking, no allocations, no handles
jmethodID InstanceKlass::jmethod_id_or_null(Method* method) {
  int idnum = method->method_idnum();
  jmethodID* jmeths = methods_jmethod_ids_acquire();
  return (jmeths != nullptr) ? jmeths[idnum + 1] : nullptr;
}

inline DependencyContext InstanceKlass::dependencies() {
  DependencyContext dep_context(&_dep_context, &_dep_context_last_cleaned);
  return dep_context;
}

void InstanceKlass::mark_dependent_nmethods(DeoptimizationScope* deopt_scope, KlassDepChange& changes) {
  dependencies().mark_dependent_nmethods(deopt_scope, changes);
}

void InstanceKlass::add_dependent_nmethod(nmethod* nm) {
  assert_lock_strong(CodeCache_lock);
  dependencies().add_dependent_nmethod(nm);
}

void InstanceKlass::clean_dependency_context() {
  dependencies().clean_unloading_dependents();
}

#ifndef PRODUCT
void InstanceKlass::print_dependent_nmethods(bool verbose) {
  dependencies().print_dependent_nmethods(verbose);
}

bool InstanceKlass::is_dependent_nmethod(nmethod* nm) {
  return dependencies().is_dependent_nmethod(nm);
}
#endif //PRODUCT

void InstanceKlass::clean_weak_instanceklass_links() {
  clean_implementors_list();
  clean_method_data();
}

void InstanceKlass::clean_implementors_list() {
  assert(is_loader_alive(), "this klass should be live");
  if (is_interface()) {
    assert (ClassUnloading, "only called for ClassUnloading");
    for (;;) {
      // Use load_acquire due to competing with inserts
      InstanceKlass* volatile* iklass = adr_implementor();
      assert(iklass != nullptr, "Klass must not be null");
      InstanceKlass* impl = Atomic::load_acquire(iklass);
      if (impl != nullptr && !impl->is_loader_alive()) {
        // null this field, might be an unloaded instance klass or null
        if (Atomic::cmpxchg(iklass, impl, (InstanceKlass*)nullptr) == impl) {
          // Successfully unlinking implementor.
          if (log_is_enabled(Trace, class, unload)) {
            ResourceMark rm;
            log_trace(class, unload)("unlinking class (implementor): %s", impl->external_name());
          }
          return;
        }
      } else {
        return;
      }
    }
  }
}

void InstanceKlass::clean_method_data() {
  for (int m = 0; m < methods()->length(); m++) {
    MethodData* mdo = methods()->at(m)->method_data();
    if (mdo != nullptr) {
      mdo->clean_method_data(/*always_clean*/false);
    }
  }
}

void InstanceKlass::metaspace_pointers_do(MetaspaceClosure* it) {
  Klass::metaspace_pointers_do(it);

  if (log_is_enabled(Trace, aot)) {
    ResourceMark rm;
    log_trace(aot)("Iter(InstanceKlass): %p (%s)", this, external_name());
  }

  it->push(&_annotations);
  it->push((Klass**)&_array_klasses);
  if (!is_rewritten()) {
    it->push(&_constants, MetaspaceClosure::_writable);
  } else {
    it->push(&_constants);
  }
  it->push(&_inner_classes);
#if INCLUDE_JVMTI
  it->push(&_previous_versions);
#endif
#if INCLUDE_CDS
  // For "old" classes with methods containing the jsr bytecode, the _methods array will
  // be rewritten during runtime (see Rewriter::rewrite_jsrs()) but they cannot be safely
  // checked here with ByteCodeStream. All methods that can't be verified are made writable.
  // The length check on the _methods is necessary because classes which don't have any
  // methods share the Universe::_the_empty_method_array which is in the RO region.
  if (_methods != nullptr && _methods->length() > 0 && !can_be_verified_at_dumptime()) {
    // To handle jsr bytecode, new Method* maybe stored into _methods
    it->push(&_methods, MetaspaceClosure::_writable);
  } else {
#endif
    it->push(&_methods);
#if INCLUDE_CDS
  }
#endif
  it->push(&_default_methods);
  it->push(&_local_interfaces);
  it->push(&_transitive_interfaces);
  it->push(&_method_ordering);
  if (!is_rewritten()) {
    it->push(&_default_vtable_indices, MetaspaceClosure::_writable);
  } else {
    it->push(&_default_vtable_indices);
  }

  it->push(&_fieldinfo_stream);
  // _fields_status might be written into by Rewriter::scan_method() -> fd.set_has_initialized_final_update()
  it->push(&_fields_status, MetaspaceClosure::_writable);

  if (itable_length() > 0) {
    itableOffsetEntry* ioe = (itableOffsetEntry*)start_of_itable();
    int method_table_offset_in_words = ioe->offset()/wordSize;
    int itable_offset_in_words = (int)(start_of_itable() - (intptr_t*)this);

    int nof_interfaces = (method_table_offset_in_words - itable_offset_in_words)
                         / itableOffsetEntry::size();

    for (int i = 0; i < nof_interfaces; i ++, ioe ++) {
      if (ioe->interface_klass() != nullptr) {
        it->push(ioe->interface_klass_addr());
        itableMethodEntry* ime = ioe->first_method_entry(this);
        int n = klassItable::method_count_for_interface(ioe->interface_klass());
        for (int index = 0; index < n; index ++) {
          it->push(ime[index].method_addr());
        }
      }
    }
  }

  it->push(&_nest_host);
  it->push(&_nest_members);
  it->push(&_permitted_subclasses);
  it->push(&_record_components);
}

#if INCLUDE_CDS
void InstanceKlass::remove_unshareable_info() {

  if (is_linked()) {
    assert(can_be_verified_at_dumptime(), "must be");
    // Remember this so we can avoid walking the hierarchy at runtime.
    set_verified_at_dump_time();
  }

  _misc_flags.set_has_init_deps_processed(false);

  Klass::remove_unshareable_info();

  if (SystemDictionaryShared::has_class_failed_verification(this)) {
    // Classes are attempted to link during dumping and may fail,
    // but these classes are still in the dictionary and class list in CLD.
    // If the class has failed verification, there is nothing else to remove.
    return;
  }

  // Reset to the 'allocated' state to prevent any premature accessing to
  // a shared class at runtime while the class is still being loaded and
  // restored. A class' init_state is set to 'loaded' at runtime when it's
  // being added to class hierarchy (see InstanceKlass:::add_to_hierarchy()).
  _init_state = allocated;

  { // Otherwise this needs to take out the Compile_lock.
    assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
    init_implementor();
  }

  // Call remove_unshareable_info() on other objects that belong to this class, except
  // for constants()->remove_unshareable_info(), which is called in a separate pass in
  // ArchiveBuilder::make_klasses_shareable(),

  for (int i = 0; i < methods()->length(); i++) {
    Method* m = methods()->at(i);
    m->remove_unshareable_info();
  }

  // do array classes also.
  if (array_klasses() != nullptr) {
    array_klasses()->remove_unshareable_info();
  }

  // These are not allocated from metaspace. They are safe to set to null.
  _source_debug_extension = nullptr;
  _dep_context = nullptr;
  _osr_nmethods_head = nullptr;
#if INCLUDE_JVMTI
  _breakpoints = nullptr;
  _previous_versions = nullptr;
  _cached_class_file = nullptr;
  _jvmti_cached_class_field_map = nullptr;
#endif

  _init_thread = nullptr;
  _methods_jmethod_ids = nullptr;
  _jni_ids = nullptr;
  _oop_map_cache = nullptr;
  if (CDSConfig::is_dumping_method_handles() && HeapShared::is_lambda_proxy_klass(this)) {
    // keep _nest_host
  } else {
    // clear _nest_host to ensure re-load at runtime
    _nest_host = nullptr;
  }
  init_shared_package_entry();
  _dep_context_last_cleaned = 0;
  DEBUG_ONLY(_shared_class_load_count = 0);

  remove_unshareable_flags();
}

void InstanceKlass::remove_unshareable_flags() {
  // clear all the flags/stats that shouldn't be in the archived version
  assert(!is_scratch_class(), "must be");
  assert(!has_been_redefined(), "must be");
#if INCLUDE_JVMTI
  set_is_being_redefined(false);
#endif
  set_has_resolved_methods(false);
}

void InstanceKlass::remove_java_mirror() {
  Klass::remove_java_mirror();

  // do array classes also.
  if (array_klasses() != nullptr) {
    array_klasses()->remove_java_mirror();
  }
}

void InstanceKlass::init_shared_package_entry() {
  assert(CDSConfig::is_dumping_archive(), "must be");
#if !INCLUDE_CDS_JAVA_HEAP
  _package_entry = nullptr;
#else
  if (CDSConfig::is_dumping_full_module_graph()) {
    if (defined_by_other_loaders()) {
      _package_entry = nullptr;
    } else {
      _package_entry = PackageEntry::get_archived_entry(_package_entry);
    }
  } else if (CDSConfig::is_dumping_dynamic_archive() &&
             CDSConfig::is_using_full_module_graph() &&
             MetaspaceShared::is_in_shared_metaspace(_package_entry)) {
    // _package_entry is an archived package in the base archive. Leave it as is.
  } else {
    _package_entry = nullptr;
  }
  ArchivePtrMarker::mark_pointer((address**)&_package_entry);
#endif
}

void InstanceKlass::compute_has_loops_flag_for_methods() {
  Array<Method*>* methods = this->methods();
  for (int index = 0; index < methods->length(); ++index) {
    Method* m = methods->at(index);
    if (!m->is_overpass()) { // work around JDK-8305771
      m->compute_has_loops_flag();
    }
  }
}

void InstanceKlass::restore_unshareable_info(ClassLoaderData* loader_data, Handle protection_domain,
                                             PackageEntry* pkg_entry, TRAPS) {
  // InstanceKlass::add_to_hierarchy() sets the init_state to loaded
  // before the InstanceKlass is added to the SystemDictionary. Make
  // sure the current state is <loaded.
  assert(!is_loaded(), "invalid init state");
  assert(!shared_loading_failed(), "Must not try to load failed class again");
  set_package(loader_data, pkg_entry, CHECK);
  Klass::restore_unshareable_info(loader_data, protection_domain, CHECK);

  Array<Method*>* methods = this->methods();
  int num_methods = methods->length();
  for (int index = 0; index < num_methods; ++index) {
    methods->at(index)->restore_unshareable_info(CHECK);
  }
#if INCLUDE_JVMTI
  if (JvmtiExport::has_redefined_a_class()) {
    // Reinitialize vtable because RedefineClasses may have changed some
    // entries in this vtable for super classes so the CDS vtable might
    // point to old or obsolete entries.  RedefineClasses doesn't fix up
    // vtables in the shared system dictionary, only the main one.
    // It also redefines the itable too so fix that too.
    // First fix any default methods that point to a super class that may
    // have been redefined.
    bool trace_name_printed = false;
    adjust_default_methods(&trace_name_printed);
    if (verified_at_dump_time()) {
      // Initialize vtable and itable for classes which can be verified at dump time.
      // Unlinked classes such as old classes with major version < 50 cannot be verified
      // at dump time.
      vtable().initialize_vtable();
      itable().initialize_itable();
    }
  }
#endif // INCLUDE_JVMTI

  // restore constant pool resolved references
  constants()->restore_unshareable_info(CHECK);

  if (array_klasses() != nullptr) {
    // To get a consistent list of classes we need MultiArray_lock to ensure
    // array classes aren't observed while they are being restored.
    RecursiveLocker rl(MultiArray_lock, THREAD);
    assert(this == array_klasses()->bottom_klass(), "sanity");
    // Array classes have null protection domain.
    // --> see ArrayKlass::complete_create_array_klass()
    array_klasses()->restore_unshareable_info(class_loader_data(), Handle(), CHECK);
  }

  // Initialize @ValueBased class annotation if not already set in the archived klass.
  if (DiagnoseSyncOnValueBasedClasses && has_value_based_class_annotation() && !is_value_based()) {
    set_is_value_based();
  }
}

// Check if a class or any of its supertypes has a version older than 50.
// CDS will not perform verification of old classes during dump time because
// without changing the old verifier, the verification constraint cannot be
// retrieved during dump time.
// Verification of archived old classes will be performed during run time.
bool InstanceKlass::can_be_verified_at_dumptime() const {
  if (MetaspaceShared::is_in_shared_metaspace(this)) {
    // This is a class that was dumped into the base archive, so we know
    // it was verified at dump time.
    return true;
  }
  if (major_version() < 50 /*JAVA_6_VERSION*/) {
    return false;
  }
  if (java_super() != nullptr && !java_super()->can_be_verified_at_dumptime()) {
    return false;
  }
  Array<InstanceKlass*>* interfaces = local_interfaces();
  int len = interfaces->length();
  for (int i = 0; i < len; i++) {
    if (!interfaces->at(i)->can_be_verified_at_dumptime()) {
      return false;
    }
  }
  return true;
}

#endif // INCLUDE_CDS

#if INCLUDE_JVMTI
static void clear_all_breakpoints(Method* m) {
  m->clear_all_breakpoints();
}
#endif

void InstanceKlass::unload_class(InstanceKlass* ik) {

  if (ik->is_scratch_class()) {
    assert(ik->dependencies().is_empty(), "dependencies should be empty for scratch classes");
    return;
  }
  assert(ik->is_loaded(), "class should be loaded " PTR_FORMAT, p2i(ik));

  // Release dependencies.
  ik->dependencies().remove_all_dependents();

  // notify the debugger
  if (JvmtiExport::should_post_class_unload()) {
    JvmtiExport::post_class_unload(ik);
  }

  // notify ClassLoadingService of class unload
  ClassLoadingService::notify_class_unloaded(ik);

  SystemDictionaryShared::handle_class_unloading(ik);

  if (log_is_enabled(Info, class, unload)) {
    ResourceMark rm;
    log_info(class, unload)("unloading class %s " PTR_FORMAT, ik->external_name(), p2i(ik));
  }

  Events::log_class_unloading(Thread::current(), ik);

#if INCLUDE_JFR
  assert(ik != nullptr, "invariant");
  EventClassUnload event;
  event.set_unloadedClass(ik);
  event.set_definingClassLoader(ik->class_loader_data());
  event.commit();
#endif
}

static void method_release_C_heap_structures(Method* m) {
  m->release_C_heap_structures();
}

// Called also by InstanceKlass::deallocate_contents, with false for release_sub_metadata.
void InstanceKlass::release_C_heap_structures(bool release_sub_metadata) {
  // Clean up C heap
  Klass::release_C_heap_structures();

  // Deallocate and call destructors for MDO mutexes
  if (release_sub_metadata) {
    methods_do(method_release_C_heap_structures);
  }

  // Deallocate oop map cache
  if (_oop_map_cache != nullptr) {
    delete _oop_map_cache;
    _oop_map_cache = nullptr;
  }

  // Deallocate JNI identifiers for jfieldIDs
  JNIid::deallocate(jni_ids());
  set_jni_ids(nullptr);

  jmethodID* jmeths = methods_jmethod_ids_acquire();
  if (jmeths != nullptr) {
    release_set_methods_jmethod_ids(nullptr);
    FreeHeap(jmeths);
  }

  assert(_dep_context == nullptr,
         "dependencies should already be cleaned");

#if INCLUDE_JVMTI
  // Deallocate breakpoint records
  if (breakpoints() != nullptr) {
    methods_do(clear_all_breakpoints);
    assert(breakpoints() == nullptr, "should have cleared breakpoints");
  }

  // deallocate the cached class file
  if (_cached_class_file != nullptr) {
    os::free(_cached_class_file);
    _cached_class_file = nullptr;
  }
#endif

  FREE_C_HEAP_ARRAY(char, _source_debug_extension);

  if (release_sub_metadata) {
    constants()->release_C_heap_structures();
  }
}

// The constant pool is on stack if any of the methods are executing or
// referenced by handles.
bool InstanceKlass::on_stack() const {
  return _constants->on_stack();
}

Symbol* InstanceKlass::source_file_name() const               { return _constants->source_file_name(); }
u2 InstanceKlass::source_file_name_index() const              { return _constants->source_file_name_index(); }
void InstanceKlass::set_source_file_name_index(u2 sourcefile_index) { _constants->set_source_file_name_index(sourcefile_index); }

// minor and major version numbers of class file
u2 InstanceKlass::minor_version() const                 { return _constants->minor_version(); }
void InstanceKlass::set_minor_version(u2 minor_version) { _constants->set_minor_version(minor_version); }
u2 InstanceKlass::major_version() const                 { return _constants->major_version(); }
void InstanceKlass::set_major_version(u2 major_version) { _constants->set_major_version(major_version); }

const InstanceKlass* InstanceKlass::get_klass_version(int version) const {
  for (const InstanceKlass* ik = this; ik != nullptr; ik = ik->previous_versions()) {
    if (ik->constants()->version() == version) {
      return ik;
    }
  }
  return nullptr;
}

void InstanceKlass::set_source_debug_extension(const char* array, int length) {
  if (array == nullptr) {
    _source_debug_extension = nullptr;
  } else {
    // Adding one to the attribute length in order to store a null terminator
    // character could cause an overflow because the attribute length is
    // already coded with an u4 in the classfile, but in practice, it's
    // unlikely to happen.
    assert((length+1) > length, "Overflow checking");
    char* sde = NEW_C_HEAP_ARRAY(char, (length + 1), mtClass);
    for (int i = 0; i < length; i++) {
      sde[i] = array[i];
    }
    sde[length] = '\0';
    _source_debug_extension = sde;
  }
}

Symbol* InstanceKlass::generic_signature() const                   { return _constants->generic_signature(); }
u2 InstanceKlass::generic_signature_index() const                  { return _constants->generic_signature_index(); }
void InstanceKlass::set_generic_signature_index(u2 sig_index)      { _constants->set_generic_signature_index(sig_index); }

const char* InstanceKlass::signature_name() const {

  // Get the internal name as a c string
  const char* src = (const char*) (name()->as_C_string());
  const int src_length = (int)strlen(src);

  char* dest = NEW_RESOURCE_ARRAY(char, src_length + 3);

  // Add L as type indicator
  int dest_index = 0;
  dest[dest_index++] = JVM_SIGNATURE_CLASS;

  // Add the actual class name
  for (int src_index = 0; src_index < src_length; ) {
    dest[dest_index++] = src[src_index++];
  }

  if (is_hidden()) { // Replace the last '+' with a '.'.
    for (int index = (int)src_length; index > 0; index--) {
      if (dest[index] == '+') {
        dest[index] = JVM_SIGNATURE_DOT;
        break;
      }
    }
  }

  // Add the semicolon and the null
  dest[dest_index++] = JVM_SIGNATURE_ENDCLASS;
  dest[dest_index] = '\0';
  return dest;
}

ModuleEntry* InstanceKlass::module() const {
  if (is_hidden() &&
      in_unnamed_package() &&
      class_loader_data()->has_class_mirror_holder()) {
    // For a non-strong hidden class defined to an unnamed package,
    // its (class held) CLD will not have an unnamed module created for it.
    // Two choices to find the correct ModuleEntry:
    // 1. If hidden class is within a nest, use nest host's module
    // 2. Find the unnamed module off from the class loader
    // For now option #2 is used since a nest host is not set until
    // after the instance class is created in jvm_lookup_define_class().
    if (class_loader_data()->is_boot_class_loader_data()) {
      return ClassLoaderData::the_null_class_loader_data()->unnamed_module();
    } else {
      oop module = java_lang_ClassLoader::unnamedModule(class_loader_data()->class_loader());
      assert(java_lang_Module::is_instance(module), "Not an instance of java.lang.Module");
      return java_lang_Module::module_entry(module);
    }
  }

  // Class is in a named package
  if (!in_unnamed_package()) {
    return _package_entry->module();
  }

  // Class is in an unnamed package, return its loader's unnamed module
  return class_loader_data()->unnamed_module();
}

bool InstanceKlass::in_javabase_module() const {
  return module()->name() == vmSymbols::java_base();
}

void InstanceKlass::set_package(ClassLoaderData* loader_data, PackageEntry* pkg_entry, TRAPS) {

  // ensure java/ packages only loaded by boot or platform builtin loaders
  // not needed for shared class since CDS does not archive prohibited classes.
  if (!is_shared()) {
    check_prohibited_package(name(), loader_data, CHECK);
  }

  if (is_shared() && _package_entry != nullptr) {
    if (CDSConfig::is_using_full_module_graph() && _package_entry == pkg_entry) {
      // we can use the saved package
      assert(MetaspaceShared::is_in_shared_metaspace(_package_entry), "must be");
      return;
    } else {
      _package_entry = nullptr;
    }
  }

  // ClassLoader::package_from_class_name has already incremented the refcount of the symbol
  // it returns, so we need to decrement it when the current function exits.
  TempNewSymbol from_class_name =
      (pkg_entry != nullptr) ? nullptr : ClassLoader::package_from_class_name(name());

  Symbol* pkg_name;
  if (pkg_entry != nullptr) {
    pkg_name = pkg_entry->name();
  } else {
    pkg_name = from_class_name;
  }

  if (pkg_name != nullptr && loader_data != nullptr) {

    // Find in class loader's package entry table.
    _package_entry = pkg_entry != nullptr ? pkg_entry : loader_data->packages()->lookup_only(pkg_name);

    // If the package name is not found in the loader's package
    // entry table, it is an indication that the package has not
    // been defined. Consider it defined within the unnamed module.
    if (_package_entry == nullptr) {

      if (!ModuleEntryTable::javabase_defined()) {
        // Before java.base is defined during bootstrapping, define all packages in
        // the java.base module.  If a non-java.base package is erroneously placed
        // in the java.base module it will be caught later when java.base
        // is defined by ModuleEntryTable::verify_javabase_packages check.
        assert(ModuleEntryTable::javabase_moduleEntry() != nullptr, JAVA_BASE_NAME " module is null");
        _package_entry = loader_data->packages()->create_entry_if_absent(pkg_name, ModuleEntryTable::javabase_moduleEntry());
      } else {
        assert(loader_data->unnamed_module() != nullptr, "unnamed module is null");
        _package_entry = loader_data->packages()->create_entry_if_absent(pkg_name, loader_data->unnamed_module());
      }

      // A package should have been successfully created
      DEBUG_ONLY(ResourceMark rm(THREAD));
      assert(_package_entry != nullptr, "Package entry for class %s not found, loader %s",
             name()->as_C_string(), loader_data->loader_name_and_id());
    }

    if (log_is_enabled(Debug, module)) {
      ResourceMark rm(THREAD);
      ModuleEntry* m = _package_entry->module();
      log_trace(module)("Setting package: class: %s, package: %s, loader: %s, module: %s",
                        external_name(),
                        pkg_name->as_C_string(),
                        loader_data->loader_name_and_id(),
                        (m->is_named() ? m->name()->as_C_string() : UNNAMED_MODULE));
    }
  } else {
    ResourceMark rm(THREAD);
    log_trace(module)("Setting package: class: %s, package: unnamed, loader: %s, module: %s",
                      external_name(),
                      (loader_data != nullptr) ? loader_data->loader_name_and_id() : "null",
                      UNNAMED_MODULE);
  }
}

// Function set_classpath_index ensures that for a non-null _package_entry
// of the InstanceKlass, the entry is in the boot loader's package entry table.
// It then sets the classpath_index in the package entry record.
//
// The classpath_index field is used to find the entry on the boot loader class
// path for packages with classes loaded by the boot loader from -Xbootclasspath/a
// in an unnamed module.  It is also used to indicate (for all packages whose
// classes are loaded by the boot loader) that at least one of the package's
// classes has been loaded.
void InstanceKlass::set_classpath_index(s2 path_index) {
  if (_package_entry != nullptr) {
    DEBUG_ONLY(PackageEntryTable* pkg_entry_tbl = ClassLoaderData::the_null_class_loader_data()->packages();)
    assert(pkg_entry_tbl->lookup_only(_package_entry->name()) == _package_entry, "Should be same");
    assert(path_index != -1, "Unexpected classpath_index");
    _package_entry->set_classpath_index(path_index);
  }
}

// different versions of is_same_class_package

bool InstanceKlass::is_same_class_package(const Klass* class2) const {
  oop classloader1 = this->class_loader();
  PackageEntry* classpkg1 = this->package();
  if (class2->is_objArray_klass()) {
    class2 = ObjArrayKlass::cast(class2)->bottom_klass();
  }

  oop classloader2;
  PackageEntry* classpkg2;
  if (class2->is_instance_klass()) {
    classloader2 = class2->class_loader();
    classpkg2 = class2->package();
  } else {
    assert(class2->is_typeArray_klass(), "should be type array");
    classloader2 = nullptr;
    classpkg2 = nullptr;
  }

  // Same package is determined by comparing class loader
  // and package entries. Both must be the same. This rule
  // applies even to classes that are defined in the unnamed
  // package, they still must have the same class loader.
  if ((classloader1 == classloader2) && (classpkg1 == classpkg2)) {
    return true;
  }

  return false;
}

// return true if this class and other_class are in the same package. Classloader
// and classname information is enough to determine a class's package
bool InstanceKlass::is_same_class_package(oop other_class_loader,
                                          const Symbol* other_class_name) const {
  if (class_loader() != other_class_loader) {
    return false;
  }
  if (name()->fast_compare(other_class_name) == 0) {
     return true;
  }

  {
    ResourceMark rm;

    bool bad_class_name = false;
    TempNewSymbol other_pkg = ClassLoader::package_from_class_name(other_class_name, &bad_class_name);
    if (bad_class_name) {
      return false;
    }
    // Check that package_from_class_name() returns null, not "", if there is no package.
    assert(other_pkg == nullptr || other_pkg->utf8_length() > 0, "package name is empty string");

    const Symbol* const this_package_name =
      this->package() != nullptr ? this->package()->name() : nullptr;

    if (this_package_name == nullptr || other_pkg == nullptr) {
      // One of the two doesn't have a package.  Only return true if the other
      // one also doesn't have a package.
      return this_package_name == other_pkg;
    }

    // Check if package is identical
    return this_package_name->fast_compare(other_pkg) == 0;
  }
}

static bool is_prohibited_package_slow(Symbol* class_name) {
  // Caller has ResourceMark
  int length;
  jchar* unicode = class_name->as_unicode(length);
  return (length >= 5 &&
          unicode[0] == 'j' &&
          unicode[1] == 'a' &&
          unicode[2] == 'v' &&
          unicode[3] == 'a' &&
          unicode[4] == '/');
}

// Only boot and platform class loaders can define classes in "java/" packages.
void InstanceKlass::check_prohibited_package(Symbol* class_name,
                                             ClassLoaderData* loader_data,
                                             TRAPS) {
  if (!loader_data->is_boot_class_loader_data() &&
      !loader_data->is_platform_class_loader_data() &&
      class_name != nullptr && class_name->utf8_length() >= 5) {
    ResourceMark rm(THREAD);
    bool prohibited;
    const u1* base = class_name->base();
    if ((base[0] | base[1] | base[2] | base[3] | base[4]) & 0x80) {
      prohibited = is_prohibited_package_slow(class_name);
    } else {
      char* name = class_name->as_C_string();
      prohibited = (strncmp(name, JAVAPKG, JAVAPKG_LEN) == 0 && name[JAVAPKG_LEN] == '/');
    }
    if (prohibited) {
      TempNewSymbol pkg_name = ClassLoader::package_from_class_name(class_name);
      assert(pkg_name != nullptr, "Error in parsing package name starting with 'java/'");
      char* name = pkg_name->as_C_string();
      const char* class_loader_name = loader_data->loader_name_and_id();
      StringUtils::replace_no_expand(name, "/", ".");
      const char* msg_text1 = "Class loader (instance of): ";
      const char* msg_text2 = " tried to load prohibited package name: ";
      size_t len = strlen(msg_text1) + strlen(class_loader_name) + strlen(msg_text2) + strlen(name) + 1;
      char* message = NEW_RESOURCE_ARRAY_IN_THREAD(THREAD, char, len);
      jio_snprintf(message, len, "%s%s%s%s", msg_text1, class_loader_name, msg_text2, name);
      THROW_MSG(vmSymbols::java_lang_SecurityException(), message);
    }
  }
  return;
}

bool InstanceKlass::find_inner_classes_attr(int* ooff, int* noff, TRAPS) const {
  constantPoolHandle i_cp(THREAD, constants());
  for (InnerClassesIterator iter(this); !iter.done(); iter.next()) {
    int ioff = iter.inner_class_info_index();
    if (ioff != 0) {
      // Check to see if the name matches the class we're looking for
      // before attempting to find the class.
      if (i_cp->klass_name_at_matches(this, ioff)) {
        Klass* inner_klass = i_cp->klass_at(ioff, CHECK_false);
        if (this == inner_klass) {
          *ooff = iter.outer_class_info_index();
          *noff = iter.inner_name_index();
          return true;
        }
      }
    }
  }
  return false;
}

InstanceKlass* InstanceKlass::compute_enclosing_class(bool* inner_is_member, TRAPS) const {
  InstanceKlass* outer_klass = nullptr;
  *inner_is_member = false;
  int ooff = 0, noff = 0;
  bool has_inner_classes_attr = find_inner_classes_attr(&ooff, &noff, THREAD);
  if (has_inner_classes_attr) {
    constantPoolHandle i_cp(THREAD, constants());
    if (ooff != 0) {
      Klass* ok = i_cp->klass_at(ooff, CHECK_NULL);
      if (!ok->is_instance_klass()) {
        // If the outer class is not an instance klass then it cannot have
        // declared any inner classes.
        ResourceMark rm(THREAD);
        // Names are all known to be < 64k so we know this formatted message is not excessively large.
        Exceptions::fthrow(
          THREAD_AND_LOCATION,
          vmSymbols::java_lang_IncompatibleClassChangeError(),
          "%s and %s disagree on InnerClasses attribute",
          ok->external_name(),
          external_name());
        return nullptr;
      }
      outer_klass = InstanceKlass::cast(ok);
      *inner_is_member = true;
    }
    if (nullptr == outer_klass) {
      // It may be a local class; try for that.
      int encl_method_class_idx = enclosing_method_class_index();
      if (encl_method_class_idx != 0) {
        Klass* ok = i_cp->klass_at(encl_method_class_idx, CHECK_NULL);
        outer_klass = InstanceKlass::cast(ok);
        *inner_is_member = false;
      }
    }
  }

  // If no inner class attribute found for this class.
  if (nullptr == outer_klass) return nullptr;

  // Throws an exception if outer klass has not declared k as an inner klass
  // We need evidence that each klass knows about the other, or else
  // the system could allow a spoof of an inner class to gain access rights.
  Reflection::check_for_inner_class(outer_klass, this, *inner_is_member, CHECK_NULL);
  return outer_klass;
}

u2 InstanceKlass::compute_modifier_flags() const {
  u2 access = access_flags().as_unsigned_short();

  // But check if it happens to be member class.
  InnerClassesIterator iter(this);
  for (; !iter.done(); iter.next()) {
    int ioff = iter.inner_class_info_index();
    // Inner class attribute can be zero, skip it.
    // Strange but true:  JVM spec. allows null inner class refs.
    if (ioff == 0) continue;

    // only look at classes that are already loaded
    // since we are looking for the flags for our self.
    Symbol* inner_name = constants()->klass_name_at(ioff);
    if (name() == inner_name) {
      // This is really a member class.
      access = iter.inner_access_flags();
      break;
    }
  }
  // Remember to strip ACC_SUPER bit
  return (access & (~JVM_ACC_SUPER));
}

jint InstanceKlass::jvmti_class_status() const {
  jint result = 0;

  if (is_linked()) {
    result |= JVMTI_CLASS_STATUS_VERIFIED | JVMTI_CLASS_STATUS_PREPARED;
  }

  if (is_initialized()) {
    assert(is_linked(), "Class status is not consistent");
    result |= JVMTI_CLASS_STATUS_INITIALIZED;
  }
  if (is_in_error_state()) {
    result |= JVMTI_CLASS_STATUS_ERROR;
  }
  return result;
}

Method* InstanceKlass::method_at_itable(InstanceKlass* holder, int index, TRAPS) {
  bool implements_interface; // initialized by method_at_itable_or_null
  Method* m = method_at_itable_or_null(holder, index,
                                       implements_interface); // out parameter
  if (m != nullptr) {
    assert(implements_interface, "sanity");
    return m;
  } else if (implements_interface) {
    // Throw AbstractMethodError since corresponding itable slot is empty.
    THROW_NULL(vmSymbols::java_lang_AbstractMethodError());
  } else {
    // If the interface isn't implemented by the receiver class,
    // the VM should throw IncompatibleClassChangeError.
    ResourceMark rm(THREAD);
    stringStream ss;
    bool same_module = (module() == holder->module());
    ss.print("Receiver class %s does not implement "
             "the interface %s defining the method to be called "
             "(%s%s%s)",
             external_name(), holder->external_name(),
             (same_module) ? joint_in_module_of_loader(holder) : class_in_module_of_loader(),
             (same_module) ? "" : "; ",
             (same_module) ? "" : holder->class_in_module_of_loader());
    THROW_MSG_NULL(vmSymbols::java_lang_IncompatibleClassChangeError(), ss.as_string());
  }
}

Method* InstanceKlass::method_at_itable_or_null(InstanceKlass* holder, int index, bool& implements_interface) {
  klassItable itable(this);
  for (int i = 0; i < itable.size_offset_table(); i++) {
    itableOffsetEntry* offset_entry = itable.offset_entry(i);
    if (offset_entry->interface_klass() == holder) {
      implements_interface = true;
      itableMethodEntry* ime = offset_entry->first_method_entry(this);
      Method* m = ime[index].method();
      return m;
    }
  }
  implements_interface = false;
  return nullptr; // offset entry not found
}

int InstanceKlass::vtable_index_of_interface_method(Method* intf_method) {
  assert(is_linked(), "required");
  assert(intf_method->method_holder()->is_interface(), "not an interface method");
  assert(is_subtype_of(intf_method->method_holder()), "interface not implemented");

  int vtable_index = Method::invalid_vtable_index;
  Symbol* name = intf_method->name();
  Symbol* signature = intf_method->signature();

  // First check in default method array
  if (!intf_method->is_abstract() && default_methods() != nullptr) {
    int index = find_method_index(default_methods(),
                                  name, signature,
                                  Klass::OverpassLookupMode::find,
                                  Klass::StaticLookupMode::find,
                                  Klass::PrivateLookupMode::find);
    if (index >= 0) {
      vtable_index = default_vtable_indices()->at(index);
    }
  }
  if (vtable_index == Method::invalid_vtable_index) {
    // get vtable_index for miranda methods
    klassVtable vt = vtable();
    vtable_index = vt.index_of_miranda(name, signature);
  }
  return vtable_index;
}

#if INCLUDE_JVMTI
// update default_methods for redefineclasses for methods that are
// not yet in the vtable due to concurrent subclass define and superinterface
// redefinition
// Note: those in the vtable, should have been updated via adjust_method_entries
void InstanceKlass::adjust_default_methods(bool* trace_name_printed) {
  // search the default_methods for uses of either obsolete or EMCP methods
  if (default_methods() != nullptr) {
    for (int index = 0; index < default_methods()->length(); index ++) {
      Method* old_method = default_methods()->at(index);
      if (old_method == nullptr || !old_method->is_old()) {
        continue; // skip uninteresting entries
      }
      assert(!old_method->is_deleted(), "default methods may not be deleted");
      Method* new_method = old_method->get_new_method();
      default_methods()->at_put(index, new_method);

      if (log_is_enabled(Info, redefine, class, update)) {
        ResourceMark rm;
        if (!(*trace_name_printed)) {
          log_info(redefine, class, update)
            ("adjust: klassname=%s default methods from name=%s",
             external_name(), old_method->method_holder()->external_name());
          *trace_name_printed = true;
        }
        log_debug(redefine, class, update, vtables)
          ("default method update: %s(%s) ",
           new_method->name()->as_C_string(), new_method->signature()->as_C_string());
      }
    }
  }
}
#endif // INCLUDE_JVMTI

// On-stack replacement stuff
void InstanceKlass::add_osr_nmethod(nmethod* n) {
  assert_lock_strong(NMethodState_lock);
#ifndef PRODUCT
  nmethod* prev = lookup_osr_nmethod(n->method(), n->osr_entry_bci(), n->comp_level(), true);
  assert(prev == nullptr || !prev->is_in_use() COMPILER2_PRESENT(|| StressRecompilation),
      "redundant OSR recompilation detected. memory leak in CodeCache!");
#endif
  // only one compilation can be active
  assert(n->is_osr_method(), "wrong kind of nmethod");
  n->set_osr_link(osr_nmethods_head());
  set_osr_nmethods_head(n);
  // Raise the highest osr level if necessary
  n->method()->set_highest_osr_comp_level(MAX2(n->method()->highest_osr_comp_level(), n->comp_level()));

  // Get rid of the osr methods for the same bci that have lower levels.
  for (int l = CompLevel_limited_profile; l < n->comp_level(); l++) {
    nmethod *inv = lookup_osr_nmethod(n->method(), n->osr_entry_bci(), l, true);
    if (inv != nullptr && inv->is_in_use()) {
      inv->make_not_entrant("OSR invalidation of lower levels", false /* already being replaced */);
    }
  }
}

// Remove osr nmethod from the list. Return true if found and removed.
bool InstanceKlass::remove_osr_nmethod(nmethod* n) {
  // This is a short non-blocking critical region, so the no safepoint check is ok.
  ConditionalMutexLocker ml(NMethodState_lock, !NMethodState_lock->owned_by_self(), Mutex::_no_safepoint_check_flag);
  assert(n->is_osr_method(), "wrong kind of nmethod");
  nmethod* last = nullptr;
  nmethod* cur  = osr_nmethods_head();
  int max_level = CompLevel_none;  // Find the max comp level excluding n
  Method* m = n->method();
  // Search for match
  bool found = false;
  while(cur != nullptr && cur != n) {
    if (m == cur->method()) {
      // Find max level before n
      max_level = MAX2(max_level, cur->comp_level());
    }
    last = cur;
    cur = cur->osr_link();
  }
  nmethod* next = nullptr;
  if (cur == n) {
    found = true;
    next = cur->osr_link();
    if (last == nullptr) {
      // Remove first element
      set_osr_nmethods_head(next);
    } else {
      last->set_osr_link(next);
    }
  }
  n->set_osr_link(nullptr);
  cur = next;
  while (cur != nullptr) {
    // Find max level after n
    if (m == cur->method()) {
      max_level = MAX2(max_level, cur->comp_level());
    }
    cur = cur->osr_link();
  }
  m->set_highest_osr_comp_level(max_level);
  return found;
}

int InstanceKlass::mark_osr_nmethods(DeoptimizationScope* deopt_scope, const Method* m) {
  ConditionalMutexLocker ml(NMethodState_lock, !NMethodState_lock->owned_by_self(), Mutex::_no_safepoint_check_flag);
  nmethod* osr = osr_nmethods_head();
  int found = 0;
  while (osr != nullptr) {
    assert(osr->is_osr_method(), "wrong kind of nmethod found in chain");
    if (osr->method() == m) {
      deopt_scope->mark(osr);
      found++;
    }
    osr = osr->osr_link();
  }
  return found;
}

nmethod* InstanceKlass::lookup_osr_nmethod(const Method* m, int bci, int comp_level, bool match_level) const {
  ConditionalMutexLocker ml(NMethodState_lock, !NMethodState_lock->owned_by_self(), Mutex::_no_safepoint_check_flag);
  nmethod* osr = osr_nmethods_head();
  nmethod* best = nullptr;
  while (osr != nullptr) {
    assert(osr->is_osr_method(), "wrong kind of nmethod found in chain");
    // There can be a time when a c1 osr method exists but we are waiting
    // for a c2 version. When c2 completes its osr nmethod we will trash
    // the c1 version and only be able to find the c2 version. However
    // while we overflow in the c1 code at back branches we don't want to
    // try and switch to the same code as we are already running

    if (osr->method() == m &&
        (bci == InvocationEntryBci || osr->osr_entry_bci() == bci)) {
      if (match_level) {
        if (osr->comp_level() == comp_level) {
          // Found a match - return it.
          return osr;
        }
      } else {
        if (best == nullptr || (osr->comp_level() > best->comp_level())) {
          if (osr->comp_level() == CompilationPolicy::highest_compile_level()) {
            // Found the best possible - return it.
            return osr;
          }
          best = osr;
        }
      }
    }
    osr = osr->osr_link();
  }

  assert(match_level == false || best == nullptr, "shouldn't pick up anything if match_level is set");
  if (best != nullptr && best->comp_level() >= comp_level) {
    return best;
  }
  return nullptr;
}

// -----------------------------------------------------------------------------------------------------
// Printing

#define BULLET  " - "

static const char* state_names[] = {
  "allocated", "loaded", "linked", "being_initialized", "fully_initialized", "initialization_error"
};

static void print_vtable(intptr_t* start, int len, outputStream* st) {
  for (int i = 0; i < len; i++) {
    intptr_t e = start[i];
    st->print("%d : " INTPTR_FORMAT, i, e);
    if (MetaspaceObj::is_valid((Metadata*)e)) {
      st->print(" ");
      ((Metadata*)e)->print_value_on(st);
    }
    st->cr();
  }
}

static void print_vtable(vtableEntry* start, int len, outputStream* st) {
  return print_vtable(reinterpret_cast<intptr_t*>(start), len, st);
}

const char* InstanceKlass::init_state_name() const {
  return state_names[init_state()];
}

void InstanceKlass::print_on(outputStream* st) const {
  assert(is_klass(), "must be klass");
  Klass::print_on(st);

  st->print(BULLET"instance size:     %d", size_helper());                        st->cr();
  st->print(BULLET"klass size:        %d", size());                               st->cr();
  st->print(BULLET"access:            "); access_flags().print_on(st);            st->cr();
  st->print(BULLET"flags:             "); _misc_flags.print_on(st);               st->cr();
  st->print(BULLET"state:             "); st->print_cr("%s", init_state_name());
  st->print(BULLET"name:              "); name()->print_value_on(st);             st->cr();
  st->print(BULLET"super:             "); Metadata::print_value_on_maybe_null(st, super()); st->cr();
  st->print(BULLET"sub:               ");
  Klass* sub = subklass();
  int n;
  for (n = 0; sub != nullptr; n++, sub = sub->next_sibling()) {
    if (n < MaxSubklassPrintSize) {
      sub->print_value_on(st);
      st->print("   ");
    }
  }
  if (n >= MaxSubklassPrintSize) st->print("(%zd more klasses...)", n - MaxSubklassPrintSize);
  st->cr();

  if (is_interface()) {
    st->print_cr(BULLET"nof implementors:  %d", nof_implementors());
    if (nof_implementors() == 1) {
      st->print_cr(BULLET"implementor:    ");
      st->print("   ");
      implementor()->print_value_on(st);
      st->cr();
    }
  }

  st->print(BULLET"arrays:            "); Metadata::print_value_on_maybe_null(st, array_klasses()); st->cr();
  st->print(BULLET"methods:           "); methods()->print_value_on(st);               st->cr();
  if (Verbose || WizardMode) {
    Array<Method*>* method_array = methods();
    for (int i = 0; i < method_array->length(); i++) {
      st->print("%d : ", i); method_array->at(i)->print_value(); st->cr();
    }
  }
  st->print(BULLET"method ordering:   "); method_ordering()->print_value_on(st);      st->cr();
  if (default_methods() != nullptr) {
    st->print(BULLET"default_methods:   "); default_methods()->print_value_on(st);    st->cr();
    if (Verbose) {
      Array<Method*>* method_array = default_methods();
      for (int i = 0; i < method_array->length(); i++) {
        st->print("%d : ", i); method_array->at(i)->print_value(); st->cr();
      }
    }
  }
  print_on_maybe_null(st, BULLET"default vtable indices:   ", default_vtable_indices());
  st->print(BULLET"local interfaces:  "); local_interfaces()->print_value_on(st);      st->cr();
  st->print(BULLET"trans. interfaces: "); transitive_interfaces()->print_value_on(st); st->cr();

  st->print(BULLET"secondary supers: "); secondary_supers()->print_value_on(st); st->cr();

  st->print(BULLET"hash_slot:         %d", hash_slot()); st->cr();
  st->print(BULLET"secondary bitmap: " UINTX_FORMAT_X_0, _secondary_supers_bitmap); st->cr();

  if (secondary_supers() != nullptr) {
    if (Verbose) {
      bool is_hashed = (_secondary_supers_bitmap != SECONDARY_SUPERS_BITMAP_FULL);
      st->print_cr(BULLET"---- secondary supers (%d words):", _secondary_supers->length());
      for (int i = 0; i < _secondary_supers->length(); i++) {
        ResourceMark rm; // for external_name()
        Klass* secondary_super = _secondary_supers->at(i);
        st->print(BULLET"%2d:", i);
        if (is_hashed) {
          int home_slot = compute_home_slot(secondary_super, _secondary_supers_bitmap);
          int distance = (i - home_slot) & SECONDARY_SUPERS_TABLE_MASK;
          st->print(" dist:%02d:", distance);
        }
        st->print_cr(" %p %s", secondary_super, secondary_super->external_name());
      }
    }
  }
  st->print(BULLET"constants:         "); constants()->print_value_on(st);         st->cr();

  print_on_maybe_null(st, BULLET"class loader data:  ", class_loader_data());
  print_on_maybe_null(st, BULLET"source file:       ", source_file_name());
  if (source_debug_extension() != nullptr) {
    st->print(BULLET"source debug extension:       ");
    st->print("%s", source_debug_extension());
    st->cr();
  }
  print_on_maybe_null(st, BULLET"class annotations:       ", class_annotations());
  print_on_maybe_null(st, BULLET"class type annotations:  ", class_type_annotations());
  print_on_maybe_null(st, BULLET"field annotations:       ", fields_annotations());
  print_on_maybe_null(st, BULLET"field type annotations:  ", fields_type_annotations());
  {
    bool have_pv = false;
    // previous versions are linked together through the InstanceKlass
    for (InstanceKlass* pv_node = previous_versions();
         pv_node != nullptr;
         pv_node = pv_node->previous_versions()) {
      if (!have_pv)
        st->print(BULLET"previous version:  ");
      have_pv = true;
      pv_node->constants()->print_value_on(st);
    }
    if (have_pv) st->cr();
  }

  print_on_maybe_null(st, BULLET"generic signature: ", generic_signature());
  st->print(BULLET"inner classes:     "); inner_classes()->print_value_on(st);     st->cr();
  st->print(BULLET"nest members:     "); nest_members()->print_value_on(st);     st->cr();
  print_on_maybe_null(st, BULLET"record components:     ", record_components());
  st->print(BULLET"permitted subclasses:     "); permitted_subclasses()->print_value_on(st);     st->cr();
  if (java_mirror() != nullptr) {
    st->print(BULLET"java mirror:       ");
    java_mirror()->print_value_on(st);
    st->cr();
  } else {
    st->print_cr(BULLET"java mirror:       null");
  }
  st->print(BULLET"vtable length      %d  (start addr: " PTR_FORMAT ")", vtable_length(), p2i(start_of_vtable())); st->cr();
  if (vtable_length() > 0 && (Verbose || WizardMode))  print_vtable(start_of_vtable(), vtable_length(), st);
  st->print(BULLET"itable length      %d (start addr: " PTR_FORMAT ")", itable_length(), p2i(start_of_itable())); st->cr();
  if (itable_length() > 0 && (Verbose || WizardMode))  print_vtable(start_of_itable(), itable_length(), st);
  st->print_cr(BULLET"---- static fields (%d words):", static_field_size());

  FieldPrinter print_static_field(st);
  ((InstanceKlass*)this)->do_local_static_fields(&print_static_field);
  st->print_cr(BULLET"---- non-static fields (%d words):", nonstatic_field_size());
  FieldPrinter print_nonstatic_field(st);
  InstanceKlass* ik = const_cast<InstanceKlass*>(this);
  ik->print_nonstatic_fields(&print_nonstatic_field);

  st->print(BULLET"non-static oop maps (%d entries): ", nonstatic_oop_map_count());
  OopMapBlock* map     = start_of_nonstatic_oop_maps();
  OopMapBlock* end_map = map + nonstatic_oop_map_count();
  while (map < end_map) {
    st->print("%d-%d ", map->offset(), map->offset() + heapOopSize*(map->count() - 1));
    map++;
  }
  st->cr();
}

void InstanceKlass::print_value_on(outputStream* st) const {
  assert(is_klass(), "must be klass");
  if (Verbose || WizardMode)  access_flags().print_on(st);
  name()->print_value_on(st);
}

void FieldPrinter::do_field(fieldDescriptor* fd) {
  _st->print(BULLET);
   if (_obj == nullptr) {
     fd->print_on(_st);
     _st->cr();
   } else {
     fd->print_on_for(_st, _obj);
     _st->cr();
   }
}


void InstanceKlass::oop_print_on(oop obj, outputStream* st) {
  Klass::oop_print_on(obj, st);

  if (this == vmClasses::String_klass()) {
    typeArrayOop value  = java_lang_String::value(obj);
    juint        length = java_lang_String::length(obj);
    if (value != nullptr &&
        value->is_typeArray() &&
        length <= (juint) value->length()) {
      st->print(BULLET"string: ");
      java_lang_String::print(obj, st);
      st->cr();
    }
  }

  st->print_cr(BULLET"---- fields (total size %zu words):", oop_size(obj));
  FieldPrinter print_field(st, obj);
  print_nonstatic_fields(&print_field);

  if (this == vmClasses::Class_klass()) {
    st->print(BULLET"signature: ");
    java_lang_Class::print_signature(obj, st);
    st->cr();
    Klass* real_klass = java_lang_Class::as_Klass(obj);
    if (real_klass != nullptr && real_klass->is_instance_klass()) {
      st->print_cr(BULLET"---- static fields (%d):", java_lang_Class::static_oop_field_count(obj));
      InstanceKlass::cast(real_klass)->do_local_static_fields(&print_field);
    }
  } else if (this == vmClasses::MethodType_klass()) {
    st->print(BULLET"signature: ");
    java_lang_invoke_MethodType::print_signature(obj, st);
    st->cr();
  }
}

#ifndef PRODUCT

bool InstanceKlass::verify_itable_index(int i) {
  int method_count = klassItable::method_count_for_interface(this);
  assert(i >= 0 && i < method_count, "index out of bounds");
  return true;
}

#endif //PRODUCT

void InstanceKlass::oop_print_value_on(oop obj, outputStream* st) {
  st->print("a ");
  name()->print_value_on(st);
  obj->print_address_on(st);
  if (this == vmClasses::String_klass()
      && java_lang_String::value(obj) != nullptr) {
    ResourceMark rm;
    int len = java_lang_String::length(obj);
    int plen = (len < 24 ? len : 12);
    char* str = java_lang_String::as_utf8_string(obj, 0, plen);
    st->print(" = \"%s\"", str);
    if (len > plen)
      st->print("...[%d]", len);
  } else if (this == vmClasses::Class_klass()) {
    Klass* k = java_lang_Class::as_Klass(obj);
    st->print(" = ");
    if (k != nullptr) {
      k->print_value_on(st);
    } else {
      const char* tname = type2name(java_lang_Class::primitive_type(obj));
      st->print("%s", tname ? tname : "type?");
    }
  } else if (this == vmClasses::MethodType_klass()) {
    st->print(" = ");
    java_lang_invoke_MethodType::print_signature(obj, st);
  } else if (java_lang_boxing_object::is_instance(obj)) {
    st->print(" = ");
    java_lang_boxing_object::print(obj, st);
  } else if (this == vmClasses::LambdaForm_klass()) {
    oop vmentry = java_lang_invoke_LambdaForm::vmentry(obj);
    if (vmentry != nullptr) {
      st->print(" => ");
      vmentry->print_value_on(st);
    }
  } else if (this == vmClasses::MemberName_klass()) {
    Metadata* vmtarget = java_lang_invoke_MemberName::vmtarget(obj);
    if (vmtarget != nullptr) {
      st->print(" = ");
      vmtarget->print_value_on(st);
    } else {
      oop clazz = java_lang_invoke_MemberName::clazz(obj);
      oop name  = java_lang_invoke_MemberName::name(obj);
      if (clazz != nullptr) {
        clazz->print_value_on(st);
      } else {
        st->print("null");
      }
      st->print(".");
      if (name != nullptr) {
        name->print_value_on(st);
      } else {
        st->print("null");
      }
    }
  }
}

const char* InstanceKlass::internal_name() const {
  return external_name();
}

void InstanceKlass::print_class_load_logging(ClassLoaderData* loader_data,
                                             const ModuleEntry* module_entry,
                                             const ClassFileStream* cfs) const {

  if (ClassListWriter::is_enabled()) {
    ClassListWriter::write(this, cfs);
  }

  print_class_load_helper(loader_data, module_entry, cfs);
  print_class_load_cause_logging();
}

void InstanceKlass::print_class_load_helper(ClassLoaderData* loader_data,
                                             const ModuleEntry* module_entry,
                                             const ClassFileStream* cfs) const {

  if (!log_is_enabled(Info, class, load)) {
    return;
  }

  ResourceMark rm;
  LogMessage(class, load) msg;
  stringStream info_stream;

  // Name and class hierarchy info
  info_stream.print("%s", external_name());

  // Source
  if (cfs != nullptr) {
    if (cfs->source() != nullptr) {
      const char* module_name = (module_entry->name() == nullptr) ? UNNAMED_MODULE : module_entry->name()->as_C_string();
      if (module_name != nullptr) {
        // When the boot loader created the stream, it didn't know the module name
        // yet. Let's format it now.
        if (cfs->from_boot_loader_modules_image()) {
          info_stream.print(" source: jrt:/%s", module_name);
        } else {
          info_stream.print(" source: %s", cfs->source());
        }
      } else {
        info_stream.print(" source: %s", cfs->source());
      }
    } else if (loader_data == ClassLoaderData::the_null_class_loader_data()) {
      Thread* current = Thread::current();
      Klass* caller = current->is_Java_thread() ?
        JavaThread::cast(current)->security_get_caller_class(1):
        nullptr;
      // caller can be null, for example, during a JVMTI VM_Init hook
      if (caller != nullptr) {
        info_stream.print(" source: instance of %s", caller->external_name());
      } else {
        // source is unknown
      }
    } else {
      oop class_loader = loader_data->class_loader();
      info_stream.print(" source: %s", class_loader->klass()->external_name());
    }
  } else {
    assert(this->is_shared(), "must be");
    if (MetaspaceShared::is_shared_dynamic((void*)this)) {
      info_stream.print(" source: shared objects file (top)");
    } else {
      info_stream.print(" source: shared objects file");
    }
  }

  msg.info("%s", info_stream.as_string());

  if (log_is_enabled(Debug, class, load)) {
    stringStream debug_stream;

    // Class hierarchy info
    debug_stream.print(" klass: " PTR_FORMAT " super: " PTR_FORMAT,
                       p2i(this),  p2i(superklass()));

    // Interfaces
    if (local_interfaces() != nullptr && local_interfaces()->length() > 0) {
      debug_stream.print(" interfaces:");
      int length = local_interfaces()->length();
      for (int i = 0; i < length; i++) {
        debug_stream.print(" " PTR_FORMAT,
                           p2i(InstanceKlass::cast(local_interfaces()->at(i))));
      }
    }

    // Class loader
    debug_stream.print(" loader: [");
    loader_data->print_value_on(&debug_stream);
    debug_stream.print("]");

    // Classfile checksum
    if (cfs) {
      debug_stream.print(" bytes: %d checksum: %08x",
                         cfs->length(),
                         ClassLoader::crc32(0, (const char*)cfs->buffer(),
                         cfs->length()));
    }

    msg.debug("%s", debug_stream.as_string());
  }
}

void InstanceKlass::print_class_load_cause_logging() const {
  bool log_cause_native = log_is_enabled(Info, class, load, cause, native);
  if (log_cause_native || log_is_enabled(Info, class, load, cause)) {
    JavaThread* current = JavaThread::current();
    ResourceMark rm(current);
    const char* name = external_name();

    if (LogClassLoadingCauseFor == nullptr ||
        (strcmp("*", LogClassLoadingCauseFor) != 0 &&
         strstr(name, LogClassLoadingCauseFor) == nullptr)) {
        return;
    }

    // Log Java stack first
    {
      LogMessage(class, load, cause) msg;
      NonInterleavingLogStream info_stream{LogLevelType::Info, msg};

      info_stream.print_cr("Java stack when loading %s:", name);
      current->print_stack_on(&info_stream);
    }

    // Log native stack second
    if (log_cause_native) {
      // Log to string first so that lines can be indented
      stringStream stack_stream;
      char buf[O_BUFLEN];
      address lastpc = nullptr;
      NativeStackPrinter nsp(current);
      nsp.print_stack(&stack_stream, buf, sizeof(buf), lastpc,
                      true /* print_source_info */, -1 /* max stack */);

      LogMessage(class, load, cause, native) msg;
      NonInterleavingLogStream info_stream{LogLevelType::Info, msg};
      info_stream.print_cr("Native stack when loading %s:", name);

      // Print each native stack line to the log
      int size = (int) stack_stream.size();
      char* stack = stack_stream.as_string();
      char* stack_end = stack + size;
      char* line_start = stack;
      for (char* p = stack; p < stack_end; p++) {
        if (*p == '\n') {
          *p = '\0';
          info_stream.print_cr("\t%s", line_start);
          line_start = p + 1;
        }
      }
      if (line_start < stack_end) {
        info_stream.print_cr("\t%s", line_start);
      }
    }
  }
}

// Verification

class VerifyFieldClosure: public BasicOopIterateClosure {
 protected:
  template <class T> void do_oop_work(T* p) {
    oop obj = RawAccess<>::oop_load(p);
    if (!oopDesc::is_oop_or_null(obj)) {
      tty->print_cr("Failed: " PTR_FORMAT " -> " PTR_FORMAT, p2i(p), p2i(obj));
      Universe::print_on(tty);
      guarantee(false, "boom");
    }
  }
 public:
  virtual void do_oop(oop* p)       { VerifyFieldClosure::do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { VerifyFieldClosure::do_oop_work(p); }
};

void InstanceKlass::verify_on(outputStream* st) {
#ifndef PRODUCT
  // Avoid redundant verifies, this really should be in product.
  if (_verify_count == Universe::verify_count()) return;
  _verify_count = Universe::verify_count();
#endif

  // Verify Klass
  Klass::verify_on(st);

  // Verify that klass is present in ClassLoaderData
  guarantee(class_loader_data()->contains_klass(this),
            "this class isn't found in class loader data");

  // Verify vtables
  if (is_linked()) {
    // $$$ This used to be done only for m/s collections.  Doing it
    // always seemed a valid generalization.  (DLD -- 6/00)
    vtable().verify(st);
  }

  // Verify first subklass
  if (subklass() != nullptr) {
    guarantee(subklass()->is_klass(), "should be klass");
  }

  // Verify siblings
  Klass* super = this->super();
  Klass* sib = next_sibling();
  if (sib != nullptr) {
    if (sib == this) {
      fatal("subclass points to itself " PTR_FORMAT, p2i(sib));
    }

    guarantee(sib->is_klass(), "should be klass");
    guarantee(sib->super() == super, "siblings should have same superklass");
  }

  // Verify local interfaces
  if (local_interfaces()) {
    Array<InstanceKlass*>* local_interfaces = this->local_interfaces();
    for (int j = 0; j < local_interfaces->length(); j++) {
      InstanceKlass* e = local_interfaces->at(j);
      guarantee(e->is_klass() && e->is_interface(), "invalid local interface");
    }
  }

  // Verify transitive interfaces
  if (transitive_interfaces() != nullptr) {
    Array<InstanceKlass*>* transitive_interfaces = this->transitive_interfaces();
    for (int j = 0; j < transitive_interfaces->length(); j++) {
      InstanceKlass* e = transitive_interfaces->at(j);
      guarantee(e->is_klass() && e->is_interface(), "invalid transitive interface");
    }
  }

  // Verify methods
  if (methods() != nullptr) {
    Array<Method*>* methods = this->methods();
    for (int j = 0; j < methods->length(); j++) {
      guarantee(methods->at(j)->is_method(), "non-method in methods array");
    }
    for (int j = 0; j < methods->length() - 1; j++) {
      Method* m1 = methods->at(j);
      Method* m2 = methods->at(j + 1);
      guarantee(m1->name()->fast_compare(m2->name()) <= 0, "methods not sorted correctly");
    }
  }

  // Verify method ordering
  if (method_ordering() != nullptr) {
    Array<int>* method_ordering = this->method_ordering();
    int length = method_ordering->length();
    if (JvmtiExport::can_maintain_original_method_order() ||
        ((CDSConfig::is_using_archive() || CDSConfig::is_dumping_archive()) && length != 0)) {
      guarantee(length == methods()->length(), "invalid method ordering length");
      jlong sum = 0;
      for (int j = 0; j < length; j++) {
        int original_index = method_ordering->at(j);
        guarantee(original_index >= 0, "invalid method ordering index");
        guarantee(original_index < length, "invalid method ordering index");
        sum += original_index;
      }
      // Verify sum of indices 0,1,...,length-1
      guarantee(sum == ((jlong)length*(length-1))/2, "invalid method ordering sum");
    } else {
      guarantee(length == 0, "invalid method ordering length");
    }
  }

  // Verify default methods
  if (default_methods() != nullptr) {
    Array<Method*>* methods = this->default_methods();
    for (int j = 0; j < methods->length(); j++) {
      guarantee(methods->at(j)->is_method(), "non-method in methods array");
    }
    for (int j = 0; j < methods->length() - 1; j++) {
      Method* m1 = methods->at(j);
      Method* m2 = methods->at(j + 1);
      guarantee(m1->name()->fast_compare(m2->name()) <= 0, "methods not sorted correctly");
    }
  }

  // Verify JNI static field identifiers
  if (jni_ids() != nullptr) {
    jni_ids()->verify(this);
  }

  // Verify other fields
  if (constants() != nullptr) {
    guarantee(constants()->is_constantPool(), "should be constant pool");
  }
}

void InstanceKlass::oop_verify_on(oop obj, outputStream* st) {
  Klass::oop_verify_on(obj, st);
  VerifyFieldClosure blk;
  obj->oop_iterate(&blk);
}


// JNIid class for jfieldIDs only
// Note to reviewers:
// These JNI functions are just moved over to column 1 and not changed
// in the compressed oops workspace.
JNIid::JNIid(Klass* holder, int offset, JNIid* next) {
  _holder = holder;
  _offset = offset;
  _next = next;
  DEBUG_ONLY(_is_static_field_id = false;)
}


JNIid* JNIid::find(int offset) {
  JNIid* current = this;
  while (current != nullptr) {
    if (current->offset() == offset) return current;
    current = current->next();
  }
  return nullptr;
}

void JNIid::deallocate(JNIid* current) {
  while (current != nullptr) {
    JNIid* next = current->next();
    delete current;
    current = next;
  }
}


void JNIid::verify(Klass* holder) {
  int first_field_offset  = InstanceMirrorKlass::offset_of_static_fields();
  int end_field_offset;
  end_field_offset = first_field_offset + (InstanceKlass::cast(holder)->static_field_size() * wordSize);

  JNIid* current = this;
  while (current != nullptr) {
    guarantee(current->holder() == holder, "Invalid klass in JNIid");
#ifdef ASSERT
    int o = current->offset();
    if (current->is_static_field_id()) {
      guarantee(o >= first_field_offset  && o < end_field_offset,  "Invalid static field offset in JNIid");
    }
#endif
    current = current->next();
  }
}

void InstanceKlass::set_init_state(ClassState state) {
#ifdef ASSERT
  bool good_state = is_shared() ? (_init_state <= state)
                                               : (_init_state < state);
  assert(good_state || state == allocated, "illegal state transition");
#endif
  assert(_init_thread == nullptr, "should be cleared before state change");
  Atomic::release_store(&_init_state, state);
}

#if INCLUDE_JVMTI

// RedefineClasses() support for previous versions

// Globally, there is at least one previous version of a class to walk
// during class unloading, which is saved because old methods in the class
// are still running.   Otherwise the previous version list is cleaned up.
bool InstanceKlass::_should_clean_previous_versions = false;

// Returns true if there are previous versions of a class for class
// unloading only. Also resets the flag to false. purge_previous_version
// will set the flag to true if there are any left, i.e., if there's any
// work to do for next time. This is to avoid the expensive code cache
// walk in CLDG::clean_deallocate_lists().
bool InstanceKlass::should_clean_previous_versions_and_reset() {
  bool ret = _should_clean_previous_versions;
  log_trace(redefine, class, iklass, purge)("Class unloading: should_clean_previous_versions = %s",
     ret ? "true" : "false");
  _should_clean_previous_versions = false;
  return ret;
}

// This nulls out jmethodIDs for all methods in 'klass'
// It needs to be called explicitly for all previous versions of a class because these may not be cleaned up
// during class unloading.
// We can not use the jmethodID cache associated with klass directly because the 'previous' versions
// do not have the jmethodID cache filled in. Instead, we need to lookup jmethodID for each method and this
// is expensive - O(n) for one jmethodID lookup. For all contained methods it is O(n^2).
// The reason for expensive jmethodID lookup for each method is that there is no direct link between method and jmethodID.
void InstanceKlass::clear_jmethod_ids(InstanceKlass* klass) {
  Array<Method*>* method_refs = klass->methods();
  for (int k = 0; k < method_refs->length(); k++) {
    Method* method = method_refs->at(k);
    if (method != nullptr && method->is_obsolete()) {
      method->clear_jmethod_id();
    }
  }
}

// Purge previous versions before adding new previous versions of the class and
// during class unloading.
void InstanceKlass::purge_previous_version_list() {
  assert(SafepointSynchronize::is_at_safepoint(), "only called at safepoint");
  assert(has_been_redefined(), "Should only be called for main class");

  // Quick exit.
  if (previous_versions() == nullptr) {
    return;
  }

  // This klass has previous versions so see what we can cleanup
  // while it is safe to do so.

  int deleted_count = 0;    // leave debugging breadcrumbs
  int live_count = 0;
  ClassLoaderData* loader_data = class_loader_data();
  assert(loader_data != nullptr, "should never be null");

  ResourceMark rm;
  log_trace(redefine, class, iklass, purge)("%s: previous versions", external_name());

  // previous versions are linked together through the InstanceKlass
  InstanceKlass* pv_node = previous_versions();
  InstanceKlass* last = this;
  int version = 0;

  // check the previous versions list
  for (; pv_node != nullptr; ) {

    ConstantPool* pvcp = pv_node->constants();
    assert(pvcp != nullptr, "cp ref was unexpectedly cleared");

    if (!pvcp->on_stack()) {
      // If the constant pool isn't on stack, none of the methods
      // are executing.  Unlink this previous_version.
      // The previous version InstanceKlass is on the ClassLoaderData deallocate list
      // so will be deallocated during the next phase of class unloading.
      log_trace(redefine, class, iklass, purge)
        ("previous version " PTR_FORMAT " is dead.", p2i(pv_node));
      // Unlink from previous version list.
      assert(pv_node->class_loader_data() == loader_data, "wrong loader_data");
      InstanceKlass* next = pv_node->previous_versions();
      clear_jmethod_ids(pv_node); // jmethodID maintenance for the unloaded class
      pv_node->link_previous_versions(nullptr);   // point next to null
      last->link_previous_versions(next);
      // Delete this node directly. Nothing is referring to it and we don't
      // want it to increase the counter for metadata to delete in CLDG.
      MetadataFactory::free_metadata(loader_data, pv_node);
      pv_node = next;
      deleted_count++;
      version++;
      continue;
    } else {
      assert(pvcp->pool_holder() != nullptr, "Constant pool with no holder");
      guarantee (!loader_data->is_unloading(), "unloaded classes can't be on the stack");
      live_count++;
      if (pvcp->is_shared()) {
        // Shared previous versions can never be removed so no cleaning is needed.
        log_trace(redefine, class, iklass, purge)("previous version " PTR_FORMAT " is shared", p2i(pv_node));
      } else {
        // Previous version alive, set that clean is needed for next time.
        _should_clean_previous_versions = true;
        log_trace(redefine, class, iklass, purge)("previous version " PTR_FORMAT " is alive", p2i(pv_node));
      }
    }

    // next previous version
    last = pv_node;
    pv_node = pv_node->previous_versions();
    version++;
  }
  log_trace(redefine, class, iklass, purge)
    ("previous version stats: live=%d, deleted=%d", live_count, deleted_count);
}

void InstanceKlass::mark_newly_obsolete_methods(Array<Method*>* old_methods,
                                                int emcp_method_count) {
  int obsolete_method_count = old_methods->length() - emcp_method_count;

  if (emcp_method_count != 0 && obsolete_method_count != 0 &&
      _previous_versions != nullptr) {
    // We have a mix of obsolete and EMCP methods so we have to
    // clear out any matching EMCP method entries the hard way.
    int local_count = 0;
    for (int i = 0; i < old_methods->length(); i++) {
      Method* old_method = old_methods->at(i);
      if (old_method->is_obsolete()) {
        // only obsolete methods are interesting
        Symbol* m_name = old_method->name();
        Symbol* m_signature = old_method->signature();

        // previous versions are linked together through the InstanceKlass
        int j = 0;
        for (InstanceKlass* prev_version = _previous_versions;
             prev_version != nullptr;
             prev_version = prev_version->previous_versions(), j++) {

          Array<Method*>* method_refs = prev_version->methods();
          for (int k = 0; k < method_refs->length(); k++) {
            Method* method = method_refs->at(k);

            if (!method->is_obsolete() &&
                method->name() == m_name &&
                method->signature() == m_signature) {
              // The current RedefineClasses() call has made all EMCP
              // versions of this method obsolete so mark it as obsolete
              log_trace(redefine, class, iklass, add)
                ("%s(%s): flush obsolete method @%d in version @%d",
                 m_name->as_C_string(), m_signature->as_C_string(), k, j);

              method->set_is_obsolete();
              break;
            }
          }

          // The previous loop may not find a matching EMCP method, but
          // that doesn't mean that we can optimize and not go any
          // further back in the PreviousVersion generations. The EMCP
          // method for this generation could have already been made obsolete,
          // but there still may be an older EMCP method that has not
          // been made obsolete.
        }

        if (++local_count >= obsolete_method_count) {
          // no more obsolete methods so bail out now
          break;
        }
      }
    }
  }
}

// Save the scratch_class as the previous version if any of the methods are running.
// The previous_versions are used to set breakpoints in EMCP methods and they are
// also used to clean MethodData links to redefined methods that are no longer running.
void InstanceKlass::add_previous_version(InstanceKlass* scratch_class,
                                         int emcp_method_count) {
  assert(Thread::current()->is_VM_thread(),
         "only VMThread can add previous versions");

  ResourceMark rm;
  log_trace(redefine, class, iklass, add)
    ("adding previous version ref for %s, EMCP_cnt=%d", scratch_class->external_name(), emcp_method_count);

  // Clean out old previous versions for this class
  purge_previous_version_list();

  // Mark newly obsolete methods in remaining previous versions.  An EMCP method from
  // a previous redefinition may be made obsolete by this redefinition.
  Array<Method*>* old_methods = scratch_class->methods();
  mark_newly_obsolete_methods(old_methods, emcp_method_count);

  // If the constant pool for this previous version of the class
  // is not marked as being on the stack, then none of the methods
  // in this previous version of the class are on the stack so
  // we don't need to add this as a previous version.
  ConstantPool* cp_ref = scratch_class->constants();
  if (!cp_ref->on_stack()) {
    log_trace(redefine, class, iklass, add)("scratch class not added; no methods are running");
    scratch_class->class_loader_data()->add_to_deallocate_list(scratch_class);
    return;
  }

  // Add previous version if any methods are still running or if this is
  // a shared class which should never be removed.
  assert(scratch_class->previous_versions() == nullptr, "shouldn't have a previous version");
  scratch_class->link_previous_versions(previous_versions());
  link_previous_versions(scratch_class);
  if (cp_ref->is_shared()) {
    log_trace(redefine, class, iklass, add) ("scratch class added; class is shared");
  } else {
    //  We only set clean_previous_versions flag for processing during class
    // unloading for non-shared classes.
    _should_clean_previous_versions = true;
    log_trace(redefine, class, iklass, add) ("scratch class added; one of its methods is on_stack.");
  }
} // end add_previous_version()

#endif // INCLUDE_JVMTI

Method* InstanceKlass::method_with_idnum(int idnum) const {
  Method* m = nullptr;
  if (idnum < methods()->length()) {
    m = methods()->at(idnum);
  }
  if (m == nullptr || m->method_idnum() != idnum) {
    for (int index = 0; index < methods()->length(); ++index) {
      m = methods()->at(index);
      if (m->method_idnum() == idnum) {
        return m;
      }
    }
    // None found, return null for the caller to handle.
    return nullptr;
  }
  return m;
}


Method* InstanceKlass::method_with_orig_idnum(int idnum) const {
  if (idnum >= methods()->length()) {
    return nullptr;
  }
  Method* m = methods()->at(idnum);
  if (m != nullptr && m->orig_method_idnum() == idnum) {
    return m;
  }
  // Obsolete method idnum does not match the original idnum
  for (int index = 0; index < methods()->length(); ++index) {
    m = methods()->at(index);
    if (m->orig_method_idnum() == idnum) {
      return m;
    }
  }
  // None found, return null for the caller to handle.
  return nullptr;
}


Method* InstanceKlass::method_with_orig_idnum(int idnum, int version) const {
  const InstanceKlass* holder = get_klass_version(version);
  if (holder == nullptr) {
    return nullptr; // The version of klass is gone, no method is found
  }
  return holder->method_with_orig_idnum(idnum);
}

#if INCLUDE_JVMTI
JvmtiCachedClassFileData* InstanceKlass::get_cached_class_file() {
  return _cached_class_file;
}

jint InstanceKlass::get_cached_class_file_len() {
  return VM_RedefineClasses::get_cached_class_file_len(_cached_class_file);
}

unsigned char * InstanceKlass::get_cached_class_file_bytes() {
  return VM_RedefineClasses::get_cached_class_file_bytes(_cached_class_file);
}
#endif

// Make a step iterating over the class hierarchy under the root class.
// Skips subclasses if requested.
void ClassHierarchyIterator::next() {
  assert(_current != nullptr, "required");
  if (_visit_subclasses && _current->subklass() != nullptr) {
    _current = _current->subklass();
    return; // visit next subclass
  }
  _visit_subclasses = true; // reset
  while (_current->next_sibling() == nullptr && _current != _root) {
    _current = _current->superklass(); // backtrack; no more sibling subclasses left
  }
  if (_current == _root) {
    // Iteration is over (back at root after backtracking). Invalidate the iterator.
    _current = nullptr;
    return;
  }
  _current = _current->next_sibling();
  return; // visit next sibling subclass
}
