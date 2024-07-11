# CRaC JDK

For build instructions please see the
[online documentation](https://openjdk.org/groups/build/doc/building.html),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.

## Building with CRIU

To be able to use the default CRIU-based CRaC implementation follow these
additional steps after building the JDK:

1. Download a build of [modified CRIU](https://github.com/CRaC/criu/releases/tag/release-1.4)
2. Extract and copy `criu` binary over a file with the same name in the JDK
   ```
   cp $CRIU_DIR/sbin/criu $JDK_DIR/lib/criu
   ```
3. Grant permissions to allow a regular user to run it
   ```
   sudo chown root:root $JDK_DIR/lib/criu
   sudo chmod u+s $JDK_DIR/lib/criu
   ```
