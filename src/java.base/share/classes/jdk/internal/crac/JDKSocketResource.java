package jdk.internal.crac;

import jdk.crac.impl.CheckpointOpenSocketException;

import java.io.IOException;
import java.net.*;

public abstract class JDKSocketResource extends JDKSocketResourceBase {

    private SocketAddress local;
    private SocketAddress remote;

    public JDKSocketResource(Object owner) {
        super(owner);
    }

    protected abstract SocketAddress localAddress() throws IOException;
    protected abstract SocketAddress remoteAddress() throws IOException;

    @Override
    protected OpenResourcePolicies.Policy findPolicy(boolean isRestore) throws CheckpointOpenSocketException {
        if (!isRestore) {
            try {
                local = localAddress();
            } catch (IOException e) {
                throw new CheckpointOpenSocketException("Cannot find local address for " + owner, e);
            }
            try {
                remote = remoteAddress();
            } catch (IOException e) {
                throw new CheckpointOpenSocketException("Cannot find remote address for " + owner, e);
            }
        }
        var localMatcher = getMatcher(local, "localAddress", "localPort", "localPath");
        var remoteMatcher = getMatcher(remote, "remoteAddress", "remotePort", "remotePath");
        return OpenResourcePolicies.find(isRestore, OpenResourcePolicies.SOCKET,
                params -> localMatcher.test(params) && remoteMatcher.test(params));
    }

    @Override
    protected void reset() {
        // Allow garbage collection
        local = null;
        remote = null;
    }
}
