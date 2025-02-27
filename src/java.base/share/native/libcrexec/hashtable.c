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
#include "hashtable.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static unsigned int string_hash(const char *str) {
  unsigned int hash = 0;
  for (; *str; str++) {
    hash = 31 * hash + *str;
  }
  return hash;
}

hashtable_t *hashtable_create(const char **keys, size_t length) {
  hashtable_t * const ht = (hashtable_t *) malloc(sizeof(hashtable_t));
  if (ht == NULL) {
    return NULL;
  }

  ht->length = length;
  ht->keys = (const char **) malloc(length * sizeof(const char **));
  if (ht->keys == NULL) {
    hashtable_destroy(ht);
    return NULL;
  }
  ht->values = (void **) malloc(length * sizeof(void **));
  if (ht->values == NULL) {
    hashtable_destroy(ht);
    return NULL;
  }

  memset((char **) ht->keys, 0, length * sizeof(const char **)); // Cast silences MSVC warning C4090
  memset(ht->values, 0, length * sizeof(void **));

  for (size_t i = 0; i < length; i++) {
    const char *key = keys[i];
    assert(key != NULL);
    const unsigned int hash = string_hash(key) % length;
    bool place_found = false;
    for (size_t j = hash; !place_found && j < length; j++) {
      if (ht->keys[j] == NULL) {
        ht->keys[j] = key;
        place_found = true;
      }
    }
    for (size_t j = 0; !place_found && j < hash; j++) {
      if (ht->keys[j] == NULL) {
        ht->keys[j] = key;
        place_found = true;
      }
    }
    assert(place_found); // There should be enough space for all keys to fit
  }

  return ht;
}

void hashtable_destroy(hashtable_t *ht) {
  if (ht == NULL) {
    return;
  }
  free((char **) ht->keys); // Cast silences MSVC warning C4090
  free(ht->values);
  free(ht);
}

static void **find_value(hashtable_t *ht, const char *key) {
  const unsigned int hash = string_hash(key) % ht->length;
  for (size_t i = hash; i < ht->length; i++) {
    if (strcmp(key, ht->keys[i]) == 0) {
      return &ht->values[i];
    }
  }
  for (size_t i = 0; i < hash; i++) {
    if (strcmp(key, ht->keys[i]) == 0) {
      return &ht->values[i];
    }
  }
  return NULL;
}

bool hashtable_contains(hashtable_t *ht, const char *key) {
  return find_value(ht, key) != NULL;
}

void *hashtable_get(hashtable_t *ht, const char *key) {
  void ** const value_ptr = find_value(ht, key);
  return value_ptr != NULL ? *value_ptr : NULL;
}

bool hashtable_put(hashtable_t *ht, const char *key, void *value) {
  void ** const value_ptr = find_value(ht, key);
  if (value_ptr == NULL) {
    return false;
  }
  *value_ptr = value;
  return true;
}
