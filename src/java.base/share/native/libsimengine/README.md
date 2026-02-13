# SimEngine library

This library implements the C/R API (see `src/hotspot/share/include/crlib/`) used as Checkpoint/Restore Engine for CRaC. It does not perform any snapshotting of the process; the checkpoint is either a no-op, or in the `pause=true` case (ATM implemented on Linux only) the invoking thread is waiting until JVM invokes (fake) restore. This serves as a tool for development of the application, mostly on non-Linux platforms where a full C/R mechanism is not available.

## Extensions

For development purposes, SimEngine implements the "Image Constraints" and "Image Score" extensions to the same extent as C/R Exec (see `src/hotspot/share/native/libcrexec`) - in fact the implementation is shared. See the README.md in there for details about the format.
