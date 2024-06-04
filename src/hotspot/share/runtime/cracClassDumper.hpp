#ifndef SHARE_RUNTIME_CRACCLASSDUMPER_HPP
#define SHARE_RUNTIME_CRACCLASSDUMPER_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

// Dumps runtime class data for CRaC portable mode.
//
// The dump is expected to be accompanied by an HPROF heap dump from the
// heapDumper, hence IDs used are the same as heapDumper uses and some class
// data available from there is not duplicated.
//
// First, after the header, IDs of primitive array classes are dumped, followed
// by dumps of instance classes.
//
// Instance classes are sorted so that for any instance class C the following
// instance classes are dumped ahead of C:
// 1. Class of C's class loader.
// 2. Class of C's class loader's parent.
// 3. C's super class.
// 4. Interfaces implemented by C.
// This ordering makes it easier to load classes as the dump is being parsed.
//
// Each primitive-array and instance class is followed by IDs of object array
// classes sorted by ascending dimensionality.
struct CracClassDumper : public AllStatic {
  // Kinds of classes with regards to how they were loaded.
  enum ClassLoadingKind : u1 {
    NORMAL            = 0,
    NON_STRONG_HIDDEN = 1,
    STRONG_HIDDEN     = 2
  };
  // Bit positions in compressed VM options.
  enum VMOptionShift : u1 {
    is_sync_on_value_based_classes_diagnosed_shift = 0,
    are_all_annotations_preserved_shift            = 1,
    num_vm_options,
  };
  // Bit positions of in resolved method entries' flags.
  enum ResolvedMethodEntryFlagShift : u1 {
    is_vfinal_shift           = 0,
    is_final_shift            = 1,
    is_forced_virtual_shift   = 2,
    has_appendix_shift        = 3,
    has_local_signature_shift = 4,
    num_method_entry_flags,
  };
  // For null class metadata arrays.
  static constexpr u4 NO_ARRAY_SENTINEL = 0xFFFFFFFF;
  // For null cached class file.
  static constexpr jint NO_CACHED_CLASS_FILE_SENTINEL = -1;

  // Dumps the data into the specified file, possibly overwriting it if the
  // corresponding parameter is set to true, Returns nullptr on success, or a
  // pointer to a static IO error message otherwise.
  //
  // Must be called on a safepoint.
  static const char *dump(const char *path, bool overwrite = false);
};

#endif // SHARE_RUNTIME_CRACCLASSDUMPER_HPP
