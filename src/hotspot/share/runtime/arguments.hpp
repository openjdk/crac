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

#ifndef SHARE_RUNTIME_ARGUMENTS_HPP
#define SHARE_RUNTIME_ARGUMENTS_HPP

#include "logging/logLevel.hpp"
#include "logging/logTag.hpp"
#include "memory/allStatic.hpp"
#include "memory/allocation.hpp"
#include "runtime/globals.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/vmEnums.hpp"

// Arguments parses the command line and recognizes options

class JVMFlag;

// Invocation API hook typedefs (these should really be defined in jni.h)
extern "C" {
  typedef void (JNICALL *abort_hook_t)(void);
  typedef void (JNICALL *exit_hook_t)(jint code);
  typedef jint (JNICALL *vfprintf_hook_t)(FILE *fp, const char *format, va_list args)  ATTRIBUTE_PRINTF(2, 0);
}

// Obsolete or deprecated -XX flag.
struct SpecialFlag {
  const char* name;
  JDK_Version deprecated_in; // When the deprecation warning started (or "undefined").
  JDK_Version obsolete_in;   // When the obsolete warning started (or "undefined").
  JDK_Version expired_in;    // When the option expires (or "undefined").
};

struct LegacyGCLogging {
    const char* file;        // null -> stdout
    int lastFlag;            // 0 not set; 1 -> -verbose:gc; 2 -> -Xloggc
};

// PathString is used as:
//  - the underlying value for a SystemProperty
//  - the path portion of an --patch-module module/path pair
//  - the string that represents the boot class path, Arguments::_boot_class_path.
class PathString : public CHeapObj<mtArguments> {
 protected:
  char* _value;
 public:
  char* value() const { return _value; }

  // return false iff OOM && alloc_failmode == AllocFailStrategy::RETURN_NULL
  bool set_value(const char *value, AllocFailType alloc_failmode = AllocFailStrategy::EXIT_OOM);
  void append_value(const char *value);

  PathString(const char* value);
  ~PathString();

  // for JVM_ReadSystemPropertiesInfo
  static int value_offset_in_bytes()  { return (int)offset_of(PathString, _value);  }
};

// ModulePatchPath records the module/path pair as specified to --patch-module.
class ModulePatchPath : public CHeapObj<mtInternal> {
private:
  char* _module_name;
  PathString* _path;
public:
  ModulePatchPath(const char* module_name, const char* path);
  ~ModulePatchPath();

  inline const char* module_name() const { return _module_name; }
  inline char* path_string() const { return _path->value(); }
};

// Element describing System and User (-Dkey=value flags) defined property.
//
// An internal SystemProperty is one that has been removed in
// jdk.internal.VM.saveAndRemoveProperties, like jdk.boot.class.path.append.
//
class SystemProperty : public PathString {
 private:
  char*           _key;
  SystemProperty* _next;
  bool            _internal;
  bool            _writeable;

 public:
  // Accessors
  char* value() const                 { return PathString::value(); }
  const char* key() const             { return _key; }
  bool internal() const               { return _internal; }
  SystemProperty* next() const        { return _next; }
  void set_next(SystemProperty* next) { _next = next; }
  bool writeable() const              { return _writeable; }

  bool readable() const {
    return !_internal || (strcmp(_key, "jdk.boot.class.path.append") == 0 &&
                          value() != nullptr);
  }

  // A system property should only have its value set
  // via an external interface if it is a writeable property.
  // The internal, non-writeable property jdk.boot.class.path.append
  // is the only exception to this rule.  It can be set externally
  // via -Xbootclasspath/a or JVMTI OnLoad phase call to AddToBootstrapClassLoaderSearch.
  // In those cases for jdk.boot.class.path.append, the base class
  // set_value and append_value methods are called directly.
  void set_writeable_value(const char *value) {
    if (writeable()) {
      set_value(value);
    }
  }
  void append_writeable_value(const char *value) {
    if (writeable()) {
      append_value(value);
    }
  }

  // Constructor
  SystemProperty(const char* key, const char* value, bool writeable, bool internal = false);

  // for JVM_ReadSystemPropertiesInfo
  static int key_offset_in_bytes()  { return (int)offset_of(SystemProperty, _key);  }
  static int next_offset_in_bytes() { return (int)offset_of(SystemProperty, _next); }
};

// Helper class for controlling the lifetime of JavaVMInitArgs objects.
class ScopedVMInitArgs;

