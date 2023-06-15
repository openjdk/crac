package jdk.internal.crac;

import jdk.crac.Context;
import jdk.crac.Resource;
import jdk.crac.impl.CheckpointOpenFileException;
import jdk.crac.impl.CheckpointOpenSocketException;
import jdk.crac.impl.OpenSocketPolicies;
import jdk.internal.access.JavaIOFileDescriptorAccess;
import jdk.internal.access.SharedSecrets;
import sun.nio.ch.Net;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.*;
import java.util.function.Supplier;

public class JDKSocketResource extends JDKFdResource {
    private static final JavaIOFileDescriptorAccess fdAccess =
            SharedSecrets.getJavaIOFileDescriptorAccess();

    private final Object owner;
    private final ProtocolFamily family;
    private final Supplier<FileDescriptor> fdSupplier;

    private int originalFd = -1;
    private SocketAddress local;
    private SocketAddress remote;
    private boolean error;

    public JDKSocketResource(Object owner, ProtocolFamily family, FileDescriptor fd) {
        this.owner = owner;
        this.family = family;
        this.fdSupplier = () -> fd;
    }

    public JDKSocketResource(Object owner, ProtocolFamily family, Supplier<FileDescriptor> fdSupplier) {
        this.owner = owner;
        this.family = family;
        this.fdSupplier = fdSupplier;
    }

    @SuppressWarnings("fallthrough")
    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) throws Exception {
        FileDescriptor fd = fdSupplier.get();
        if (fd == null) {
            return;
        }
        synchronized (fd) {
            if (!fd.valid()) {
                return;
            }
            originalFd = fdAccess.get(fd);
            try {
                if (family == StandardProtocolFamily.INET || family == StandardProtocolFamily.INET6) {
                    local = Net.localAddress(fd);
                    remote = inetRemoteAddress(fd);
                } else if (family == StandardProtocolFamily.UNIX) {
                    local = unixDomainLocalAddress(fd);
                    remote = unixDomainRemoteAddress(fd);
                } else {
                    LoggerContainer.warn("CRaC: Unknown socket family " + family + " for FD " + originalFd);
                }
            } catch (Exception e) {
                LoggerContainer.error(e,"Error reading local/remote address for FD " + originalFd);
            }
            OpenSocketPolicies.BeforeCheckpoint policy = OpenSocketPolicies.CHECKPOINT.get(originalFd, local, remote);
            switch (policy) {
                case ERROR:
                    error = true;
                    Core.getClaimedFDs().claimFd(fd, owner, () -> new CheckpointOpenSocketException(owner.toString() + "(FD " + originalFd + ")", getStackTraceHolder()), fd);
                    break;
                case WARN_CLOSE:
                    LoggerContainer.warn("CRaC: Socket {0} (FD {1}) was not closed by the application!", owner, originalFd);
                    // intentional fallthrough
                case CLOSE:
                    try {
                        // do not unregister any handlers
                        fdAccess.closeNoCleanup(fd);
                    } catch (IOException e) {
                        throw new CheckpointOpenFileException("Cannot close file descriptor " + fd + " (" + owner + ") before checkpoint", e);
                    }
                    break;
            }
        }
    }

    @SuppressWarnings("fallthrough")
    @Override
    public void afterRestore(Context<? extends Resource> context) throws Exception {
        if (originalFd < 0) {
            return;
        }
        FileDescriptor fd = fdSupplier.get();
        if (fd == null) {
            return;
        }
        synchronized (fd) {
            OpenSocketPolicies.AfterRestorePolicy policy = OpenSocketPolicies.RESTORE.get(originalFd, local, remote);
            try {
                switch (policy.type) {
                    // FIXME: implement
                    case REOPEN_OR_ERROR:
                    case OPEN_OTHER:
                        // Don't throw another error when we've already failed once
                        if (!error) {
                            throw new UnsupportedOperationException("Policy " + policy.type + " not implemented (FD " + originalFd + ")");
                        }
                        break;
                    case KEEP_CLOSED:
                        // nothing to do
                        break;
                }
            } finally {
                // Allow garbage collection
                local = null;
                remote = null;
            }
        }
    }

    private static UnixDomainSocketAddress unixDomainLocalAddress(FileDescriptor fd) {
        byte[] bytes = unixDomainLocalAddress0(fd);
        if (bytes == null || bytes.length == 0) {
            return null;
        }
        String path = new String(bytes);
        return UnixDomainSocketAddress.of(path);
    }

    private static UnixDomainSocketAddress unixDomainRemoteAddress(FileDescriptor fd) {
        byte[] bytes = unixDomainRemoteAddress0(fd);
        if (bytes == null || bytes.length == 0) {
            return null;
        }
        String path = new String(bytes);
        return UnixDomainSocketAddress.of(path);
    }

    private static native byte[] unixDomainLocalAddress0(FileDescriptor fd);
    private static native byte[] unixDomainRemoteAddress0(FileDescriptor fd);
    private static native InetSocketAddress inetRemoteAddress(FileDescriptor fd);
}
