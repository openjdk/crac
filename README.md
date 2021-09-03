# CRaC JDK

The `jdk-base` tracks latest OpenJDK GA revision.
Now it is `jdk14-ga`.

The main branch for CRaC implementation is `jdk-crac` (hosts `jdk.crac` package).

## Build

CRaC JDK have extended build procedure.

1. Build JDK as usual
```
bash configure
make images
mv build/linux-x86_64-server-release/images/jdk/ .
```
2. Download a build of [modified CRIU](https://github.com/org-crac/criu/releases/tag/release-crac)
3. Extract and copy `criu` binary over a same named file in the JDK
```
cp criu-dist/sbin/criu jdk/lib/criu
```
Grant permissions to allow regular user to run it
```
sudo chown root:root jdk/lib/criu
sudo chmod u+s jdk/lib/criu
```
