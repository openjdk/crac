/*
 * Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
 */

#ifndef SHARE_RUNTIME_ABSTRACT_VM_VERSION_INLINE_HPP
#define SHARE_RUNTIME_ABSTRACT_VM_VERSION_INLINE_HPP

#include "memory/resourceArea.hpp"

VM_Features VM_Version::CPUFeatures_parse(const char *str) {
#ifndef LINUX
  _ignore_glibc_not_using = true;
#endif // !LINUX
  if (str == nullptr || strcmp(str, "native") == 0) {
    return _features;
  }
  if (strcmp(str, "ignore") == 0) {
    _ignore_glibc_not_using = true;
    return _features;
  }
  if (strcmp(str, "generic") == 0) {
    return CPUFeatures_generic();
  }
#ifndef LINUX
  vm_exit_during_initialization("This OS does not support any arch-specific -XX:CPUFeatures options");
  return {};
#else // LINUX
  int count = VM_Features::features_bitmap_element_count();
  VM_Features retval;
  const char *str_orig = str;
  for (int idx = 0;; ++idx) {
    static_assert(sizeof(uint64_t) == sizeof(unsigned long long), "unexpected arch");
    char *endptr;
    errno = 0;
    uint64_t u64 = strtoull(str, &endptr, 0);
    if (errno != 0) {
      break;
    }
    bool last = idx + 1 == count;
    if (*endptr != (last ? 0 : ',')) {
      break;
    }
    retval.set_feature_idx(idx, u64);
    if (last) {
      return retval;
    }
    str = endptr + 1;
  }
  char buf[MAX_CPU_FEATURES];
  char *s = buf;
  for (int idx = 0; idx < count; ++idx) {
    s = stpcpy(s, ",0xNUM");
  }
  vm_exit_during_initialization(err_msg("VM option 'CPUFeatures=%s' must be of the form: %s", str_orig, buf + 1));
  return {};
#endif // LINUX
}

bool VM_Version::_ignore_glibc_not_using = false;
#ifdef LINUX
bool VM_Version::glibc_env_set(char *disable_str) {
#define TUNABLES_NAME "GLIBC_TUNABLES"
#define REEXEC_NAME "HOTSPOT_GLIBC_TUNABLES_REEXEC"
  char *env_val = disable_str;
  const char *env = getenv(TUNABLES_NAME);
  bool from_reexec = getenv(REEXEC_NAME) != nullptr;
  if (env && (strcmp(env, env_val) == 0 || (!INCLUDE_CPU_FEATURE_ACTIVE && from_reexec))) {
    if (!INCLUDE_CPU_FEATURE_ACTIVE) {
      if (ShowCPUFeatures) {
        tty->print_cr("Environment variable already set, glibc CPU_FEATURE_ACTIVE is unavailable - re-exec suppressed: " TUNABLES_NAME "=%s", env);
      }
      return true;
    }
  }
  {
    ResourceMark rm;
    size_t env_buf_size = strlen(disable_str) + (!env ? 0 : strlen(env) + 100);
    char *env_buf = NEW_RESOURCE_ARRAY(char, env_buf_size);
    if (env) {
      if (ShowCPUFeatures) {
        tty->print_cr("Original environment variable: " TUNABLES_NAME "=%s", env);
      }
      const char *hwcaps = strstr(env, glibc_prefix + 1 /* skip ':' */);
      if (!hwcaps) {
        strcpy(env_buf, env);
        strcat(env_buf, disable_str);
      } else {
        const char *colon = strchr(hwcaps, ':');
        if (!colon) {
          strcpy(env_buf, env);
          strcat(env_buf, disable_str + glibc_prefix_len);
        } else {
          int err = jio_snprintf(env_buf, env_buf_size, "%.*s%s%s", (int)(colon - env), env, disable_str + glibc_prefix_len, colon);
          assert(err >= 0 && (unsigned) err < env_buf_size, "internal error: " TUNABLES_NAME " buffer overflow");
        }
      }
      env_val = env_buf;
    }
    if (ShowCPUFeatures) {
      tty->print_cr("Re-exec of java with new environment variable: " TUNABLES_NAME "=%s", env_val);
    }
    if (setenv(TUNABLES_NAME, env_val, 1)) {
      vm_exit_during_initialization(err_msg("setenv " TUNABLES_NAME " error: %m"));
    }
  }

  if (from_reexec) {
    vm_exit_during_initialization(err_msg("internal error: " TUNABLES_NAME "=%s failed and " REEXEC_NAME " is set", disable_str));
  }
  if (setenv(REEXEC_NAME, "1", 1)) {
    vm_exit_during_initialization(err_msg("setenv " REEXEC_NAME " error: %m"));
  }
  return false;
}

