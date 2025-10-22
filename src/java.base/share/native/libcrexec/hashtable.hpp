/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
#ifndef HASHTABLE_HPP
#define HASHTABLE_HPP

#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

template<class T>
class Hashtable {
public:
  inline Hashtable(const char * const keys[], size_t num_keys): Hashtable(keys, num_keys, num_keys) {}
  Hashtable(const char * const keys[], size_t num_keys, size_t capacity);
  ~Hashtable();

  // Use this to check whether the constructor succeeded.
  bool is_initialized() const { return _keys != nullptr; }

  bool contains(const char* key) const;
  T* get(const char* key) const;
  template<typename TT> bool put(const char* key, TT&& value);

  template<typename F> void foreach(F func) const;

private:
  size_t _capacity;
  const char** _keys;
  T* _values;

  static unsigned int string_hash(const char* str);
};

template<class T>
unsigned int Hashtable<T>::string_hash(const char* str) {
  assert(str != nullptr);
  unsigned int hash = 0;
  for (; *str != '\0'; str++) {
    hash = 31 * hash + *str;
  }
  return hash;
}

template<class T>
Hashtable<T>::Hashtable(const char * const keys[], size_t num_keys, size_t capacity) :
    _capacity(capacity),
    _keys(new(std::nothrow) const char*[capacity]()),
    _values(new(std::nothrow) T[capacity]()) {
  if (_keys == nullptr || _values == nullptr) {
    delete[] _keys;
    delete[] _values;
    _keys = nullptr;
    _values = nullptr;
    assert(!is_initialized());
    return;
  }

  for (size_t i = 0; i < num_keys; i++) {
    const char* key = keys[i];
    assert(key != nullptr);
    const unsigned int hash = string_hash(key) % capacity;
    bool place_found = false;
    for (size_t j = hash; !place_found && j < capacity; j++) {
      if (_keys[j] == nullptr) {
        _keys[j] = key;
        place_found = true;
      } else if (!strcmp(key, _keys[j])) {
        place_found = true;
      }
    }
    for (size_t j = 0; !place_found && j < hash; j++) {
      if (_keys[j] == nullptr) {
        _keys[j] = key;
        place_found = true;
      } else if (!strcmp(key, _keys[j])) {
        place_found = true;
      }
    }
    assert(place_found); // There should be enough space for all keys to fit
  }

  assert(is_initialized());
}

template<class T>
Hashtable<T>::~Hashtable() {
  delete[] _keys;
  delete[] _values;
}

template<class T>
bool Hashtable<T>::contains(const char* key) const {
  return get(key) != nullptr;
}

template<class T>
T* Hashtable<T>::get(const char* key) const {
  if (_capacity == 0) {
    return nullptr;
  }
  assert(key != nullptr);
  const unsigned int hash = string_hash(key) % _capacity;
  for (size_t i = hash; i < _capacity; i++) {
    if (strcmp(key, _keys[i]) == 0) {
      return &_values[i];
    }
  }
  for (size_t i = 0; i < hash; i++) {
    if (strcmp(key, _keys[i]) == 0) {
      return &_values[i];
    }
  }
  return nullptr;
}

template<class T> template<typename TT>
bool Hashtable<T>::put(const char* key, TT&& value) {
  T* const value_ptr = get(key);
  if (value_ptr == nullptr) {
    return false;
  }
  *value_ptr = std::forward<TT>(value);
  return true;
}

template<class T> template<typename F>
void Hashtable<T>::foreach(F func) const {
  for (size_t i = 0; i < _capacity; ++i) {
    const char *key = _keys[i];
    if (key != nullptr) {
      func(key, _values[i]);
    }
  }
}

#endif // HASHTABLE_HPP
