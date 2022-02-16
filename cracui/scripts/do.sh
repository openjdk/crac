#!/bin/bash

bash configure --with-boot-jdk=/home/kznts9v_1lya/Education/CRaC_Java_UI/OpenJDK/jdk-17/ --with-jtreg=/home/kznts9v_1lya/Education/CRaC_Java_UI/OpenJDK/jdk/jtreg/
make images
cp -f ../../Criu/criu-dist/sbin/criu ./build/linux-x86_64-server-release/images/jdk/lib/
sudo chown root:root build/linux-x86_64-server-release/images/jdk/lib/criu
sudo chmod u+s build/linux-x86_64-server-release/images/jdk/lib/criu