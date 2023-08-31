# CRaC JDK

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

# JDK

For build instructions please see the
[online documentation](https://openjdk.org/groups/build/doc/building.html),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.
