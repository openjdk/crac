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
    Node* _next;

    template<typename TT>
    Node(TT&& v): _item(std::forward<TT>(v)) {}
  };

  Node* _head = nullptr;
  Node* _tail = nullptr;
  size_t _size = 0;

  inline bool add_node(Node *node) {
    if (node == nullptr) {
      return false;
    }
    node->_next = nullptr;
    if (_tail) {
      _tail->_next = node;
    }
    _tail = node;
    if (!_head) {
      _head = node;
    }
    ++_size;
    return true;
  }

public:
  ~LinkedList() {
    clear();
  }

  void clear() {
    Node *node = _head;
    while (node) {
      Node *next = node->_next;
      delete node;
      node = next;
    }
    _head = nullptr;
    _tail = nullptr;
    _size = 0;
  }

  bool add(T&& item) {
    return add_node(new(std::nothrow) Node(std::move(item)));
  }

  bool add(const T& item) {
    return add_node(new(std::nothrow) Node(item));
  }

  template<typename Func> void foreach(Func f) const {
    Node* node = _head;
    while (node) {
      f(node->_item);
      node = node->_next;
    }
  }

  size_t size() const {
    return _size;
  }
};

#endif // LINKEDLIST_HPP
