package org.mpv.egl;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Size;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;
import java.util.concurrent.CountDownLatch;

/** A real compositor-backed window, independent of Media3 and the TV application. */
public final class SurfaceActivity extends Activity implements SurfaceHolder.Callback {
    public final CountDownLatch ready = new CountDownLatch(1);
    public SurfaceView surfaceView;
    public volatile Size surfaceSize;
    public volatile CountDownLatch resized;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        FrameLayout layout = new FrameLayout(this);
        layout.setBackgroundColor(Color.BLACK);
        layout.addView(surfaceView, new FrameLayout.LayoutParams(256, 144, Gravity.CENTER));
        setContentView(layout);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceSize = new Size(width, height);
        ready.countDown();
        CountDownLatch signal = resized;
        if (signal != null)
            signal.countDown();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceSize = null;
    }
}