struct VMInitArgsGroup;
template <typename E, MemTag MT> class GrowableArrayCHeap;

class Arguments : AllStatic {
  friend class VMStructs;
  friend class JvmtiExport;
  friend class ArgumentsTest;
  friend class LargeOptionsTest;
 public:
  // Operation modi
  enum Mode {
    _int,       // corresponds to -Xint
    _mixed,     // corresponds to -Xmixed
    _comp       // corresponds to -Xcomp
  };

  enum ArgsRange {
    arg_unreadable = -3,
    arg_too_small  = -2,
    arg_too_big    = -1,
    arg_in_range   = 0
  };

  enum PropertyAppendable {
    AppendProperty,
    AddProperty
  };

  enum PropertyWriteable {
    WriteableProperty,
    UnwriteableProperty
  };

  enum PropertyInternal {
    InternalProperty,
    ExternalProperty
  };

 private:

  // a pointer to the flags file name if it is specified
  static char*  _jvm_flags_file;
  // an array containing all flags specified in the .hotspotrc file
  static char** _jvm_flags_array;
  static int    _num_jvm_flags;
  // an array containing all jvm arguments specified in the command line
  static char** _jvm_args_array;
  static int    _num_jvm_args;
  // string containing all java command (class/jarfile name and app args)
  static char* _java_command;
  static char* _java_command_crac;
  // number of unique modules specified in the --add-modules option
  static unsigned int _addmods_count;
#if INCLUDE_JVMCI
  // was jdk.internal.vm.ci module specified in the --add-modules option?
  static bool _jvmci_module_added;
#endif

  // Property list
  static SystemProperty* _system_properties;

  // Quick accessor to System properties in the list:
  static SystemProperty *_sun_boot_library_path;
  static SystemProperty *_java_library_path;
  static SystemProperty *_java_home;
  static SystemProperty *_java_class_path;
  static SystemProperty *_jdk_boot_class_path_append;
  static SystemProperty *_vm_info;

  // --patch-module=module=<file>(<pathsep><file>)*
  // Each element contains the associated module name, path
  // string pair as specified to --patch-module.
  static GrowableArray<ModulePatchPath*>* _patch_mod_prefix;

  // The constructed value of the system class path after
  // argument processing and JVMTI OnLoad additions via
  // calls to AddToBootstrapClassLoaderSearch.  This is the
  // final form before ClassLoader::setup_bootstrap_search().
  // Note: since --patch-module is a module name/path pair, the
  // boot class path string no longer contains the "prefix"
  // to the boot class path base piece as it did when
  // -Xbootclasspath/p was supported.
  static PathString* _boot_class_path;

  // Set if a modular java runtime image is present vs. a build with exploded modules
  static bool _has_jimage;

  // temporary: to emit warning if the default ext dirs are not empty.
  // remove this variable when the warning is no longer needed.
  static char* _ext_dirs;

  // java.vendor.url.bug, bug reporting URL for fatal errors.
  static const char* _java_vendor_url_bug;

  // sun.java.launcher, private property to provide information about
  // java launcher
  static const char* _sun_java_launcher;

  // was this VM created with the -XX:+ExecutingUnitTests option
  static bool _executing_unit_tests;

  // for legacy gc options (-verbose:gc and -Xloggc:)
  static LegacyGCLogging _legacyGCLogging;

  // Value of the conservative maximum heap alignment needed
  static size_t  _conservative_max_heap_alignment;

  // Operation modi
  static Mode _mode;

  // preview features
  static bool _enable_preview;

  // jdwp
  static bool _has_jdwp_agent;

  // Used to save default settings
  static bool _AlwaysCompileLoopMethods;
  static bool _UseOnStackReplacement;
  static bool _BackgroundCompilation;
  static bool _ClipInlining;

  // GC ergonomics
  static void set_conservative_max_heap_alignment();
  static void set_use_compressed_oops();
  static jint set_ergonomics_flags();
  static void set_compact_headers_flags();
  // Limits the given heap size by the maximum amount of virtual
  // memory this process is currently allowed to use. It also takes
  // the virtual-to-physical ratio of the current GC into account.
  static size_t limit_heap_by_allocatable_memory(size_t size);
  // Setup heap size
  static void set_heap_size();

  // Bytecode rewriting
  static void set_bytecode_flags();

  // Invocation API hooks
  static abort_hook_t     _abort_hook;
  static exit_hook_t      _exit_hook;
  static vfprintf_hook_t  _vfprintf_hook;

