#! /bin/dash
# Copyright (c) 2026, Azul Systems, Inc. All rights reserved.
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
# @compile CPUFeaturesAWS.java
# @comment It will be always skipped unless you use jtreg option "-manual" which conflicts with JDK's default option "-automatic".
# @comment Therefore either change it in make/RunTests.gmk or run jtreg by hand.
# @run shell/manual CPUFeaturesAWS.sh

# Using '#! /bin/dash' as jtreg does not follow '#!' and Ubuntu default shell is '/bin/dash'.

set -e
# /bin/dash does not support pipefail
(set -o pipefail) 2>/dev/null && set -o pipefail
exec >&2

javasetup() {
  export JAVA_HOME=$PWD
  ulimit -c unlimited
  # Upstream CRIU: criuengine: Cannot find CRIU executable: criu_location option not set, not found on PATH
  export PATH="$PWD/lib:$PATH"
}
internal_checkpoint() {
  javasetup
  rm -rf cr || exit 1
  # Prevent on CRIU: Error (criu/pie/restorer.c:2057): Unable to create a thread: -17
  (set +x
    for i in $(seq 1 1000);do
      /bin/true
    done
  )
  bin/java -XX:CRaCCheckpointTo=cr -XX:+ShowCPUFeatures $* CPUFeaturesAWS &
  p=$!
  sleep 2
  set +e
  bin/jcmd CPUFeaturesAWS JDK.checkpoint
  wait $p
}
internal_restore() {
  javasetup
  bin/java -XX:CRaCRestoreFrom=cr $* &
  p=$!
  (sleep 2;kill $p) &
  set +e
  wait $p
  echo RC=$?
}
internal_reexec() {
  javasetup
  set +e
  ! bin/java $enginecmdline -XX:+ShowCPUFeatures --version|grep 'Re-exec of java with new environment variable'
  exit $?
}
if [ "${1#internal_}" != "$1" ];then
  set -x
  "$@"
  exit 0
fi

helpAWS_KEY_FILE="AWS_KEY_FILE=~/.ssh/aws.pem"
helpAWS_KEY_NAME="AWS_KEY_NAME=$USER for the aws --key-name parameter"
helpAWS_TAG_USER="AWS_TAG_USER=fsurname of a valid Azul username"
if [ "$1" = -h -o "$1" = --help ];then
  echo >&2 "$helpAWS_KEY_FILE $helpAWS_KEY_NAME $helpAWS_TAG_USER TESTJAVA=/java/jdk/root TESTCLASSES=/path/to/CPUFeaturesAWS.class $0 [-h|--help] [only {lineno}...]"
  exit 1
fi

set -x
profile=cpufeatures
test -r "$AWS_KEY_FILE" || (echo >&2 "required: $helpAWS_KEY_FILE";exit 1)
test -n "$AWS_KEY_NAME" || (echo >&2 "required: $helpAWS_KEY_NAME";exit 1)
JAVA_HOME=$TESTJAVA
javafilestest="bin/java"
javafilescriu="lib/criu"
javafiles="bin/jcmd lib/jvm.cfg lib/libcriuengine.so lib/libjava.so lib/libjimage.so lib/libjli.so lib/libnet.so lib/libnio.so lib/libattach.so lib/libzip.so lib/modules lib/tzdb.dat lib/server/classes.jsa lib/server/libjvm.so lib/criuhelper lib/libcrcommon.so $javafilescriu conf/security/java.security $javafilestest"
# It exists only if C2 has been configured.
javafilesoptional="lib/libjsvml.so"
classfiles="CPUFeaturesAWS.class"
# Comment out to reuse plus not to shutdown the allocated machines; see also $awstimeout
do_shutdown=true

awstaguser=${AWS_TAG_USER:-$USER}
curl --fail --head http://release.azulsystems.com/home/$awstaguser/ || (echo >&2 "required: $helpAWS_TAG_USER";exit 1)

awstimeout=30 # minutes, total script run was 9 minutes during a test
awstagname=$awstaguser-cpufeatures
# debianami is set later
debianuser=admin

test -x $0

only=""
if [ -z "$*" ];then
  true
