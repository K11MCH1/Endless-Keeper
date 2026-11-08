package org.keeperrecomped.game;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;

/**
 * The level map on a second built-in screen (Retroid Pocket Duo): a presentation window
 * whose surface the engine draws KeeperFX's map into (with its magnifier and map button),
 * and whose touches go to the engine. The size the map button picks (Large or Fill) is kept
 * here between runs. The window never takes input focus, so touching it leaves the game
 * focused.
 */
final class SecondScreen implements SurfaceHolder.Callback, View.OnTouchListener {
    private static native void surface(android.view.Surface surface);
    private static native void touch(int action, float x, float y, int width, int height);
    private static native void setFill(boolean fill);
    private static native boolean isFill();

    private final Activity activity;
    private final SharedPreferences prefs;
    private Presentation presentation;
    private boolean broken; // the engine lacks the native half (an engine built before this class)

    SecondScreen(Activity activity) {
        this.activity = activity;
        prefs = activity.getSharedPreferences("second_screen", Context.MODE_PRIVATE);
    }

    /** A presentation display other than the one the game is on, or null. */
    private Display findDisplay() {
        DisplayManager displays = activity.getSystemService(DisplayManager.class);
        Display own = activity.getDisplay();
        for (Display d : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION))
            if (d.isValid() && (own == null || d.getDisplayId() != own.getDisplayId())) return d;
        return null;
    }

    void show() {
        if (presentation != null || broken) return;
        Display display = findDisplay();
        if (display == null) return;
        try { setFill(prefs.getBoolean("fill", false)); } catch (UnsatisfiedLinkError e) { broken = true; return; }
        presentation = new Presentation(activity, display) {
            @Override protected void onCreate(Bundle state) {
                super.onCreate(state);
                SurfaceView view = new SurfaceView(getContext());
                view.getHolder().addCallback(SecondScreen.this);
                view.setOnTouchListener(SecondScreen.this);
                setContentView(view);
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                getWindow().setDecorFitsSystemWindows(false);
                if (getWindow().getInsetsController() != null) getWindow().getInsetsController().hide(WindowInsets.Type.systemBars());
            }
        };
        try {
            presentation.show();
        } catch (WindowManager.InvalidDisplayException e) {
            Log.w("KeeperAndroid", "second screen unavailable: " + e.getMessage());
            presentation = null;
        }
    }

    void hide() {
        if (presentation == null) return;
        presentation.dismiss();
        presentation = null;
        if (!broken) prefs.edit().putBoolean("fill", isFill()).apply();
    }

    private void sendSurface(android.view.Surface s) {
        if (broken) return;
        try { surface(s); } catch (UnsatisfiedLinkError e) { broken = true; Log.w("KeeperAndroid", "engine has no second-screen map; rebuild the game"); }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) { }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { sendSurface(holder.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { sendSurface(null); }

    private int finger = -1;
    @Override public boolean onTouch(View v, MotionEvent e) {
        // The first finger down drives the camera until it lifts; others are ignored.
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) finger = e.getPointerId(0);
        int index = finger < 0 ? -1 : e.findPointerIndex(finger);
        if (broken || index < 0) return true;
        boolean lifted = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
            || (action == MotionEvent.ACTION_POINTER_UP && e.getActionIndex() == index);
        if (!lifted && action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_MOVE) return true;
        if (lifted) { finger = -1; if (action != MotionEvent.ACTION_CANCEL) action = MotionEvent.ACTION_UP; }
        try { touch(action, e.getX(index), e.getY(index), v.getWidth(), v.getHeight()); } catch (UnsatisfiedLinkError err) { broken = true; }
        return true;
    }
}
