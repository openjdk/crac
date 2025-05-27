#! /bin/bash
# Copyright (c) 2025, Azul Systems, Inc. All rights reserved.
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
# @compile CPUFeatures.java
# @comment It will be always skipped unless you use jtreg option "-manual" which conflicts with JDK's default option "-automatic".
# @comment Therefore either change it in make/RunTests.gmk or run jtreg by hand.
# @run shell/manual CPUFeatures.sh

set -ex -o pipefail
exec >&2

JAVA_HOME=$TESTJAVA
javafiles="{bin/{java,jcmd},lib/{jvm.cfg,lib{crexec,java,jimage,jli,jsvml,net,nio,attach,zip}.so,modules,tzdb.dat,server/{classes.jsa,libjvm.so},criuengine,criu},conf/security/java.security}"
qemuimgurl=https://download.fedoraproject.org/pub/fedora/linux/releases/41/Cloud/x86_64/images/Fedora-Cloud-Base-Generic-41-1.4.x86_64.qcow2
qemuimgsumurl=https://download.fedoraproject.org/pub/fedora/linux/releases/41/Cloud/x86_64/images/Fedora-Cloud-41-1.4-x86_64-CHECKSUM
# FIXME: criu need an update for new kernels:
#qemuimgurl=https://download.fedoraproject.org/pub/fedora/linux/releases/42/Cloud/x86_64/images/Fedora-Cloud-Base-Generic-42-1.1.x86_64.qcow2
#qemuimgsumurl=https://download.fedoraproject.org/pub/fedora/linux/releases/42/Cloud/x86_64/images/Fedora-Cloud-42-1.1-x86_64-CHECKSUM
qemuimgdir=/tmp
qemuimgfile=$qemuimgdir/$(basename $qemuimgurl)
qemuimgsumfile=$qemuimgdir/$(basename $qemuimgsumurl)
# SHA256 (Fedora-Cloud-Base-Generic.x86_64-40-1.14.qcow2) = ac58f3c35b73272d5986fa6d3bc44fd246b45df4c334e99a07b3bbd00684adee
qemuimgsumgrep="^SHA256 ($(basename $qemuimgurl)) = "
if ! grep "$qemuimgsumgrep" $qemuimgsumfile;then
  flock $qemuimgsumfile.lock -c "wget -O $qemuimgsumfile $qemuimgsumurl"
  if ! grep "$qemuimgsumgrep" $qemuimgsumfile;then
    echo >&2 "Failed to download: $qemuimgsumurl"
    exit 1
  fi
fi
function checksum
{
  (cd $qemuimgdir
    grep "^[^#].*$(basename $qemuimgfile)" $qemuimgsumfile|sha256sum -c -|grep "^$(basename $qemuimgurl): OK$"
  )
}
if ! checksum;then
  flock $qemuimgfile.lock -c "wget -O $qemuimgfile $qemuimgurl"
  if ! checksum;then
    echo >&2 "Failed to download: $qemuimgurl"
    exit 1
  fi
fi
tmpdir=/tmp/CPUFeatures.$$
rm -rf $tmpdir
mkdir $tmpdir
qemuimg=$tmpdir/CPUFeatures.qcow2
sshkey=$qemuimg.key
mountdir=$qemuimg.d

