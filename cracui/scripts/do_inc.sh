#!/bin/bash

make images
cp -f ../../Criu/criu-dist/sbin/criu ./build/linux-x86_64-server-release/images/jdk/lib/
sudo chown root:root build/linux-x86_64-server-release/images/jdk/lib/criu
sudo chmod u+s build/linux-x86_64-server-release/images/jdk/lib/criu