package com.sun.management.internal;

import com.sun.management.CRaCMXBean;
import sun.management.Util;
import sun.management.VMManagement;

import javax.management.ObjectName;

public class CRaCImpl implements CRaCMXBean {
    private final VMManagement vm;

    public CRaCImpl(VMManagement vm) {
        this.vm = vm;
    }

    @Override
    public long getUptimeSinceRestore() {
        return vm.getUptimeSinceRestore();
    }

    @Override
    public long getRestoreTime() {
        return vm.getRestoreTime();
    }

    @Override
    public ObjectName getObjectName() {
        return Util.newObjectName(PlatformMBeanProviderImpl.CRAC_MXBEAN_NAME);
    }
}
