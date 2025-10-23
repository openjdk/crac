# C/R Exec library

This is an implementation the C/R API (see `src/hotspot/share/include/crlib/`) that is used as Checkpoint/Restore Engine for CRaC. Instead of implementing the mechanics on its own it interfaces with different binary utilities that perform the actual operations.

## Extensions

C/R API defines a mechanism for extending the interactions with the engine beyond simple properties-style configuration and the actual checkpoint and restore. The functionality for these extensions is implemented directly in this library.

### Image Constraints

JVM uses the Image Constraints extension to store metadata about current architecture and CPU features. When the checkpoint is created, C/R exec creates text file `tags` in the checkpoint directory with this format:

```
label:cpu.arch=amd64
bitmap:cpu.features=f7fbfd051c8eef03cc03000000000000
```

There is one line per tag created through the `crlib_image_constraints_t` interface, using either the `label:` or `bitmap:` prefix, followed by the tag name and `=`-separated value. In case of bitmap this is encoded as a string of two hexadecimal characters per byte (`<higher-4-bits><lower-4-bits`) in memory order. The string in the example is recorded when JVM runs with:

```
-XX:CPUFeatures=0x3ef8e1c05fdfbf7,0x3cc
```

### Image Score

Image Score records metadata that should assist with selecting the best image before restore. For each metric recorded through the `crlib_image_score_t` interface this file contains one line in format `<metric-name>=<floating-point-value>`, e.g.

```
java.cls.loadedClasses=204.000000
vm.uptime=632.050484
```

C/R exec ensures that the metric names are unique. The floating point value separates decimal part with a dot (`.`), and negative values are permitted. If the metric name contains a newline character the implementation truncates this.

### User Data

While currently not utilized by the JVM, C/R Exec can store custom data in the checkpoint directory. The filename uses the name passed to `crlib_user_data_t`, and the data is encoded as a string of two hexadecimal characters per byte (`<higher-4-bits><lower-4-bits`) in memory order. It is up to JVM to ensure that there would be no conflict with name used by another extension or engine implementation.

