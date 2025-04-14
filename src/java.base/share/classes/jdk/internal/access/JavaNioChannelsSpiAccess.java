package jdk.internal.access;

import java.nio.channels.spi.AbstractInterruptibleChannel;

public interface JavaNioChannelsSpiAccess {
    void setChannelReopened(AbstractInterruptibleChannel channel);
}
