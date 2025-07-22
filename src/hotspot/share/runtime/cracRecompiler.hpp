/*
 * Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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

#ifndef SHARE_RUNTIME_CRACRECOMPILER_HPP
#define SHARE_RUNTIME_CRACRECOMPILER_HPP

#include "memory/allStatic.hpp"
#include "oops/metadata.hpp"
#include "oops/method.hpp"

// During checkpoint-restore there is a high chance that application state will
// temporarily change. This may trigger deoptimizations and make methods
// decompile (make nmethods non-entrant). After restore the application is
// likely to quickly return to its previous stable state but it will take some
// time to compile the decompiled methods back, probably to the same code as
// before.
//
// To speed up such after-restore warmup this class records decompilations
// occuring during checkpoint-restore (and shortly after) and requests their
// compilation immediately afterards.
class CRaCRecompiler : public AllStatic {
public:
  static void start_recording_decompilations();
  static bool record_decompilation(Method *method, int bci, int comp_level);
  static void finish_recording_decompilations_and_recompile();

  static void metadata_do(void f(Metadata *));
};

#endif // SHARE_RUNTIME_CRACRECOMPILER_HPP
