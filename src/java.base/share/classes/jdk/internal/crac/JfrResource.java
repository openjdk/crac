package jdk.internal.crac;

import jdk.internal.crac.mirror.Context;
import jdk.internal.crac.mirror.Resource;

class JfrResource implements JDKResource {
    private Runnable startRecording;

    JfrResource() {
        Core.Priority.JFR.getContext().register(this);
    }

    @Override
    public void beforeCheckpoint(Context<? extends Resource> context) {
    }

    @Override
    public void afterRestore(Context<? extends Resource> context) {
        if (startRecording != null) {
            startRecording.run();
        }
    }

    public void setStartFlightRecorder(Runnable runnable) {
        this.startRecording = runnable;
    }
}