elif [ "$1" = only ];then
  shift
  only="$*"
else
  echo >&2 "invalid parameters: $*"
  exit 1
fi

if ! aws sts get-caller-identity --profile $profile;then
  cat >&2 <<EOH

Authentication required:
$ aws configure sso
...
$ aws sso login --profile $profile

EOH
  exit 1
fi

shutdown() {
  if ! ${do_shutdown:-false};then return;fi
  do_shutdown=false
  aws ec2 terminate-instances --profile $profile --region us-west-2 --instance-ids $(aws ec2 describe-instances --profile $profile --region us-west-2 --filters "Name=tag:Name,Values=$awstagname-*" --query 'Reservations[*].Instances[*].InstanceId' --output text)
}
trap shutdown EXIT
fatal() {
  shutdown
  echo >&2 "$*"
  exit 1
}

for file in $javafiles;do
  test -e $JAVA_HOME/$file
done
for file in $javafilesoptional;do
  test -e $JAVA_HOME/$file && javafiles="$javafiles $file"
done
for file in $classfiles;do
  test -e $TESTCLASSES/$file
done

tmpdir=/tmp/CPUFeaturesAWS.$$
trap "rm -rf $tmpdir" EXIT
rm -rf $tmpdir
mkdir $tmpdir
ssh-keygen -f $tmpdir/key -N ''

getipaddr() {
  local ipaddr="$(aws ec2 describe-instances --profile $profile --region us-west-2 --filters "Name=tag:Name,Values=$awstagname-$kind" "Name=instance-state-name,Values=running" --query 'Reservations[*].Instances[*].PrivateIpAddress' --output text|tr -cd 0-9.)"
  # $kind contains '.' which is not permitted in a variable name.
  local kind2=$(echo $kind|tr -cd a-zA-Z0-9)
  eval ipaddr_$kind2=$ipaddr
}

ipaddr() {
  local kind2=$(echo $1|tr -cd a-zA-Z0-9)
  eval "echo \$ipaddr_$kind2"
}

runssh() {
  (set -x;ssh -o 'UserKnownHostsFile /dev/null' -o 'StrictHostKeyChecking no' -i $AWS_KEY_FILE $debianuser@$(ipaddr $kind) "$*")
}

setup() {
  if [ -n "$(ipaddr $kind)" ];then
    return
  fi
  getipaddr
  if [ -z "$(ipaddr $kind)" ];then
    aws ec2 run-instances --no-cli-pager --profile $profile --region us-west-2 --image-id $debianami --instance-type $kind --key-name $AWS_KEY_NAME --subnet-id subnet-0a6fd4c98705a4c63 --security-group-ids sg-0081bc08de42b1086 --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$awstagname-$kind}]" --instance-initiated-shutdown-behavior terminate --user-data $'#!/bin/bash\nshutdown -P +'$awstimeout
    for i in `seq 1 60`;do
      getipaddr
      if [ -n "$(ipaddr $kind)" ];then
        break
      fi
      sleep 1
    done
    if [ -z "$(ipaddr $kind)" ];then
      fatal "Timeout detecing an AWS instance IP address."
    fi
    local ok=false
    for i in `seq 1 60`;do
      runssh true && ok=true && break
      sleep 1
    done
    if ! $ok;then
      fatal "Timeout connecting to the AWS instance IP $(ipaddr $kind) by SSH."
    fi
  fi

  # key+key.pub will be different when reusing the machine, install our current one.
  runssh <$tmpdir/key.pub "cat >>.ssh/authorized_keys;echo kernel.core_pattern=core|sudo tee -a /etc/sysctl.d/CPUFeatures.conf;sudo sysctl --system"
  (cd $tmpdir;tar cf - key)|runssh "set -ex;tar xf -"
  runssh "set -ex;cat >CPUFeaturesAWS.sh;chmod +x CPUFeaturesAWS.sh" <$0
  if ${do_shutdown:-false} || ! runssh test -x $javafilestest;then
    (cd $TESTCLASSES;tar cf - $(eval echo $classfiles))|runssh "set -ex;tar xf -"
    local after="sudo chown root:root $javafilescriu;sudo chmod u+s $javafilescriu;bin/java -XX:+ShowCPUFeatures --version"
    if [ -z "$firstupload" ];then
      (cd $JAVA_HOME;tar chf - $(eval echo $javafiles))|runssh "set -ex;tar xf -;$after"
    else
      runssh "ssh -o 'UserKnownHostsFile /dev/null' -o 'StrictHostKeyChecking no' -i key $(ipaddr $firstupload) tar chf - $(eval echo $javafiles)|tar xf -;$after"
    fi
  fi
  if [ -z "$firstupload" ];then
    firstupload=$kind
  fi
}

