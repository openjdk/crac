package sun.nio.ch;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.internal.crac.Core;
import jdk.internal.crac.JDKContext;
import jdk.internal.crac.JDKResource;

import java.util.Arrays;

class NativeFileDescriptorResource implements JDKResource {
    private final Object owner;
    private int[] fds = new int[] { -1, -1, -1 };

    NativeFileDescriptorResource(Object owner) {
        this.owner = owner;
        Core.getJDKContext().register(this);
    }

    public void add(int fd) {
        for (int i = 0; i < fds.length; ++i) {
            if (fds[i] < 0) {
                fds[i] = fd;
                return;
            }
        }
        int prevLength = fds.length;
        fds = Arrays.copyOf(fds, prevLength * 2);
        fds[prevLength] = fd;
        Arrays.fill(fds, prevLength + 1, fds.length - 1, -1);
    }

    public void remove(int fd) {
        for (int i = 0; i < fds.length; ++i) {
            if (fds[i] == fd) {
                fds[i] = -1;
                return;
            }
        }
        throw new IllegalArgumentException("File descriptor " + fd + " not present in " + Arrays.toString(fds));
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        for (int i = 0; i < fds.length; ++i) {
            if (fds[i] >= 0) {
                ((JDKContext) context).claimNativeFd(fds[i], owner);
            }
        }
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
    }

    @Override
    public Priority getPriority() {
        return Priority.FILE_DESCRIPTORS;
    }
}