  // prop points to a string of the form key=value
  // Parse the string to extract key and the value
  static void get_key_value(const char* prop, const char** key, const char** value);

  // System properties
  static bool add_property(const char* prop, PropertyWriteable writeable=WriteableProperty,
                           PropertyInternal internal=ExternalProperty);

  // Used for module system related properties: converted from command-line flags.
  // Basic properties are writeable as they operate as "last one wins" and will get overwritten.
  // Numbered properties are never writeable, and always internal.
  static bool create_module_property(const char* prop_name, const char* prop_value, PropertyInternal internal);
  static bool create_numbered_module_property(const char* prop_base_name, const char* prop_value, unsigned int count);

  static int process_patch_mod_option(const char* patch_mod_tail);

  // Aggressive optimization flags.
  static jint set_aggressive_opts_flags();

  static jint set_aggressive_heap_flags();

  // Argument parsing
  static bool parse_argument(const char* arg, JVMFlagOrigin origin);
  static bool process_argument(const char* arg, jboolean ignore_unrecognized, JVMFlagOrigin origin);
  static void process_java_launcher_argument(const char*, void*);
  static jint parse_options_environment_variable(const char* name, ScopedVMInitArgs* vm_args);
  static jint parse_java_tool_options_environment_variable(ScopedVMInitArgs* vm_args);
  static jint parse_java_options_environment_variable(ScopedVMInitArgs* vm_args);
  static jint parse_jdk_aot_vm_options_environment_variable(GrowableArrayCHeap<VMInitArgsGroup, mtArguments>* all_args,
                                                            ScopedVMInitArgs* jdk_aot_vm_options_args);
  static jint parse_vm_options_file(const char* file_name, ScopedVMInitArgs* vm_args);
  static jint parse_options_buffer(const char* name, char* buffer, const size_t buf_len, ScopedVMInitArgs* vm_args);
  static jint parse_xss(const JavaVMOption* option, const char* tail, intx* out_ThreadStackSize);
  static jint insert_vm_options_file(const JavaVMInitArgs* args,
                                     const char* vm_options_file,
                                     const int vm_options_file_pos,
                                     ScopedVMInitArgs* vm_options_file_args,
                                     ScopedVMInitArgs* args_out);
  static bool args_contains_vm_options_file_arg(const JavaVMInitArgs* args);
  static jint expand_vm_options_as_needed(const JavaVMInitArgs* args_in,
                                          ScopedVMInitArgs* mod_args,
                                          JavaVMInitArgs** args_out);
  static jint match_special_option_and_act(const JavaVMInitArgs* args,
                                           ScopedVMInitArgs* args_out);

  static bool handle_deprecated_print_gc_flags();

  static jint parse_vm_init_args(GrowableArrayCHeap<VMInitArgsGroup, mtArguments>* all_args);
  static jint parse_each_vm_init_arg(const JavaVMInitArgs* args, JVMFlagOrigin origin);
  static jint finalize_vm_init_args();
  static bool is_bad_option(const JavaVMOption* option, jboolean ignore, const char* option_type);

  static bool is_bad_option(const JavaVMOption* option, jboolean ignore) {
    return is_bad_option(option, ignore, nullptr);
  }

  static void describe_range_error(ArgsRange errcode);
  static ArgsRange check_memory_size(julong size, julong min_size, julong max_size);
  static ArgsRange parse_memory_size(const char* s, julong* long_arg,
                                     julong min_size, julong max_size = max_uintx);

  // methods to build strings from individual args
  static void build_jvm_args(const char* arg);
  static void build_jvm_flags(const char* arg);
  static void add_string(char*** bldarray, int* count, const char* arg);
  static const char* build_resource_string(char** args, int count);

  // Returns true if the flag is obsolete (and not yet expired).
  // In this case the 'version' buffer is filled in with
  // the version number when the flag became obsolete.
  static bool is_obsolete_flag(const char* flag_name, JDK_Version* version);

  // Returns 1 if the flag is deprecated (and not yet obsolete or expired).
  //     In this case the 'version' buffer is filled in with the version number when
  //     the flag became deprecated.
  // Returns -1 if the flag is expired or obsolete.
  // Returns 0 otherwise.
  static int is_deprecated_flag(const char* flag_name, JDK_Version* version);