results=""
exitcode=0
checkpoint_restore_one() {
  local lastline=$1
  if [ -n "$only" ] && ! echo "$only"|grep -w "$lastline";then
    return
  fi
  if [ -z "$lastline" ];then
    lastline="use bash for line numbers"
  fi
  kind_checkpoint="$2"
  kind_restore="$3"
  # restoring java exit code 143 (128+SIGTERM from function restore's kill)
  local expectrc=143
  if [ -n "$4" ];then
    expectrc=${4%%:*}
    expect_error="${4#[-0-9]*:}"
  else
    expectrc=143
    expect_error="CPUFeaturesCheck "
  fi
  checkpoint_args="$5"
  restore_args="$6"
  echo checkpoint_restore called from line=$lastline engine=$engine kind_checkpoint=$kind_checkpoint kind_restore=$kind_restore expect_error=$expect_error checkpoint_args=$checkpoint_args restore_args=$restore_args
  kind=$kind_checkpoint
  setup
  snapshot="$(runssh "./CPUFeaturesAWS.sh internal_checkpoint $enginecmdline $checkpoint_args" 2>&1|tee /proc/self/fd/2)"
  if [ "$expectrc" = -1 ] && echo "$snapshot"|grep "$expect_error";then
    r="$r-PASS"
  else
    if [ $kind_restore != $kind_checkpoint ];then
      kind=$kind_restore
      setup
      runssh "rm -rf cr;ssh -o 'UserKnownHostsFile /dev/null' -o 'StrictHostKeyChecking no' -i key $(ipaddr $kind_checkpoint) tar cf - cr|tar xf -"
    fi
    restore="$(runssh "./CPUFeaturesAWS.sh internal_restore $enginecmdline $restore_args" 2>&1|tee /proc/self/fd/2)"
    local r="$engine"
    if echo "$restore"|grep -w RC=$expectrc && echo "$restore"|grep "$expect_error";then
      r="$r-PASS"
    else
      r="$r-FAIL"
      exitcode=1
    fi
  fi
  echo "$r"
  shift # do not duplicate $lastline
  results="$results\n$lastline: $*:$r"
}

checkpoint_restore_criu() {
  engine=criu
  enginecmdline="-XX:CRaCEngine=criuengine"
  checkpoint_restore_one "$@"
}

checkpoint_restore() {
  checkpoint_restore_criu "$@"
}

arch="$(file $TESTJAVA/bin/java)"

if echo "$arch"|grep -q ', x86-64, ';then
  debianami=ami-081ac37fe26dacc98

# t3a.nano:
# Exception in thread "main" java.lang.OutOfMemoryError: Java heap space
#         at CPUFeaturesAWS.<clinit>(CPUFeaturesAWS.java:32)

# Test 2 issues:
# 1: ZULU-84672: CPUFeatures: Intel/AMD image portability problem; non-contained intersection; a!=(a&b)!=b
# 2: IgnoreCPUFeatures is not inherited from snapshot to restore.
checkpoint_restore "$LINENO" t3a.micro t3.micro "1:Restore failed due to incompatible or missing CPU features, try using -XX:CPUFeatures=0x21461805ddfbf7,0xfcc on checkpoint." \
  "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures" ""

# Test IgnoreCPUFeatures - in this case it only works by luck.
checkpoint_restore "$LINENO" t3a.micro t3.micro "" "" "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"

