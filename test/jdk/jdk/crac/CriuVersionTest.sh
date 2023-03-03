#!/bin/bash
## @test
## @run shell CriuVersionTest.sh

exec 2>&1
set -x
export
whereis criu
criu --version
whoami
sudo -n whoami
find . -type f -iname criu /home/runner/jdk-linux-x64
find . -type f -iname criu /home/runner/work/

exit 1; # print output to log