package org.keeperrecomped.game;

import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.RelativeLayout;
import org.libsdl.app.SDLActivity;
import java.util.ArrayList;
import java.util.Collections;

/** SDL owns the native surface and lifecycle; the launcher owns preparation. */
public final class GameActivity extends SDLActivity {
    private AutoCloseable debugProbe;
    private SecondScreen secondScreen; // the level map on a second built-in screen, if there is one
    private String builtEngine;
    private static native void requestMenu(int action);
    @Override protected String[] getLibraries() { return new String[]{"SDL3", "keeperfx"}; }
    @Override protected String getMainSharedObject() {
        return builtEngine != null ? builtEngine : getApplicationInfo().nativeLibraryDir + "/libkeeperfx.so";
    }
    // The launcher may pass an engine compiled on the phone for the installed KeeperFX version
    // ("Rebuild game"); it links against libkrdeps.so. Falls back to the app's own engine.
    // System.load, not dlopen, so the engine's JNI methods (requestMenu) bind.
    @Override public void loadLibraries() {
        String built = getIntent().getStringExtra("engine_lib");
        if (built != null && new java.io.File(built).isFile()) {
            try {
                System.loadLibrary("SDL3");
                System.loadLibrary("krdeps");
                System.load(built);
                builtEngine = built;
                return;
            } catch (UnsatisfiedLinkError e) {
                android.util.Log.w("KeeperAndroid", "phone-built engine not loadable, using the app's: " + e.getMessage());
            }
        }
        super.loadLibraries();
    }
    @Override protected String getMainFunction() { return "SDL_main"; }
    @Override protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        args.add("--game-dir=" + getIntent().getStringExtra("game_root"));
        args.add("--profile=" + getIntent().getStringExtra("profile"));
        String renderer=getIntent().getStringExtra("renderer");
        args.add("--renderer=" + ("gles".equals(renderer) ? "gles" : "vulkan"));
        args.add("--fps-cap=" + getIntent().getIntExtra("fps_cap", 0));
        args.add("--render-scale=" + getIntent().getFloatExtra("render_scale", 0));
        args.add("--touch-slop=" + ViewConfiguration.get(this).getScaledTouchSlop());
        args.add("--rotate-deadzone=" + getIntent().getFloatExtra("touch_rotate_deadzone_deg", 8));
        args.add("--pinch-deadzone=" + getIntent().getFloatExtra("touch_pinch_deadzone", .12f));
        args.add("--rotate-with-fingers=" + (getIntent().getBooleanExtra("touch_rotate_with_fingers", true) ? "1" : "0"));
        args.add("--pan-sensitivity=" + getIntent().getFloatExtra("touch_pan_sensitivity", 1));
        String caBundle = getIntent().getStringExtra("ca_bundle");
        if (caBundle != null) args.add("--ca-bundle=" + caBundle);
        args.add("--vsync=" + (getIntent().getBooleanExtra("vsync",true) ? "1" : "0"));
        args.add("--show-fps=" + (getIntent().getBooleanExtra("show_fps",false) ? "1" : "0"));
        // SDL lists only the current display mode at startup (often 60 Hz); the engine's
        // frame-rate request must name the panel's real maximum.
        float maxHz = 0;
        Display display = getDisplay();
        if (display != null)
            for (Display.Mode mode : display.getSupportedModes())
                if (mode.getRefreshRate() <= 165.5f) maxHz = Math.max(maxHz, mode.getRefreshRate());
        args.add("--display-max-hz=" + Math.round(maxHz));
        args.add("--input-diagnostics=" + (getIntent().getBooleanExtra("input_diagnostics",false) ? "1" : "0"));
        String[] extra = getIntent().getStringArrayExtra("args");
        if (extra != null) Collections.addAll(args, extra);
        return args.toArray(new String[0]);
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
            && getIntent().getBooleanExtra("debug_show_when_locked", false)) {
            // Display only this test activity over keyguard. This does not unlock
            // the device or change lock-screen settings, and is debug opt-in only.
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController insets=getWindow().getInsetsController();
        if(insets!=null){insets.hide(WindowInsets.Type.systemBars());insets.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);}
        WindowManager.LayoutParams screenParams=getWindow().getAttributes();
        screenParams.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(screenParams);
        String[] engineArgs=getIntent().getStringArrayExtra("args");
        if(engineArgs!=null && java.util.Arrays.asList(engineArgs).contains("-alex")) {
            Button cheats=new Button(this);cheats.setText("Cheats");cheats.setTextSize(12);cheats.setAlpha(.65f);
            cheats.setOnClickListener(v->requestMenu(2));
            cheats.setOnLongClickListener(v->{requestMenu(3);return true;});
            float density=getResources().getDisplayMetrics().density;
            RelativeLayout.LayoutParams params=new RelativeLayout.LayoutParams((int)(88*density),(int)(40*density));
            params.addRule(RelativeLayout.ALIGN_PARENT_END);params.topMargin=(int)(8*density);params.rightMargin=(int)(12*density);
            mLayout.addView(cheats,params);
        }
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
            && getIntent().getBooleanExtra("enable_touch_probe",false)) {
            try { debugProbe=(AutoCloseable)Class.forName("org.keeperrecomped.game.TouchProbe").getMethod("install",android.app.Activity.class).invoke(null,this); }
            catch(ReflectiveOperationException ignored) { }
        }
        if (getIntent().getBooleanExtra("second_screen_map", true)) secondScreen = new SecondScreen(this);
        // Match the real panel. Rendering targets may cap lower, never change simulation TPS.
        Display display = getDisplay();
        int requested = getIntent().getIntExtra("fps_cap", 0);
        if (display != null) {
            Display.Mode current=display.getMode(), best=current;
            float ceiling=requested > 0 ? Math.min(165,requested) : 165;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth()==current.getPhysicalWidth() && mode.getPhysicalHeight()==current.getPhysicalHeight()
                    && mode.getRefreshRate() <= ceiling+.5f && (best.getRefreshRate()>ceiling+.5f || mode.getRefreshRate()>best.getRefreshRate())) best=mode;
            }
            WindowManager.LayoutParams params=getWindow().getAttributes();
            params.preferredDisplayModeId=best.getModeId(); params.preferredRefreshRate=best.getRefreshRate();
            getWindow().setAttributes(params);
        }
    }
    /** The screen safe area for the game's UI (2026-11-24): phones with rounded corners and camera hole punches
     *  cut the side panel, the minimap and the clock. The 3D view still fills the screen; the engine keeps its UI
     *  inside these insets ({left, right, top, bottom}, pixels; SDL_GetWindowSafeArea). Launcher setting
     *  "Screen safe area": auto = the camera cutout's safe insets and, for each rounded corner of radius r,
     *  r * (1 - 1/sqrt 2) from both edges along it (a square corner that far in clears the curve); manual = the
     *  player's margin on every side; off = none. */
    @Override protected int[] safeAreaInsets(WindowInsets insets) {
        String mode = getIntent().getStringExtra("safe_area");
        int[] safe = new int[4];
        if ("off".equals(mode))
            return safe;
        if ("manual".equals(mode)) {
            int px = Math.round(getIntent().getIntExtra("safe_area_margin", 16) * getResources().getDisplayMetrics().density);
            java.util.Arrays.fill(safe, Math.max(0, px));
            return safe;
        }
        android.view.DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout != null) {
            safe[0] = cutout.getSafeInsetLeft();
            safe[1] = cutout.getSafeInsetRight();
            safe[2] = cutout.getSafeInsetTop();
            safe[3] = cutout.getSafeInsetBottom();
        }
        final int[][] corners = {   // the corner, then the indices of its two edges in safe[]
            { android.view.RoundedCorner.POSITION_TOP_LEFT, 0, 2 },
            { android.view.RoundedCorner.POSITION_TOP_RIGHT, 1, 2 },
            { android.view.RoundedCorner.POSITION_BOTTOM_RIGHT, 1, 3 },
            { android.view.RoundedCorner.POSITION_BOTTOM_LEFT, 0, 3 },
        };
        for (int[] c : corners) {
            android.view.RoundedCorner corner = insets.getRoundedCorner(c[0]);
            if (corner == null)
                continue;
            int in = (int) Math.ceil(corner.getRadius() * (1.0 - Math.sqrt(0.5)));
            safe[c[1]] = Math.max(safe[c[1]], in);
            safe[c[2]] = Math.max(safe[c[2]], in);
        }
        android.util.Log.i("KeeperAndroid", "safe area insets (l,r,t,b): " + safe[0] + "," + safe[1] + "," + safe[2] + "," + safe[3]);
        return safe;
    }
    /** Launcher setting "Black bars at the screen edges" (2026-11-24, a player's phone cut the side panel's corners):
     *  the game's surface keeps clear of the camera cutout's safe insets and, at the ends of the screen's long sides,
     *  of each rounded corner's whole radius, so the picture's corners sit on the curve, not under it. */
    @Override protected int[] safeBarsInsets(WindowInsets insets) {
        if (!getIntent().getBooleanExtra("safe_bars", false))
            return null;
        int[] bars = new int[4];
        android.view.DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout != null) {
            bars[0] = cutout.getSafeInsetLeft();
            bars[1] = cutout.getSafeInsetRight();
            bars[2] = cutout.getSafeInsetTop();
            bars[3] = cutout.getSafeInsetBottom();
        }
        android.graphics.Rect screen = getWindowManager().getCurrentWindowMetrics().getBounds();
        final boolean wide = screen.width() >= screen.height();
        final int[][] corners = {   // the corner, then its edge across the long side: left/right when wide, else top/bottom
            { android.view.RoundedCorner.POSITION_TOP_LEFT, 0, 2 },
            { android.view.RoundedCorner.POSITION_TOP_RIGHT, 1, 2 },
            { android.view.RoundedCorner.POSITION_BOTTOM_RIGHT, 1, 3 },
            { android.view.RoundedCorner.POSITION_BOTTOM_LEFT, 0, 3 },
        };
        for (int[] c : corners) {
            android.view.RoundedCorner corner = insets.getRoundedCorner(c[0]);
            if (corner == null)
                continue;
            int edge = wide ? c[1] : c[2];
            bars[edge] = Math.max(bars[edge], corner.getRadius());
        }
        // never more than a quarter of the screen from one side (a broken report must not squeeze the game away)
        int maxX = screen.width() / 4, maxY = screen.height() / 4;
        bars[0] = Math.min(bars[0], maxX); bars[1] = Math.min(bars[1], maxX);
        bars[2] = Math.min(bars[2], maxY); bars[3] = Math.min(bars[3], maxY);
        return bars;
    }
    @Override protected void onStart() { super.onStart(); if (secondScreen != null) secondScreen.show(); }
    @Override protected void onStop() { if (secondScreen != null) secondScreen.hide(); super.onStop(); }
    @Override protected void onDestroy() {
        if(debugProbe!=null)try{debugProbe.close();}catch(Exception ignored){}
        boolean finishing=isFinishing();
        super.onDestroy();
        // The launcher places this activity in :game. A fresh process is required
        // for the legacy engine globals on the next Play action.
        if(finishing)android.os.Process.killProcess(android.os.Process.myPid());
    }
    @Override public void onBackPressed(){requestMenu(1);}
    // SDL's view consumes KEYCODE_BACK as a key event, so onBackPressed never runs;
    // take Back here before SDL sees it.
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent e){
        if(e.getKeyCode()==android.view.KeyEvent.KEYCODE_BACK){
            if(e.getAction()==android.view.KeyEvent.ACTION_UP&&!e.isCanceled())requestMenu(1);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
}
