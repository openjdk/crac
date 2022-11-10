#!/bin/sh

# Copyright 2019-2021 Azul Systems, Inc.  All Rights Reserved.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License version 2 only, as published by
# the Free Software Foundation.
#
# This code is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE.  See the GNU General Public License version 2 for more
# details (a copy is included in the LICENSE file that accompanied this code).
#
# You should have received a copy of the GNU General Public License version 2
# along with this work; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Please contact Azul Systems, 385 Moffett Park Drive, Suite 115, Sunnyvale,
# CA 94089 USA or visit www.azul.com if you need additional information or
# have any questions.

##
## @test RestoreEnvironmentTest.sh
## @summary the test checks that actual environment variables are propagated into a restored process.
## @compile RestoreEnvironmentTest.java
## @run shell/timeout=120 RestoreEnvironmentTest.sh
##

set -x

CHECKPOINT_DIR=cr_dir
BEFORE=BeforeCheckpoint
AFTER=AfterRestore
NEWVAL=NewValue

echo CHECKPOINT_DIR=$CHECKPOINT_DIR
rm -rf CHECKPOINT_DIR

echo === Checkpointing...
export RESTORE_ENVIRONMENT_TEST_VAR0=$BEFORE
export RESTORE_ENVIRONMENT_TEST_VAR1=$BEFORE
${TESTJAVA}/bin/java -cp ${TESTCLASSPATH} -XX:CRaCCheckpointTo=$CHECKPOINT_DIR RestoreEnvironmentTest

echo === Restoring...
export RESTORE_ENVIRONMENT_TEST_VAR1=$AFTER
RESULT=`RESTORE_ENVIRONMENT_TEST_VAR2=$NEWVAL ${TESTJAVA}/bin/java -cp ${TESTCLASSPATH} -XX:CRaCRestoreFrom=$CHECKPOINT_DIR`

EXPECTED="(after restore) RESTORE_ENVIRONMENT_TEST_VAR0=$BEFORE;RESTORE_ENVIRONMENT_TEST_VAR1=$AFTER;RESTORE_ENVIRONMENT_TEST_VAR2=$NEWVAL;"
echo RESULT=$RESULT
echo EXPECTED=$EXPECTED
if [ "$EXPECTED" != "$RESULT" ]; then
    echo FAILED
    exit 1
fi
echo PASSED
