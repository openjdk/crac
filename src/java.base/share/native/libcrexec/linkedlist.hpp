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
#ifndef LINKEDLIST_HPP
#define LINKEDLIST_HPP

#include <new>
#include <utility>

template<class T> class LinkedList {
private:
  struct Node {
    T _item;
    Node *_next;
  };

  Node *_head;
  Node *_tail;
  size_t _size;
public:
  LinkedList(): _head(nullptr), _tail(nullptr), _size(0) {}
  ~LinkedList() {
    Node *node = _head;
    while (node) {
      Node *next = node->_next;
      delete node;
      node = next;
    }
  }

  T& add(T&& item) {
    Node *node = new(std::nothrow) Node();
    node->_item = std::forward<T>(item);
    node->_next = nullptr;
    if (_tail) {
      _tail->_next = node;
    } else {
      _tail = node;
    }
    if (!_head) {
      _head = node;
    }
    ++_size;
    return node->_item;
  }

  template<typename Func> void foreach(Func f) const {
    Node *node = _head;
    while (node) {
      f(node->_item);
      node = node->_next;
    }
  }

  size_t size() {
    return _size;
  }
};

#endif // LINKEDLIST_HPP