void VM_Version::glibc_reexec() {
  char *buf = nullptr;
  size_t buf_allocated = 0;
  size_t buf_used = 0;
#define CMDLINE "/proc/self/cmdline"
  int fd = open(CMDLINE, O_RDONLY);
  if (fd == -1)
    vm_exit_during_initialization(err_msg("Cannot open " CMDLINE ": %m"));
  ssize_t got;
  do {
    if (buf_used == buf_allocated) {
      buf_allocated = MAX2(size_t(4096), 2 * buf_allocated);
      buf = (char *)os::realloc(buf, buf_allocated, mtOther);
      if (buf == nullptr)
        vm_exit_during_initialization(err_msg(CMDLINE " reading failed allocating %zu bytes", buf_allocated));
    }
    got = read(fd, buf + buf_used, buf_allocated - buf_used);
    if (got == -1)
      vm_exit_during_initialization(err_msg("Cannot read " CMDLINE ": %m"));
    buf_used += got;
  } while (got);
  if (close(fd))
    vm_exit_during_initialization(err_msg("Cannot close " CMDLINE ": %m"));
  char **argv = nullptr;
  size_t argv_allocated = 0;
  size_t argv_used = 0;
  char *s = buf;
  while (s <= buf + buf_used) {
    if (argv_used == argv_allocated) {
      argv_allocated = MAX2(size_t(256), 2 * argv_allocated);
      argv = (char **)os::realloc(argv, argv_allocated * sizeof(*argv), mtOther);
      if (argv == nullptr)
        vm_exit_during_initialization(err_msg(CMDLINE " reading failed allocating %zu pointers", argv_allocated));
    }
    if (s == buf + buf_used) {
      break;
    }
    argv[argv_used++] = s;
    s += strnlen(s, buf + buf_used - s);
    if (s == buf + buf_used)
      vm_exit_during_initialization("Missing end of string zero while parsing " CMDLINE);
    ++s;
  }
  argv[argv_used] = nullptr;
#undef CMDLINE

#define EXEC "/proc/self/exe"
  execv(EXEC, argv);
  vm_exit_during_initialization(err_msg("Cannot re-execute " EXEC ": %m"));
#undef EXEC
  ShouldNotReachHere(); // vm_exit_during_initialization should be [[noreturn]]
}

// Returns whether we should have got set a GLIBC_TUNABLES environment variables but did not get any.
bool VM_Version::glibc_not_using() {
  if (_ignore_glibc_not_using)
    return true;

  VM_Features features_expected;
  features_expected.set_all_features();
  if (!INCLUDE_CPU_FEATURE_ACTIVE) {
    features_expected = _features;
  }
  VM_Features shouldnotuse = features_expected & ~_features;

#ifndef ASSERT
  if (shouldnotuse.empty())
    return true;
#endif

  glibc_patch(shouldnotuse);

  static const size_t tunables_size_max = strlen("AVX_Fast_Unaligned_Load");
  char disable_str[MAX_CPU_FEATURES * (1/*','*/ + 1/*'-'*/ + tunables_size_max) + 1/*'\0'*/];
  strcpy(disable_str, glibc_prefix);
  char *disable_end = disable_str + glibc_prefix_len;
  auto disable = [&](const char *tunables) {
    size_t remains = disable_str + sizeof(disable_str) - disable_end;
    guarantee(2 + strlen(tunables) < remains, "internal error: disable_str overflow");
    *disable_end++ = ',';
    *disable_end++ = '-';
    disable_end = stpcpy(disable_end, tunables);
  };

#ifdef ASSERT
  VM_Features handled;
#endif
  auto shouldnotuse_handled = [&](Feature_Flag feature, const char *tunables) {
    assert(strlen(tunables) <= tunables_size_max, "Too long string %s", tunables);
    assert(!handled.supports_feature(feature), "already used %s", tunables);
    DEBUG_ONLY(handled.set_feature(feature));
  };
#define EXCESSIVE_HANDLED(tunables) shouldnotuse_handled(PASTE_TOKENS(CPU_, tunables), STR(tunables))

#if INCLUDE_CPU_FEATURE_ACTIVE
# define FEATURE_ACTIVE(tunables) CPU_FEATURE_ACTIVE(tunables)
#else
# define FEATURE_ACTIVE(tunables) true
#endif

  auto shouldnotuse_set = [&](Feature_Flag feature, const char *tunables, bool feature_active) {
    shouldnotuse_handled(feature, tunables);
    if (shouldnotuse.supports_feature(feature) && feature_active) {
      disable(tunables);
    }
  };
#define EXCESSIVE2(tunables, feature_active) do {                                                                        \
    static_assert(strlen(STR(tunables)) <= tunables_size_max, "\"" STR(tunables) "\" is longer than tunables_size_max"); \
    shouldnotuse_set(PASTE_TOKENS(CPU_, tunables), STR(tunables), feature_active);                                       \
  } while (0)