  // Return the real name for the flag passed on the command line (either an alias name or "flag_name").
  static const char* real_flag_name(const char *flag_name);
  static JVMFlag* find_jvm_flag(const char* name, size_t name_length);

  // Return the "real" name for option arg if arg is an alias, and print a warning if arg is deprecated.
  // Return nullptr if the arg has expired.
  static const char* handle_aliases_and_deprecation(const char* arg);
  static size_t _default_SharedBaseAddress; // The default value specified in globals.hpp

  static bool internal_module_property_helper(const char* property, bool check_for_cds);

 public:
  // Parses the arguments, first phase
  static jint parse(const JavaVMInitArgs* args);
  // Parse a string for a unsigned integer.  Returns true if value
  // is an unsigned integer greater than or equal to the minimum
  // parameter passed and returns the value in uint_arg.  Returns
  // false otherwise, with uint_arg undefined.
  static bool parse_uint(const char* value, uint* uintx_arg,
                         uint min_size);
  // Apply ergonomics
  static jint apply_ergo();
  // Adjusts the arguments after the OS have adjusted the arguments
  static jint adjust_after_os();

  // Check consistency or otherwise of VM argument settings
  static bool check_vm_args_consistency();
  // Used by os_solaris
  static bool process_settings_file(const char* file_name, bool should_exist, jboolean ignore_unrecognized);

  static size_t conservative_max_heap_alignment() { return _conservative_max_heap_alignment; }
  // Return the maximum size a heap with compressed oops can take
  static size_t max_heap_for_compressed_oops();

  // return a char* array containing all options
  static char** jvm_flags_array()          { return _jvm_flags_array; }
  static char** jvm_args_array()           { return _jvm_args_array; }
  static int num_jvm_flags()               { return _num_jvm_flags; }
  static int num_jvm_args()                { return _num_jvm_args; }
  // return the arguments passed to the Java application
  static const char* java_command()        { return _java_command; }
  static const char* java_command_crac()        { return _java_command_crac; }

  // print jvm_flags, jvm_args and java_command
  static void print_on(outputStream* st);
  static void print_summary_on(outputStream* st);

  // convenient methods to get and set jvm_flags_file
  static const char* get_jvm_flags_file()  { return _jvm_flags_file; }
  static void set_jvm_flags_file(const char *value) {
    if (_jvm_flags_file != nullptr) {
      os::free(_jvm_flags_file);
    }
    _jvm_flags_file = os::strdup_check_oom(value);
  }
  // convenient methods to obtain / print jvm_flags and jvm_args
  static const char* jvm_flags()           { return build_resource_string(_jvm_flags_array, _num_jvm_flags); }
  static const char* jvm_args()            { return build_resource_string(_jvm_args_array, _num_jvm_args); }
  static void print_jvm_flags_on(outputStream* st);
  static void print_jvm_args_on(outputStream* st);

  // -Dkey=value flags
  static SystemProperty*  system_properties()   { return _system_properties; }
  static const char*    get_property(const char* key);

  // -Djava.vendor.url.bug
  static const char* java_vendor_url_bug()  { return _java_vendor_url_bug; }

  // -Dsun.java.launcher
  static const char* sun_java_launcher()    { return _sun_java_launcher; }
  // Was VM created by a Java launcher?
  static bool created_by_java_launcher();
  // -XX:+ExecutingUnitTests
  static bool executing_unit_tests();

  // abort, exit, vfprintf hooks
  static abort_hook_t    abort_hook()       { return _abort_hook; }
  static exit_hook_t     exit_hook()        { return _exit_hook; }
  static vfprintf_hook_t vfprintf_hook()    { return _vfprintf_hook; }

  static void no_shared_spaces(const char* message);
  static size_t default_SharedBaseAddress() { return _default_SharedBaseAddress; }
  // Java launcher properties
  static void process_sun_java_launcher_properties(JavaVMInitArgs* args);

  // System properties
  static void init_system_properties();

  // Update/Initialize System properties after JDK version number is known
  static void init_version_specific_system_properties();

  // Update VM info property - called after argument parsing
  static void update_vm_info_property(const char* vm_info) {
    _vm_info->set_value(vm_info);
  }

  // Reset LogVMoutput to default values
  static void reset_for_crac_restore();
  // Property List manipulation
  static void PropertyList_add(SystemProperty *element);
  static void PropertyList_add(SystemProperty** plist, SystemProperty *element);
  static void PropertyList_add(SystemProperty** plist, const char* k, const char* v, bool writeable, bool internal);

