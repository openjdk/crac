package sun.nio.ch.sctp;

import jdk.internal.crac.OpenResourcePolicies;
import jdk.internal.crac.JDKSocketResourceBase;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.nio.channels.spi.AbstractSelectableChannel;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Predicate;

abstract class SctpResource extends JDKSocketResourceBase {
    private Set<InetSocketAddress> localCopy;
    private Set<SocketAddress> remoteCopy;

    public SctpResource(AbstractSelectableChannel sctpChannel) {
        super(sctpChannel);
    }

    @Override
    protected OpenResourcePolicies.Policy findPolicy(boolean isRestore) {
        if (!isRestore) {
            localCopy = Set.copyOf(getLocalAddresses());
            remoteCopy = Set.copyOf(getRemoteAddresses());
        }
        List<Predicate<Map<String, String>>> localMatchers = localCopy.stream()
                .map(addr -> getMatcher(addr, "localAddress", "localPort", null)).toList();
        List<Predicate<Map<String, String>>> remoteMatchers = remoteCopy.stream()
                .map(addr -> getMatcher(addr, "remoteAddress", "remotePort", null)).toList();
        return OpenResourcePolicies.find(false, OpenResourcePolicies.SOCKET,
                params -> localMatchers.stream().anyMatch(matcher -> matcher.test(params)) &&
                        remoteMatchers.stream().anyMatch(matcher -> matcher.test(params)));
    }

    @Override
    protected void reset() {
        localCopy = null;
        remoteCopy = null;
    }

    @Override
    protected abstract void closeBeforeCheckpoint() throws IOException;

    protected abstract Set<SocketAddress> getRemoteAddresses();

    protected abstract HashSet<InetSocketAddress> getLocalAddresses();
}