# Test the current state of things, it may be fixed in the future.
checkpoint_restore "$LINENO" t3a.micro t3.micro "1:VM option .*CPUFeatures.* is not restore-settable and is not available on restore." \
  "" "-XX:CPUFeatures=ignore"

# Test printing during snapshot: "CPU features are being kept intact as requested by -XX:CPUFeatures=ignore"
checkpoint_restore "$LINENO" t3a.micro t3.micro "" "-XX:CPUFeatures=ignore" "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"

checkpoint_restore "$LINENO" t3a.micro t3.micro "" "-XX:CPUFeatures=0x21461805ddfbf7,0xfcc"
checkpoint_restore "$LINENO" t3.micro t3a.micro "" "-XX:CPUFeatures=0x21461805ddfbf7,0xfcc"

checkpoint_restore "$LINENO" m1.small t3.micro

# criu FAIL: JDK-8373027: [CRaC] [CRIU] x86: FPU xsave area present, but host cpu doesn't support it
checkpoint_restore "$LINENO" t3.micro m1.small "" "-XX:CPUFeatures=0x142000070bbd7,0x380" ""

checkpoint_restore "$LINENO" t3.micro t3.micro "" "-XX:CPUFeatures=native" ""

checkpoint_restore "$LINENO" t3.micro t3.micro "1:Restore failed due to wrong or missing CPU architecture (current architecture is amd64)" \
  "-XX:CPUFeatures=ignore" ""

checkpoint_restore "$LINENO" t3.micro t3.micro "" "-XX:CPUFeatures=generic" ""

# Currently the most modern x86_64 CPU in AWS.
# criu FAIL: ZULU-84505: [CRaC] [CRIU] Fix a failure for checkpoint on AWS m8i-flex.large
checkpoint_restore "$LINENO" m8i-flex.large m1.small "" "-XX:CPUFeatures=0x142000070bbd7,0x380" ""

# 8374491: CPUFeatures: check performance regression of AVX_Fast_Unaligned_Load
lastline="$LINENO"
if [ -z "$only" ] || echo "$only"|grep -w $lastline;then
  kind=m1.small
  setup
  if runssh "./CPUFeaturesAWS.sh internal_reexec";then
    r="PASS"
  else
    r="FAIL"
    exitcode=1
  fi
  results="$results\n$lastline:$r"
fi

elif echo "$arch"|grep -q ', ARM aarch64, ';then
  debianami=ami-0aed0cf56d6c464bc

checkpoint_restore "$LINENO" a1.medium a1.medium
checkpoint_restore "$LINENO" t4g.micro t4g.micro
checkpoint_restore "$LINENO" a1.medium t4g.micro
checkpoint_restore "$LINENO" t4g.micro a1.medium "1:Restore failed due to incompatible or missing CPU features, try using -XX:CPUFeatures=0x4000ff on checkpoint."
checkpoint_restore "$LINENO" t4g.micro a1.medium "-1:One cannot disable LSE (0x100) by -XX:CPUFeatures as GLIBC_TUNABLES=glibc.cpu.hwcaps is unsupported on aarch64." "-XX:CPUFeatures=0x4000ff" ""

checkpoint_restore "$LINENO" c7g.medium c7g.medium
checkpoint_restore "$LINENO" c8g.medium c8g.medium
# JDK-8385359: checkpoint_restore "$LINENO" c7g.medium c8g.medium
checkpoint_restore "$LINENO" c8g.medium c7g.medium "1:Restore failed due to incompatible or missing CPU features, try using -XX:CPUFeatures=0x17fff on checkpoint."
checkpoint_restore "$LINENO" c8g.medium c7g.medium "" "-XX:CPUFeatures=0x17fff" ""

checkpoint_restore "$LINENO" t4g.micro c8g.medium "1:Restore failed due to incompatible aarch64 CPU feature PACA (0x10000); these CPUs each require a separate image."
checkpoint_restore "$LINENO" c8g.medium t4g.micro "1:Restore failed due to incompatible aarch64 CPU feature PACA (0x10000); these CPUs each require a separate image."

else
  fatal "Unknown arch: $arch"
fi

shutdown
echo -e "$results"
echo done, exitcode=$exitcode
exit $exitcode