  static void PropertyList_unique_add(SystemProperty** plist, const char* k, const char* v,
                                      PropertyAppendable append, PropertyWriteable writeable,
                                      PropertyInternal internal);
  static const char* PropertyList_get_value(SystemProperty* plist, const char* key);
  static const char* PropertyList_get_readable_value(SystemProperty* plist, const char* key);
  static int  PropertyList_count(SystemProperty* pl);
  static int  PropertyList_readable_count(SystemProperty* pl);

  static bool is_internal_module_property(const char* option);
  static bool is_incompatible_cds_internal_module_property(const char* property);

  // Miscellaneous System property value getter and setters.
  static void set_dll_dir(const char *value) { _sun_boot_library_path->set_value(value); }
  static void set_java_home(const char *value) { _java_home->set_value(value); }
  static void set_library_path(const char *value) { _java_library_path->set_value(value); }
  static void set_ext_dirs(char *value)     { _ext_dirs = os::strdup_check_oom(value); }

  // Set up the underlying pieces of the boot class path
  static void add_patch_mod_prefix(const char *module_name, const char *path);
  static void set_boot_class_path(const char *value, bool has_jimage) {
    // During start up, set by os::set_boot_path()
    assert(get_boot_class_path() == nullptr, "Boot class path previously set");
    _boot_class_path->set_value(value);
    _has_jimage = has_jimage;
  }
  static void append_sysclasspath(const char *value) {
    _boot_class_path->append_value(value);
    _jdk_boot_class_path_append->append_value(value);
  }

  static GrowableArray<ModulePatchPath*>* get_patch_mod_prefix() { return _patch_mod_prefix; }
  static char* get_boot_class_path() { return _boot_class_path->value(); }
  static bool has_jimage() { return _has_jimage; }

  static char* get_java_home()    { return _java_home->value(); }
  static char* get_dll_dir()      { return _sun_boot_library_path->value(); }
  static char* get_appclasspath() { return _java_class_path->value(); }
  static void  fix_appclasspath();

  // Operation modi
  static Mode mode()                { return _mode;           }
  static void set_mode_flags(Mode mode);
  static bool is_interpreter_only() { return mode() == _int;  }
  static bool is_compiler_only()    { return mode() == _comp; }


  // preview features
  static void set_enable_preview() { _enable_preview = true; }
  static bool enable_preview() { return _enable_preview; }

  // jdwp
  static bool has_jdwp_agent() { return _has_jdwp_agent; }

  // Utility: copies src into buf, replacing "%%" with "%" and "%p" with pid.
  static bool copy_expand_pid(const char* src, size_t srclen, char* buf, size_t buflen);

  static bool atojulong(const char *s, julong* result);

  static bool has_jfr_option() NOT_JFR_RETURN_(false);

  static bool is_restore_option_set(const JavaVMInitArgs* args);

  static bool parse_options_for_restore(const JavaVMInitArgs* args);

  DEBUG_ONLY(static bool verify_special_jvm_flags(bool check_globals);)
};

// Disable options not supported in this release, with a warning if they
// were explicitly requested on the command-line
#define UNSUPPORTED_OPTION(opt)                          \
do {                                                     \
  if (opt) {                                             \
    if (FLAG_IS_CMDLINE(opt)) {                          \
      warning("-XX:+" #opt " not supported in this VM"); \
    }                                                    \
    FLAG_SET_DEFAULT(opt, false);                        \
  }                                                      \
} while(0)

// similar to UNSUPPORTED_OPTION but sets flag to nullptr
#define UNSUPPORTED_OPTION_NULL(opt)                         \
do {                                                         \
  if (opt) {                                                 \
    if (FLAG_IS_CMDLINE(opt)) {                              \
      warning("-XX flag " #opt " not supported in this VM"); \
    }                                                        \
    FLAG_SET_DEFAULT(opt, nullptr);                          \
  }                                                          \
} while(0)

// Initialize options not supported in this release, with a warning
// if they were explicitly requested on the command-line
#define UNSUPPORTED_OPTION_INIT(opt, value)              \
do {                                                     \
  if (FLAG_IS_CMDLINE(opt)) {                            \
    warning("-XX flag " #opt " not supported in this VM"); \
  }                                                      \
  FLAG_SET_DEFAULT(opt, value);                          \
} while(0)

#endif // SHARE_RUNTIME_ARGUMENTS_HPP
