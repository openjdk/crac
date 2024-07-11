# Portable checkpoint-restore algorithm

This document describes the algorithm used by CRaC in the portable mode.

On checkpoint, three dumps are created:

1. Dump of Java threads' states — includes only user-created threads, while GC, compiler and other VM-created threads
   are excluded since their state is not portable.
    - If an included thread executes native code the checkpoint gets postponed in hopes that the thread will leave the
      native code soon
    - Some special cases (e.g. CRaC's own native code) are handled specifically and are allowed to be checkpointed
2. Class dump — basically a list of serialized `InstanceKlass`es.
3. Heap dump — currently an HPROF dump.

The following sections describe the process of restoration from the above files.

## Metaspace and heap

Restoration of class metadata and objects begins after the second phase of VM initialization because "only java.base
classes can be loaded until phase 2 completes" while the classes being restored can come from arbitrary modules. At that
point a lot of JDK's own Java code has been executed (e.g. foundational classes such as `java.lang.Class` have been
loaded and initialized, initialization methods of `java.lang.System` have been executed and even CRaC's own
`jdk.crac.Core` with the global context has been used), so many system classes already exist in a new state. And such
re-initialization cannot be omitted since it is platform-dependent, e.g. system properties which are set up during the
first initialization phase must be retrieved from the current system and not restored from the old one.

The current implementation even begins the restoration after the initialization fully completes as to not be bothered
by stuff like proper system class loader and security manager initialization, so even some user code can get executed
(from a user-provided system class loader, for instance).

It is planned to change the implementation to restore all classes before any Java code is executed so that there is no
need to match the new and the checkpointed states.

### Defining the dumped classes

First, class dump is parsed and the dumped classes are defined. The dump is structured in such a way that the classes
can be defined as the dump is being parsed: super classes and interfaces of a class as well as its class loader's class
all precede the class in the dump.

Prior to allocating a class its defining class loader must be prepared if it does not already exist (i.e. if it is not
the bootstrap, platform or system loader). Preparation of a class loader consists of its allocation and restoration of
a few of its fields used during class definition, such as its name and array of defined classes. The class of the class
loader is already defined at this moment which is guaranteed by the class dump structure. The classes of the restored
fields' values are also defined since they are all well-known system classes used in the VM initialization process. All
objects created during preparation are recorded with their dumped IDs so that they are not re-created during the
subsequent heap restoration.

After a class is allocated, it is defined. If the class is already defined (i.e. it is a system class) the pre-defined
version is used. It is assumed that the pre-defined version is compatible with the one created from the dump, i.e. it is
created from the same class file, but this is not checked for performance reasons (only non-product asserts exist). If
the pre-defined version has not yet been rewritten but the dumped one was, methods' code is transferred from the dumped
class into the pre-defined one. After that, if there was a pre-defined version, the newly-created one is deallocated.

> There is a problem with hidden classes in this approach: it is impossible to know if a hidden class has been defined
> already or not because there can be an arbitrary amount of hidden classes defined with the same class loader from the
> same class file. Because of this the algorithm will always define a new hidden class. This may be a problem if a
> pre-defined class and a newly-created class or a restored stack value reference such class, for example:
>  - Let's assume that at the time of checkpoint `java.lang.System` and a local variable of the main thread
     >    referenced a hidden class `H` created early during the VM initialization (i.e. they referenced an instance of
     >    `H` or an instance of `java.lang.Class<H>`).
>  - In this case, when we restore, `System` is again initialized early by the VM and so is `H` which becomes referenced
     >    by `System` again.
>  - Then we start the restoration, the algorithm cannot find `H` since it is hidden and defines a new version of it.
>  - After that the restored main thread will use the new version of `H` while `System` uses another one — this is
     >    contrary to what was checkpointed.

After all dumped classes have been defined, inter-class references of their constant pools are filled. This cannot be
performed while parsing since these references can contain cycles: if class `A` references class `B` and `B` references
`A` we need to define both before these references can be restored.

