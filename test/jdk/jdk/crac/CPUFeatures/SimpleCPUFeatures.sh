#! /bin/bash
# Copyright (c) 2023, 2025, Azul Systems, Inc. All rights reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 only, as
# published by the Free Software Foundation.
#
# This code is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# version 2 for more details (a copy is included in the LICENSE file that
# accompanied this code).
#
# You should have received a copy of the GNU General Public License version
# 2 along with this work; if not, write to the Free Software Foundation,
# Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
# or visit www.oracle.com if you need additional information or have any
# questions.

# @test
# @requires os.family == "linux"
# @requires os.arch=="amd64" | os.arch=="x86_64"
# @run shell SimpleCPUFeatures.sh
# @summary On old glibc (without INCLUDE_CPU_FEATURE_ACTIVE) one could get an internal error.
# Error occurred during initialization of VM
# internal error: GLIBC_TUNABLES=:glibc.cpu.hwcaps=,-AVX,-FMA,-AVX2,-BMI1,-BMI2,-ERMS,-LZCNT,-SSSE3,-POPCNT,-SSE4_1,-SSE4_2,-AVX512F,-AVX512CD,-AVX512BW,-AVX512DQ,-AVX512VL,-IBT,-MOVBE,-SHSTK,-XSAVE,-OSXSAVE,-HTT failed and HOTSPOT_GLIBC_TUNABLES_REEXEC is set

set -ex
export JAVA_HOME=$TESTJAVA

unset GLIBC_TUNABLES
$JAVA_HOME/bin/java -XX:CPUFeatures=generic -XX:+ShowCPUFeatures -version 2>&1 | tee /proc/self/fd/2 | grep -q 'openjdk version'

# The test from summary:
export GLIBC_TUNABLES=glibc.pthread.rseq=0
$JAVA_HOME/bin/java -XX:CPUFeatures=generic -XX:+ShowCPUFeatures -version 2>&1 | tee /proc/self/fd/2 | grep -q 'openjdk version'

for GLIBC_TUNABLES in \
                       glibc.cpu.hwcaps=-AVX                      \
  glibc.pthread.rseq=0:glibc.cpu.hwcaps=-AVX                      \
                       glibc.cpu.hwcaps=-AVX:glibc.pthread.rseq=0 \
  ; do
  $JAVA_HOME/bin/java -XX:CPUFeatures=generic -XX:+ShowCPUFeatures -version 2>&1 | tee /proc/self/fd/2 | grep -q 'openjdk version'
done

exit 0