test ! -e $mountdir
test ! -e $qemuimg
test ! -e $sshkey
test ! -e $sshkey.pub
ssh-keygen -f $sshkey -N ""
test -f $sshkey
test -f $sshkey.pub
qemu-img create -b $qemuimgfile -F qcow2 -f qcow2 $qemuimg
test -f $qemuimg
mkdir $mountdir
LIBGUESTFS_BACKEND=direct guestmount -a $qemuimg -i $mountdir
sed -i -e 's/^options /&selinux=0 /' $mountdir/boot/loader/entries/*.conf
sed -i -e 's/^root:x:/root::/' $mountdir/etc/passwd
cat $sshkey.pub >>$mountdir/root/.ssh/authorized_keys
rm -f $mountdir/usr/lib/systemd/zram-generator.conf
echo kernel.core_pattern=core >>$mountdir/etc/sysctl.d/CPUFeatures.conf
guestunmount $mountdir
rmdir $mountdir

# -nographic may not be suitable for every OS/image
for try in $(seq 1 10);do
  sshport=$[$RANDOM+1024]
  sshporthex=$(printf %04X $sshport)
  if ! grep -q "^..............:$sshporthex 00000000:0000 0A " /proc/net/tcp \
  && ! grep -q "^....: 00000000000000000000000000000000:$sshporthex 00000000000000000000000000000000:0000 0A " /proc/net/tcp6 \
  ;then
    break
  fi
  unset sshport
done
test -n "$sshport"

qemuimg2=$tmpdir/CPUFeatures-run.qcow2
qemuargs="-m 4096 -net nic -net user,hostfwd=tcp::$sshport-:22 -drive format=qcow2,media=disk,cache=unsafe,file=$qemuimg2 -nographic"

function runssh {
  ssh -i $sshkey -p $sshport -o "UserKnownHostsFile /dev/null" -o "StrictHostKeyChecking no" -o "ConnectTimeout $[10*$timeoutmultiply]" root@127.0.0.1 "$@"
}
# Do not use /tmp as that may not survive a reboot on some Linux distributions.
qemudir="/root/CPUFeatures"
# in reality it is about 20
qemustarttimeout=120
for file in $(eval echo $JAVA_HOME/$javafiles);do
  test -e $file
done
rm -f $qemuimg2

missingfiles=true
function qemucopyfiles {
  missingfiles=false
  (cd $JAVA_HOME;tar chf - $(eval echo $javafiles))|runssh "set -ex;mkdir $qemudir;cd $qemudir;tar xf -;JAVA_HOME=\$PWD bin/java -XX:+ShowCPUFeatures --version"
  (cd $TESTCLASSES;tar cf - CPUFeatures.class)|runssh "set -ex;cd $qemudir;tar xf -"
}
qemustarted=""
function qemustart {
  type=$1
  cpu=$2
  accel=""
  case $type in
    kvm)
      timeoutmultiply=1
      accel="-accel kvm"
      ;;
    system-x86_64)
      timeoutmultiply=6
      ;;
    *)
      fatal "internal error: unknown type: $1"
      ;;
  esac
  qemustarted_check="$type $cpu"
  if [ "$qemustarted" != "$qemustarted_check" ];then
    if [ ! -e $qemuimg2 ];then
      qemuimg2rebuild
    fi
    qemustop
    qemu-system-x86_64 $accel $qemuargs -cpu $cpu </dev/null & qemupid=$!
    # If SSH times out it may be a 32-bit arch but still qemu is already running.
    qemustarted="$qemustarted_check"
    t0=$(date +%s)
    while true;do
      if [ $[$(date +%s)-$t0] -ge $[$qemustarttimeout*$timeoutmultiply] ];then
	echo >&2 "qemu timeout, qemu PID=$qemupid"
	return 1
      fi
      runssh true && break
      sleep 1
    done
  fi
  if $missingfiles;then
    qemucopyfiles
  fi
}
function qemustop {
  if [ -z "$qemustarted" ];then
    return
  fi
  runssh poweroff || kill $qemupid || :
  wait || :
  qemustarted=""
}
function qemuimg2rebuild {
  qemustop
  qemu-img create -b $qemuimg -F qcow2 -f qcow2 $qemuimg2
  test -e $qemuimg2
  # FIXME: why?
  # qemu-kvm: -drive format=qcow2,media=disk,cache=unsafe,file=/tmp/CPUFeatures.3035545/CPUFeatures-run.qcow2: Could not open backing file: Failed to get shared "write" lock
  # Is another process using the image [/tmp/CPUFeatures.3035545/CPUFeatures.qcow2]?
  sleep 1
  missingfiles=true
}
javasetup="cd $qemudir;export JAVA_HOME=\$PWD;ulimit -c unlimited"
function checkpoint {
  checkpoint_args="$1"
  runssh "$javasetup;rm -rf cr || exit 1; \
    $(: 'Prevent on CRIU: Error (criu/pie/restorer.c:2057): Unable to create a thread: -17') \
    for i in \$(seq 1 1000);do /bin/true;done; \
    bin/java -XX:CRaCCheckpointTo=cr -XX:+ShowCPUFeatures $checkpoint_args CPUFeatures&p=\$!;sleep $[3*$timeoutmultiply];bin/jcmd CPUFeatures JDK.checkpoint; \
    wait \$p;true"
}
function restore {
  restore_args="$1"
  restore="$(runssh "$javasetup;bin/java -XX:CRaCRestoreFrom=cr $restore_args&p=\$!;(sleep $[6*$timeoutmultiply];kill \$p)&wait \$p;echo RC=\$?" 2>&1|tee /proc/self/fd/2)"
}
failfile=$tmpdir/failfile
rm -f $failfile
function checkpoint_restore {
  kind_checkpoint="$1"
  kind_restore="$2"
  check="${3:-CPUFeaturesCheck }"
  checkpoint_args="$4"
  restore_args="$5"
  qemuimg2rebuild
  qemustart $kind_checkpoint
  checkpoint "$checkpoint_args"
  qemustart $kind_restore
  restore "$restore_args"
  if [ "$check" != - ];then
    (set +e;echo $restore|grep "$check";checkpoint_restore_result)
  fi
}
# SIGTERM+128; see 'kill \$p' above
expectRC=143
function checkpoint_restore_result {
  rc=$?
  if ! echo $restore|grep RC=$expectRC;then
    rc=99
  fi
  set +x
  echo
  if [ $rc -eq 0 ];then
    echo -n "PASS";
  else
    echo -n "FAIL"
    touch $failfile
  fi
  echo ": criu: "
  set -x
}
function get_features {
  runssh "set -ex;cd $qemudir;JAVA_HOME=\$PWD bin/java -XX:+ShowCPUFeatures --version"|sed -n 's/^This machine.s CPU features are: -XX:CPUFeatures=//p'
}
exitcode=0
shutdown_done=false
function shutdown {
  if $shutdown_done;then return;fi
  shutdown_done=true
  qemustop
  rm -f $qemuimg2 # CPUFeatures.class
  rm -f $qemuimg $sshkey $sshkey.pub
  if [ -e $failfile ];then exitcode=1;fi
  rm -f $failfile
  rmdir $tmpdir
}
trap shutdown EXIT
function fatal {
  shutdown
  echo >&2 "$*"
  exit 1
}

if [ -z "$*" ];then

# Verify reproducibility of: https://jira.azulsystems.com/browse/ZULU-53749
qemuimg2rebuild
qemustart kvm host
if ! get_features|perl -lne '
  $a=0x4ff7fff9dfcfbf7;
  $b=0x1e6;
  /^(.*),(.*)$/ or die;
  die sprintf "FA"."IL: 0x%x required vs. 0x%x found. 0x%x required vs. 0x%x found.\n",$a,eval $1,$b,eval $2 if $a&~eval $1||$b&~eval $2;
  print "PA"."SS: Initial CPU check"
';then
  # One could verify whether lower CPU isn't sufficient. E5-2630v3 is too old, it does not reproduce ZULU-53749.
  fatal "FA$(: )IL: CPU i7-1165G7 or higher required"
fi

# Opteron_G1 is the most basic CPU (x86_64)
# SandyBridge is the first CPU with OSXSAVE+XSAVE (0x0,0x24)
expectRC_save=$expectRC
expectRC=1
checkpoint_restore "kvm           SandyBridge" "kvm           Opteron_G1" "x86: FPU xsave area present, but host cpu doesn't support it" "" "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"
expectRC=$expectRC_save
checkpoint_restore "kvm           Opteron_G1"  "kvm           Opteron_G1"
checkpoint_restore "kvm           Opteron_G1"  "kvm           host"
checkpoint_restore "kvm           host"        "kvm           host"
# "kvm max" works only as "kvm host"
# "system-x86_64 host" is unsupported by qemu
# These may be too slow to run and they do not test much.
#checkpoint_restore "system-x86_64 SandyBridge" "system-x86_64 SandyBridge"
#checkpoint_restore "system-x86_64 max"         "system-x86_64 max"

# IvyBridge is the first superset of SandyBridge
expectRC_save=$expectRC
expectRC=1
checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" -
(set +e;  echo "$restore"|grep "You have to specify" && ! echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
expectRC=$expectRC_save

checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" - "-XX:CPUFeatures=0x142100054bbd7,0xe4"
(set +e;! echo "$restore"|grep "You have to specify" &&   echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)

# This does not crash the guest despite it could.
checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" - "" "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"
(set +e;  echo "$restore"|grep "You have to specify" &&   echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)

# IgnoreCPUFeatures is not inherited from snapshot to restore.
expectRC_save=$expectRC
expectRC=1
checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" - "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures" ""
(set +e;  echo "$restore"|grep "You have to specify" && ! echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
expectRC=$expectRC_save

checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" - "-XX:+UnlockExperimentalVMOptions -XX:-IgnoreCPUFeatures" \
                                                                             "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"
(set +e;  echo "$restore"|grep "You have to specify" &&   echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
checkpoint_restore "kvm           IvyBridge"   "kvm           SandyBridge" - "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures" \
                                                                             "-XX:+UnlockExperimentalVMOptions -XX:-IgnoreCPUFeatures"
expectRC=1
(set +e;  echo "$restore"|grep "You have to specify" && ! echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
expectRC=$expectRC_save

# FAIL: https://jira.azulsystems.com/browse/ZULU-53749
# reproducible on i7-1165G7, not reproducible on E5-2630v3
checkpoint_restore "kvm host"                  "kvm           SandyBridge" - # verified, fastest
#checkpoint_restore "kvm host"                  "system-x86_64 SandyBridge" - # verified, slower
#checkpoint_restore "system-x86_64 max"         "kvm           SandyBridge" - # it does not work
# The crash was RC=139
expectRC=1
(set +e;echo "$restore"|grep "You have to specify" && ! echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
expectRC=$expectRC_save

checkpoint_restore "kvm host"                  "kvm           SandyBridge" - "" "-XX:+UnlockExperimentalVMOptions -XX:+IgnoreCPUFeatures"
expectRC=139
(set +e;echo "$restore"|grep "You have to specify" && ! echo "$restore"|grep "CPUFeaturesCheck ";checkpoint_restore_result)
expectRC=$expectRC_save

checkpoint_restore "kvm IvyBridge"             "kvm           IvyBridge"   "" "-XX:CPUFeatures=native"
checkpoint_restore "kvm IvyBridge"             "kvm           IvyBridge"   "" "-XX:CPUFeatures=ignore"
checkpoint_restore "kvm IvyBridge"             "kvm           SandyBridge" "" "-XX:CPUFeatures=generic"

if false;then # too slow, failing
checkpoint_restore "system-x86_64 max"         "kvm           SandyBridge" "" "-XX:CPUFeatures=0x142100054bbd7,0xe4"
checkpoint_restore "system-x86_64 max"         "system-x86_64 SandyBridge" "" "-XX:CPUFeatures=0x4200000081d7,0x0"
fi

elif [ $# -eq 2 -a "$1" = "--cpu-list" ];then
  list=""
  for cpu in $(qemu-$2 -cpu help|sed -n 's/^x86 \([^ ]*\) .*/\1/p');do
    qemustart $2 $cpu && list="$list$(echo "$(get_features) $cpu")
" || kill $qemupid || :
    qemustop
  done
  set +x
  echo
  echo -n "$list"|perl -lne '/^(\S+),(\S+)( \S+)$/ or die;$x{sprintf "0x%016x,0x%03x",eval $1,eval $2,$3}.=$3;END{print $_.$x{$_} for sort keys %x}'
  echo
else
  set +x
  echo
  echo >&2 "$0: [--cpu-list {kvm|system-x86_64}]"
  echo
fi
set -x

shutdown
echo done, exitcode=$exitcode
exit $exitcode
