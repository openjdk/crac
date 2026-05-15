// AI Tools Used:
// - Claude Sonnet 4.6, 2026-05-14
//   * Implemented PAC disable for CRaC restore in OpenJDK launcher, based on
//     the Azul Warp ZULU-90764 patch (src/common/pac.h).
/*
 * Copyright (c) 2026, Azul Systems, Inc. and/or its affiliates. All rights reserved.
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

#pragma once

#ifdef __aarch64__
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>

#ifndef PR_PAC_SET_ENABLED_KEYS
#define PR_PAC_SET_ENABLED_KEYS 60
#endif

#ifndef PR_PAC_APIAKEY
#define PR_PAC_APIAKEY  (1UL << 0)
#define PR_PAC_APIBKEY  (1UL << 1)
#define PR_PAC_APDAKEY  (1UL << 2)
#define PR_PAC_APDBKEY  (1UL << 3)
#endif

// After disabling PAC it is not possible to walk the stack that used PAC, e.g.
// frames of latest glibc created before calling this. So callers have to use
// exit instead of returning.
static inline __attribute__((always_inline)) void disable_pac(void) {
    // Note: PR_PAC_APGAKEY cannot be disabled through this syscall
    if (prctl(PR_PAC_SET_ENABLED_KEYS, PR_PAC_APIAKEY | PR_PAC_APIBKEY | PR_PAC_APDAKEY | PR_PAC_APDBKEY, 0, 0, 0)) {
        if (errno != EINVAL) { // Systems without PAC support return EINVAL
            perror("prctl PR_PAC_SET_ENABLED_KEYS");
            exit(1);
        }
    }
}

#else // !__aarch64__

static inline void disable_pac(void) {}

#endif // !__aarch64__
