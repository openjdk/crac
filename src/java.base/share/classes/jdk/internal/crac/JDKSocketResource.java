package jdk.internal.crac;

import jdk.internal.crac.mirror.impl.CheckpointOpenSocketException;

import java.io.IOException;
import java.net.*;
import java.util.Map;
import java.util.function.Predicate;

public abstract class JDKSocketResource extends JDKSocketResourceBase {

    private SocketAddress local;
    private SocketAddress remote;

    public JDKSocketResource(Object owner) {
        super(owner);
    }

    protected abstract SocketAddress localAddress() throws IOException;
    protected abstract SocketAddress remoteAddress() throws IOException;
    protected abstract boolean isListening();

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
        Predicate<Map<String, String>> listenMatcher = params -> {
            String cfgListening = params.get("listening");
            return cfgListening == null || Boolean.parseBoolean(cfgListening) == isListening();
        };
        return OpenResourcePolicies.find(isRestore, OpenResourcePolicies.SOCKET,
                params -> localMatcher.test(params) && remoteMatcher.test(params) && listenMatcher.test(params));
    }

    @Override
    protected void reset() {
        // Allow garbage collection
        local = null;
        remote = null;
    }
}
