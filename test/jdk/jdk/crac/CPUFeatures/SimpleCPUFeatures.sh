#! /bin/bash
# @test
# @requires os.family == "linux"
# @requires os.arch=="amd64" | os.arch=="x86_64"
# @run shell SimpleCPUFeatures.sh
# @summary On old glibc (without INCLUDE_CPU_FEATURE_ACTIVE) one could get an internal error.
# Error occurred during initialization of VM
# internal error: GLIBC_TUNABLES=:glibc.cpu.hwcaps=,-AVX,-FMA,-AVX2,-BMI1,-BMI2,-ERMS,-LZCNT,-SSSE3,-POPCNT,-SSE4_1,-SSE4_2,-AVX512F,-AVX512CD,-AVX512BW,-AVX512DQ,-AVX512VL,-IBT,-MOVBE,-SHSTK,-XSAVE,-OSXSAVE,-HTT failed and HOTSPOT_GLIBC_TUNABLES_REEXEC is set

set -ex

export JAVA_HOME=$TESTJAVA
export GLIBC_TUNABLES=glibc.pthread.rseq=0
$JAVA_HOME/bin/java -XX:CPUFeatures=generic -XX:+ShowCPUFeatures -version 2>&1 | tee /proc/self/fd/2 | grep -q 'openjdk version'

exit 0
