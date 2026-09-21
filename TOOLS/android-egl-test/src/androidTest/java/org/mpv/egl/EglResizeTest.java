package org.mpv.egl;

import static androidx.test.platform.app.InstrumentationRegistry.getInstrumentation;
import static org.junit.Assert.*;

import android.animation.ValueAnimator;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.DataSpace;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Size;
import android.view.Gravity;
import android.view.Surface;
import android.widget.FrameLayout;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class EglResizeTest {
    private SurfaceActivity activity;
    private Surface surface;
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLConfig config;
    private EGLSurface originalSurface = EGL14.EGL_NO_SURFACE;
    private long probe;
    private int texture;
    private int frameId;
    private ValueAnimator animator;
    private SurfaceTexture replacementTexture;
    private Surface replacementSurface;

    @Before
    public void setUp() throws Exception {
        activity = (SurfaceActivity) getInstrumentation().startActivitySync(
            new Intent(getInstrumentation().getTargetContext(), SurfaceActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        assertTrue("Surface not ready", activity.ready.await(5, TimeUnit.SECONDS));
        resize(256, 144);
        surface = activity.surfaceView.getHolder().getSurface();
        assertEquals(0, NativeProbe.setGeometry(surface, 256, 144));
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        assertNotEquals(EGL14.EGL_NO_DISPLAY, display);
        int[] version = new int[2];
        assertTrue(EGL14.eglInitialize(display, version, 0, version, 1));
        int[] attributes = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        assertTrue(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0));
        assertEquals(1, count[0]);
        config = configs[0];
        context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
            new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        assertNotEquals(EGL14.EGL_NO_CONTEXT, context);
        originalSurface = EGL14.eglCreateWindowSurface(display, configs[0], surface,
            new int[] {EGL14.EGL_NONE}, 0);
        assertNotEquals(EGL14.EGL_NO_SURFACE, originalSurface);
        assertTrue(EGL14.eglMakeCurrent(display, originalSurface, originalSurface, context));
        assertTrue(EGL14.eglSwapInterval(display, 0));
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        texture = textures[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture);
        probe = NativeProbe.createProbe(surface);
        submit(NativeProbe.reconfigure(probe, 256, 144));
        assertPresented("initial");
    }

    @After
    public void tearDown() {
        try {
            releaseEgl();
        } finally {
            try {
                if (probe != 0)
                    NativeProbe.destroyProbe(probe);
            } finally {
                if (replacementSurface != null)
                    replacementSurface.release();
                if (replacementTexture != null)
                    replacementTexture.release();
                if (activity != null)
                    getInstrumentation().runOnMainSync(activity::finish);
            }
        }
    }

    private void releaseEgl() {
        if (animator != null)
            getInstrumentation().runOnMainSync(animator::cancel);
        // Color negotiation may have replaced the original EGLSurface.
        EGLSurface current = EGL14.eglGetCurrentSurface(EGL14.EGL_DRAW);
        if (texture != 0)
            GLES20.glDeleteTextures(1, new int[] {texture}, 0);
        if (!display.equals(EGL14.EGL_NO_DISPLAY)) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE,
                EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (!current.equals(EGL14.EGL_NO_SURFACE))
                EGL14.eglDestroySurface(display, current);
            if (!context.equals(EGL14.EGL_NO_CONTEXT))
                EGL14.eglDestroyContext(display, context);
            EGL14.eglTerminate(display);
        }
        if (surface != null && surface.isValid())
            assertEquals(0, NativeProbe.setGeometry(surface, 0, 0));
    }

    @Test
    public void detachedWindow_nextProducerFollowsConsumerSize() throws Exception {
        NativeProbe.detachProbeWindow(probe);
        assertNextProducerFollowsConsumerSize();
    }

    @Test
    public void releasedWindow_nextProducerFollowsConsumerSize() throws Exception {
        NativeProbe.uninitProbe(probe);
        context = EGL14.EGL_NO_CONTEXT;
        texture = 0;
        assertNextProducerFollowsConsumerSize();
    }

    @Test
    public void replacedWindow_nextProducerFollowsConsumerSize() throws Exception {
        replacementTexture = new SurfaceTexture(false);
        replacementTexture.setDefaultBufferSize(256, 144);
        replacementSurface = new Surface(replacementTexture);
        NativeProbe.replaceProbeWindow(probe, replacementSurface);
        assertNextProducerFollowsConsumerSize();
    }

    private void assertNextProducerFollowsConsumerSize() throws Exception {
        // A geometry override survives EGL disconnect. Only the old producer
        // can clear it without overriding the next producer's sizing policy.
        for (Size size : new Size[] {new Size(640, 360), new Size(144, 256), new Size(256, 144)}) {
            resize(size.getWidth(), size.getHeight());
            assertSame(surface, activity.surfaceView.getHolder().getSurface());
            EGLSurface next = EGL14.eglCreateWindowSurface(display, config, surface,
                new int[] {EGL14.EGL_NONE}, 0);
            assertNotEquals(EGL14.EGL_NO_SURFACE, next);
            try {
                int[] width = new int[1];
                int[] height = new int[1];
                assertTrue(EGL14.eglQuerySurface(display, next, EGL14.EGL_WIDTH, width, 0));
                assertTrue(EGL14.eglQuerySurface(display, next, EGL14.EGL_HEIGHT, height, 0));
                assertEquals("Previous producer retained buffer width", size.getWidth(), width[0]);
                assertEquals("Previous producer retained buffer height", size.getHeight(), height[0]);
            } finally {
                assertTrue(EGL14.eglDestroySurface(display, next));
            }
        }
    }

    @Test
    public void firstFrameAndPausedRedrawKeepDrawableAligned() throws Exception {
        assertEquals(1, NativeProbe.setProbeDataspace(probe, DataSpace.DATASPACE_DISPLAY_P3));
        for (int step = 0; step < 12; step++) {
            int width = step % 2 == 0 ? 640 : 256;
            int height = step % 2 == 0 ? 360 : 144;
            resize(width, height);
            NativeProbe.State before = NativeProbe.reconfigure(probe, width, height);
            assertPreserved(before);
            assertEquals(DataSpace.DATASPACE_DISPLAY_P3, before.dataspace);
            submit(before);
            NativeProbe.State after = NativeProbe.check(probe, step % 2 != 0);
            assertPreserved(after);
            assertEquals(width, after.voWidth);
            assertEquals(height, after.voHeight);
            assertTrue("Missing resize event", after.resizeEvent);
            assertFalse("Duplicate resize event", NativeProbe.check(probe, false).resizeEvent);
            assertPresented("first-frame-" + step);
        }
    }

    @Test
    public void continuousResizePreservesSurfaceAndFrameSubmission() throws Exception {
        animator = ValueAnimator.ofFloat(0, 1);
        animator.setDuration(150);
        animator.setRepeatCount(7);
        animator.setRepeatMode(ValueAnimator.REVERSE);
        animator.addUpdateListener(value -> {
            float f = (float) value.getAnimatedValue();
            activity.surfaceView.setLayoutParams(new FrameLayout.LayoutParams(
                256 + Math.round(384 * f), 144 + Math.round(216 * f), Gravity.CENTER));
        });
        getInstrumentation().runOnMainSync(animator::start);
        long started = SystemClock.elapsedRealtime();
        long lastFrame = started;
        long maximumGap = 0;
        int frames = 0;
        int resizes = 0;
        Size previous = activity.surfaceSize;
        while (SystemClock.elapsedRealtime() - started < 1500) {
            Size size = activity.surfaceSize;
            assertNotNull("Surface disappeared during animation", size);
            if (!size.equals(previous)) {
                NativeProbe.reconfigure(probe, size.getWidth(), size.getHeight());
                previous = size;
                resizes++;
            }
            NativeProbe.State state = NativeProbe.check(probe, false);
            assertPreserved(state);
            submit(state);
            long now = SystemClock.elapsedRealtime();
            maximumGap = Math.max(maximumGap, now - lastFrame);
            lastFrame = now;
            frames++;
        }
        getInstrumentation().runOnMainSync(animator::cancel);
        report("EGL_ANIMATION frames=" + frames + " resizes=" + resizes
            + " maximumSubmissionGapMs=" + maximumGap);
        assertTrue("Too few animated size changes: " + resizes, resizes >= 12);
        assertTrue("Too few submitted frames: " + frames, frames >= 30);
        assertTrue("Submission stalled: " + maximumGap + " ms", maximumGap < 150);
    }

    @Test
    public void pendingColorResizeAndWindowReattachKeepContext() throws Exception {
        resize(640, 360);
        NativeProbe.reconfigure(probe, 640, 360);
        NativeProbe.check(probe, false);
        String extensions = EGL14.eglQueryString(display, EGL14.EGL_EXTENSIONS);
        report("EGL_COLORSPACE supported=" + extensions.contains("EGL_KHR_gl_colorspace"));
        assertTrue("Color negotiation changed drawable behind VO metadata",
            NativeProbe.checkPendingColorResize(probe));
        submit(NativeProbe.check(probe, false));
        assertPresented("pending-color");
        resize(256, 144);
        NativeProbe.reconfigure(probe, 256, 144);
        NativeProbe.cycleProbeWindow(probe);
        assertEquals(context, EGL14.eglGetCurrentContext());
        assertTrue(GLES20.glIsTexture(texture));
        NativeProbe.State attached = NativeProbe.check(probe, false);
        assertAligned(attached);
        assertEquals("Reattached surface lost configured swap interval", 0, attached.swapInterval);
        submit(attached);
        assertPresented("reattached");
    }

    private void assertAligned(NativeProbe.State state) {
        assertEquals(1, state.result);
        assertEquals(state.drawableWidth, state.voWidth);
        assertEquals(state.drawableHeight, state.voHeight);
    }

    private void assertPreserved(NativeProbe.State state) {
        assertAligned(state);
        assertEquals("Ordinary resize recreated EGLSurface", 0, state.surfaceCreations);
        assertEquals(context, EGL14.eglGetCurrentContext());
        assertEquals(originalSurface, EGL14.eglGetCurrentSurface(EGL14.EGL_DRAW));
        assertTrue("GL texture lost", GLES20.glIsTexture(texture));
    }

    private void submit(NativeProbe.State state) {
        assertAligned(state);
        int width = state.voWidth;
        int height = state.voHeight;
        GLES20.glViewport(0, 0, width, height);
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        GLES20.glClearColor(1, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
        GLES20.glScissor(width / 2, 0, width - width / 2, height);
        GLES20.glClearColor(0, 0, 1, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        // Encode a frame ID at the bottom: screenshots must not accept an older frame.
        frameId = (frameId + 1) & 255;
        for (int bit = 0; bit < 8; bit++) {
            int left = width * bit / 8;
            GLES20.glScissor(left, 0, width * (bit + 1) / 8 - left, height / 8);
            boolean one = (frameId & (1 << bit)) != 0;
            GLES20.glClearColor(one ? 0 : 1, one ? 1 : 0, one ? 0 : 1, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        }
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        assertEquals(GLES20.GL_NO_ERROR, GLES20.glGetError());
        assertTrue("Expected exactly one successful swap", NativeProbe.swapProbe(probe));
    }

    private void resize(int width, int height) throws Exception {
        Size target = new Size(width, height);
        if (target.equals(activity.surfaceSize))
            return;
        CountDownLatch changed = new CountDownLatch(1);
        getInstrumentation().runOnMainSync(() -> {
            activity.resized = changed;
            activity.surfaceView.setLayoutParams(
                new FrameLayout.LayoutParams(width, height, Gravity.CENTER));
        });
        assertTrue("Surface resize timed out", changed.await(5, TimeUnit.SECONDS));
        assertEquals(target, activity.surfaceSize);
        activity.resized = null;
        getInstrumentation().waitForIdleSync();
    }

    private void assertPresented(String name) throws Exception {
        Rect bounds = new Rect();
        getInstrumentation().runOnMainSync(() -> {
            int[] location = new int[2];
            activity.surfaceView.getLocationOnScreen(location);
            bounds.set(location[0], location[1], location[0] + activity.surfaceView.getWidth(),
                location[1] + activity.surfaceView.getHeight());
        });
        long deadline = SystemClock.elapsedRealtime() + 1000;
        Bitmap screenshot;
        while (true) {
            screenshot = getInstrumentation().getUiAutomation().takeScreenshot();
            assertNotNull("Screenshot unavailable", screenshot);
            if (hasFrameId(screenshot, bounds) || SystemClock.elapsedRealtime() >= deadline)
                break;
            screenshot.recycle();
            // Wait only for presentation; do not render or swap any additional frames.
            SystemClock.sleep(10);
        }
        try {
            File output = new File(activity.getExternalFilesDir(null), name + ".png");
            try (FileOutputStream stream = new FileOutputStream(output)) {
                assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
            }
            assertTrue("Submitted frame not presented: " + name, hasFrameId(screenshot, bounds));
            for (float x : new float[] {0.05f, 0.25f, 0.75f, 0.95f}) {
                for (float y : new float[] {0.05f, 0.5f, 0.80f}) {
                    int color = screenshot.getPixel(bounds.left + (int) (bounds.width() * x),
                        bounds.top + (int) (bounds.height() * y));
                    assertTrue("Incomplete drawable: " + name + " at " + x + "," + y,
                        x < 0.5f ? isRed(color) : isBlue(color));
                }
            }
            int redEnd = -1;
            int blueStart = -1;
            for (int x = bounds.left; x < bounds.right; x++) {
                int color = screenshot.getPixel(x, bounds.centerY());
                if (isRed(color)) redEnd = x + 1;
                if (isBlue(color) && blueStart < 0) blueStart = x;
            }
            assertTrue("Red split shifted: " + name, Math.abs(redEnd - bounds.centerX()) <= 2);
            assertTrue("Blue split shifted: " + name, Math.abs(blueStart - bounds.centerX()) <= 2);
            report("EGL_PRESENTED " + name + " frame=" + frameId + " bounds=" + bounds);
        } finally {
            screenshot.recycle();
        }
    }

    private boolean hasFrameId(Bitmap bitmap, Rect bounds) {
        for (int bit = 0; bit < 8; bit++) {
            int color = bitmap.getPixel(bounds.left + bounds.width() * (2 * bit + 1) / 16,
                bounds.top + bounds.height() * 15 / 16);
            boolean one = (frameId & (1 << bit)) != 0;
            if (one ? Color.green(color) < 200 || Color.red(color) > 50
                    : Color.green(color) > 50 || Color.red(color) < 200 || Color.blue(color) < 200)
                return false;
        }
        return true;
    }

    private static boolean isRed(int color) {
        return Color.red(color) > 200 && Color.blue(color) < 50;
    }

    private static boolean isBlue(int color) {
        return Color.blue(color) > 200 && Color.red(color) < 50;
    }

    private static void report(String text) {
        Bundle status = new Bundle();
        status.putString("stream", text + "\n");
        getInstrumentation().sendStatus(0, status);
    }
}