Then, classes are recorded with their non-defining initiating loaders so that these won't repeat the loading process.
Loading constraints are also restored (not implemented yet).

### Restoring the heap

Firstly, `java.lang.Class` objects mirroring all created classes are recorded as created with their dumped IDs. Their
fields which are filled during object creation (e.g. name, class loader and module references) are also recorded.

> In fact, all references from the pre-initialized (i.e. defined and initialized during the VM restoration) classes must
> also be recorded at this step, or they will be duplicated if referenced from a non-pre-defined class or a stack value
> of a thread being restored. See the note about hidden classes above for an example — here the idea is the same. If we
> figure out a way to fix this, we'll also fix the hidden classes problem because a reference to a hidden class is a
> reference to an object: either its `java.lang.Class` or its instance. The current implementation already pre-records
> platform and system loaders to not duplicate them.
>
> Fixing this is not trivial because we would need to match the new state of the pre-initialized classes with the
> dumped one while they may not be compatible. E.g. it's not clear what to do if a field `F` is dumped as containing an
> object of class `C1` with two fields but in the new state field `F` contains an object of class `C2` with a single
> field or no object at all.
>
> And also we'll need to account for pre-existing threads which can change the state of classes concurrently
> (newly-defined ones are captured by the restoring thread and cannot be tempered with). But the right way to solve this
> is probably to block user-threads from being created until the restoration finishes and ensure VM-created threads
> won't do this (not currently implemented, cannot use the safepoint mechanism for this since objects allocation is
> needed during restoration).

Then, each class that has not been pre-initialized is restored (pre-initialized classes are skipped since they already
have a new state):

- Fields of its `java.lang.Class` object are restored. If a field is already set, it's value is recorded instead — this
  can happen because `Class` object can be used even before the corresponding class is initialized. Previously prepared
  class loaders are also restored here since the defining loader is referenced by `Class`.
  > In fact, we should block concurrent modification of these objects and pre-record references from them for all
  pre-defined classes just as we should do for static fields of pre-initialized classes (as noted above).
- Its static fields are restored.

After a class is restored, its state is set to the target state, e.g. it is marked as initialized.

Finally, references from stacks of dumped threads are restored.

The restoration of references is recursive: to restore a reference to an object means to find its class, allocate the
object, record it with its ID and restore its fields some of which may also be references requiring the same (recursive)
restoration process.

Instances of many system classes require special treatment because they have platform-dependent fields and even raw
pointers which cannot be filled as-is or because they have to be recorded in VM's data structures.

> Handling of some system classes even involves Java code invocation. We rely on the fact that this is a well-known code
> which won't use any classes not yet fully restored or change the state of the classes.

## Threads

To restore a threads means to restore its `java.lang.Thread` object, which is done during the heap restoration described
above, and to make it resume its execution from the moment it left off.

The `main` thread and its Java object is created early during the VM's initialization process, so it is not restored.
`main` is also currently the single thread that performs the whole restoration process.

After the classes and objects have been restored, `main` creates platform threads for each checkpointed thread stack
except its own and makes them wait on a latch. After the threads have been created, `main` releases the latch and the
threads start restoring their executions. If there was an execution checkpointed for `main` it then also starts
restoring that execution.

Restoration of an execution is implemented in a way similar to the deoptimization implementation (some of its parts
are even used directly).

1. Thread calls into the oldest method on the execution stack (i.e. the one that was called first) replacing its entry
   point with an entry into a special stack restoration blob written in assembly.
2. The blob calls back into C++ code to convert the checkpointed stack into the format used by the deoptimization
   implementation — this is done inside the Java call because there should be no safepoints while the stack exists in
   that format.
3. The blob creates interpreted stack frames of required sizes and calls into C++ code again to fill them with restored
   data.
4. After the frames have been restored, the control flow is passed to the interpreter and the execution is resumed.
