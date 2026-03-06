# CRaC JDK

## Prerequisites

CRaC uses a pluggable mechanism for process snapshotting (checkpoint); currently this is implemented on Linux using the [CRIU](https://criu.org) project. Please install a recent version of CRIU (4.0+) using your package manager, or build it from sources:

### Ubuntu
```
sudo apt-get update
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y ppa:criu/ppa
sudo apt-get update
sudo apt-get install -y criu
```

### Fedora
```
sudo yum install -y criu
```

### Granting privileges

Checkpoint or restore requires privileges (capabilities) normally belonging only to the `root` user. If you are not running your Java application using the `root` user, you need to grant the privileges to the CRIU binary by setting the SUID bit:

```
sudo chown root:root /usr/sbin/criu
sudo chmod u+s /usr/sbin/criu
```

### Legacy CRIU

Historically the CRaC project maintained a [fork of CRIU](https://github.com/CRaC/criu/releases) with modifications for CRaC. The maintenance of this fork is discontinued; if you need to use the version from this fork please add this to the VM options:
```
-XX:CRaCEngineOptions=criu_location=/path/to/criu,legacy_criu=true
```

## Build

For build instructions please see the
[online documentation](https://git.openjdk.org/jdk/blob/master/doc/building.md),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.