#define EXCESSIVE(tunables) EXCESSIVE2(tunables, FEATURE_ACTIVE(tunables))
  EXCESSIVE_LIST
#undef EXCESSIVE

#ifdef ASSERT
  // These cannot be disabled by GLIBC_TUNABLES interface.
#define GLIBC_UNSUPPORTED(hotspot) EXCESSIVE_HANDLED(hotspot)
  GLIBC_UNSUPPORTED_LIST
#undef GLIBC_UNSUPPORTED

  VM_Features all_features;
  all_features.set_all_features();
  if (handled != all_features) {
    stringStream ss;
    ss.print_raw("internal error: Unsupported disabling of some CPU_* ");
    handled.print_numbers(ss);
    ss.print_raw(" != full ");
    all_features.print_numbers(ss);
    vm_exit_during_initialization(ss.base());
  }
#endif // ASSERT

  *disable_end = 0;
  if (disable_end == disable_str + glibc_prefix_len)
    return true;
  if (glibc_env_set(disable_str))
    return true;
  return false;
}
#undef REEXEC_NAME
#endif // LINUX

void VM_Version::cpu_features_init() {
  assert(!CPUFeatures == FLAG_IS_DEFAULT(CPUFeatures), "CPUFeatures parsing");

  VM_Features CPUFeatures_parsed = CPUFeatures_parse(CPUFeatures);
  VM_Features features_missing = CPUFeatures_parsed & ~_features;

  features_missing = features_missing.aot_code_cache_features();

  if (!features_missing.empty()) {
    stringStream ss;
    ss.print_raw("Specified -XX:CPUFeatures=");
    CPUFeatures_parsed.print_numbers(ss);
    ss.print_raw("; this machine's CPU features are ");
    _features.print_numbers(ss);
    ss.print_raw("; missing features of this CPU are ");
    features_missing.print_numbers(ss);
    ss.print_raw(" = ");
    insert_features_names(features_missing, ss);
    ss.cr();
    ss.print_raw_cr("If you are sure it will not crash you can override this check by -XX:+UnlockExperimentalVMOptions -XX:CheckCPUFeatures=skip .");
    vm_exit_during_initialization(ss.base());
  }

  _features = CPUFeatures_parsed;

  if (ShowCPUFeatures && !CRaCRestoreFrom) {
    print_using_features_cr();
  }

#ifdef LINUX
  if (!glibc_not_using())
    glibc_reexec();
#endif
}

void VM_Version::insert_features_names(VM_Version::VM_Features features, stringStream& ss) {
  int i = 0;
  ss.join([&]() {
    while (i < MAX_CPU_FEATURES) {
      if (features.supports_feature((VM_Version::Feature_Flag)i)) {
        return _features_names[i++];
      }
      i += 1;
    }
    return (const char*)nullptr;
  }, ", ");
}

#endif // SHARE_RUNTIME_ABSTRACT_VM_VERSION_INLINE_HPP
