package org.mpv.egl;

import android.view.Surface;

/** JNI adapter for the real mpv backend, not a replacement EGL implementation. */
public final class NativeProbe {
    static { System.loadLibrary("mpvEglTest"); }

    public static native int setGeometry(Surface surface, int width, int height);
    public static native long createProbe(Surface surface);
    public static native void destroyProbe(long handle);
    public static native boolean swapProbe(long handle);
    public static native boolean checkPendingColorResize(long handle);
    public static native int setProbeDataspace(long handle, int dataspace);
    public static native void cycleProbeWindow(long handle);
    public static native void detachProbeWindow(long handle);
    public static native void uninitProbe(long handle);
    public static native void replaceProbeWindow(long handle, Surface surface);
    private static native int[] reconfigureProbe(long handle, int width, int height);
    private static native int[] checkProbeState(long handle, boolean redraw);

    public static State reconfigure(long handle, int width, int height) {
        return new State(reconfigureProbe(handle, width, height));
    }

    public static State check(long handle, boolean redraw) {
        return new State(checkProbeState(handle, redraw));
    }

    public static final class State {
        public final int result, drawableWidth, drawableHeight, voWidth, voHeight;
        public final int surfaceCreations, dataspace, swapInterval;
        public final boolean resizeEvent;

        private State(int[] values) {
            if (values.length != 9)
                throw new IllegalArgumentException("Invalid native state");
            result = values[0];
            drawableWidth = values[1];
            drawableHeight = values[2];
            voWidth = values[3];
            voHeight = values[4];
            surfaceCreations = values[5];
            resizeEvent = values[6] != 0;
            dataspace = values[7];
            swapInterval = values[8];
        }
    }

    private NativeProbe() {}
}
