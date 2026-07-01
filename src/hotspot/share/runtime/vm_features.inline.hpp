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

#ifndef SHARE_RUNTIME_VM_FEATURES_INLINE_HPP
#define SHARE_RUNTIME_VM_FEATURES_INLINE_HPP

#include <utility>
#include "utilities/ostream.hpp"

class VM_Features : protected VM_Feature_Flag {
  friend class VMStructs;

 private:
  uint64_t _features_bitmap[(VM_Feature_Flag::MAX_CPU_FEATURES + BitsPerLong - 1) / BitsPerLong];

  STATIC_ASSERT(sizeof(_features_bitmap) * BitsPerByte >= VM_Feature_Flag::MAX_CPU_FEATURES);

  constexpr static int features_bitmap_element_shift_count() {
    return LogBitsPerLong;
  }

  constexpr static uint64_t features_bitmap_element_mask() {
    return (1ULL << features_bitmap_element_shift_count()) - 1;
  }

  static int index(Feature_Flag feature) {
    int idx = feature >> features_bitmap_element_shift_count();
    assert(idx < features_bitmap_element_count(), "Features array index out of bounds");
    return idx;
  }

  static uint64_t bit_mask(Feature_Flag feature) {
    return (1ULL << (feature & features_bitmap_element_mask()));
  }

  static uint64_t index_mask(int idx) {
    assert(idx < features_bitmap_element_count(), "Features array index out of bounds");
    if (idx + 1 < features_bitmap_element_count()) {
      return -1LL;
    }
    // It is equivalent to 'bit_mask(VM_Feature_Flag::MAX_CPU_FEATURES) - 1'.
    return ((bit_mask((Feature_Flag) ((int) VM_Feature_Flag::MAX_CPU_FEATURES - 1)) - 1) << 1) | 1;
  }

  // We do not use std::forward<> as we just call 'func'.
  template <typename T, typename F>
  static void apply_to_all_features(T&& t, F&& func) {
    for (int idx = 0; idx < t.features_bitmap_element_count(); ++idx) {
      func(t._features_bitmap[idx], idx);
    }
  }

  // We do not use std::forward<> as we just call 'func'.
  template <typename F>
  void apply_to_all_features(F&& func) {
    apply_to_all_features(*this, func);
  }

  // We do not use std::forward<> as we just call 'func'.
  template <typename F>
  void apply_to_all_features(F&& func) const {
    apply_to_all_features(*this, func);
  }

 public:
  VM_Features() {
    apply_to_all_features([](uint64_t &u, int idx) {
      u = 0;
    });
  }

  // Number of 8-byte elements in _bitmap.
  constexpr static int features_bitmap_element_count() {
    return sizeof(_features_bitmap) / sizeof(uint64_t);
  }

  void set_feature(Feature_Flag feature) {
    int idx = index(feature);
    _features_bitmap[idx] |= bit_mask(feature);
  }

  void clear_feature(Feature_Flag feature) {
    int idx = index(feature);
    _features_bitmap[idx] &= ~bit_mask(feature);
  }

  bool supports_feature(Feature_Flag feature) const {
    int idx = index(feature);
    return (_features_bitmap[idx] & bit_mask(feature)) != 0;
  }

  bool verify_aot_code_cache_features(VM_Features* features_to_test) {
    for (int i = 0; i < features_bitmap_element_count(); i++) {
      if (_features_bitmap[i] != features_to_test->_features_bitmap[i]) {
        return false;
      }
    }
    return true;
  }

  VM_Features aot_code_cache_features() {
    VM_Features copy = *this;
#ifdef AOT_CODE_CACHE_CLEAR
    copy.clear_feature(AOT_CODE_CACHE_CLEAR);
#endif
    return copy;
  }

  void set_all_features() {
    apply_to_all_features([](uint64_t &u, int idx) {
      u = index_mask(idx);
    });
  }

  void set_feature_idx(int idx, uint64_t val) {
    assert(idx < features_bitmap_element_count(), "Features array index out of bounds");
    _features_bitmap[idx] = val;
  }

  VM_Features operator ~() const {
    VM_Features retval = *this;
    apply_to_all_features(retval, [](uint64_t &u, int idx) {
      u ^= index_mask(idx);
    });
    return retval;
  }

  VM_Features operator &(const VM_Features &other) const {
    VM_Features retval = *this;
    apply_to_all_features(retval, [&other](uint64_t &u, int idx) {
      u &= other._features_bitmap[idx];
    });
    return retval;
  }

  VM_Features operator |(const VM_Features &other) const {
    VM_Features retval = *this;
    apply_to_all_features(retval, [&other](uint64_t &u, int idx) {
      u |= other._features_bitmap[idx];
    });
    return retval;
  }

  VM_Features &operator &=(const VM_Features &other) {
    *this = *this & other;
    return *this;
  }

  VM_Features &operator |=(const VM_Features &other) {
    *this = *this | other;
    return *this;
  }

  bool operator ==(const VM_Features &other) const {
    bool retval = true;
    apply_to_all_features([&other, &retval](uint64_t u, int idx) {
      if (u != other._features_bitmap[idx]) {
        retval = false;
      }
    });
    return retval;
  }

  bool operator !=(const VM_Features &other) const {
    return !(*this == other);
  }

  bool empty() const {
    VM_Features empty_features;
    return *this == empty_features;
  }

  void print_numbers(outputStream &os, bool hexonly = false) const {
    apply_to_all_features([&](uint64_t u, int idx) {
      os.print(hexonly ? UINT64_FORMAT_0 : UINT64_FORMAT_X, u);
      if (!hexonly && idx + 1 < features_bitmap_element_count()) {
        os.print_raw(",");
      }
    });
  }

  const char *print_numbers() const {
    char *buf = NEW_RESOURCE_ARRAY(char, MAX_CPU_FEATURES);
    stringStream ss(buf, MAX_CPU_FEATURES);
    print_numbers(ss);
    return buf;
  }

  static constexpr const char *make_features_names_name(size_t i) {
    switch (i) {
#define DECLARE_CPU_FEATURE_NAME(id, name, bit) case bit: return name ;
    CPU_FEATURE_FLAGS(DECLARE_CPU_FEATURE_NAME)
#undef DECLARE_CPU_FEATURE_NAME
    default:
      return nullptr;
    }
  }
  template <size_t... I>
  static constexpr std::array<const char *, sizeof...(I)> make_features_names(std::index_sequence<I...>) {
    return {{ make_features_names_name(I)... }};
  }
  static constexpr std::array<const char *, MAX_CPU_FEATURES> make_features_names() {
    return make_features_names(std::make_index_sequence<MAX_CPU_FEATURES>{});
  }
};

#endif // SHARE_RUNTIME_VM_FEATURES_INLINE_HPP
