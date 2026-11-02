package org.macnativegames.totk.vulkanprobe;

import android.app.NativeActivity;
import android.os.Bundle;
import android.graphics.Color;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Gravity;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;

/** Local settings overlay over the native Vulkan surface. No remote web content. */
public final class ThorActivity extends NativeActivity {
    static { System.loadLibrary("thorvulkan"); }
    private WebView menu;
    private Button openButton;
    private boolean menuOpen;
    private long lastDirection;
    private static native void nativeInit(String path);
    private static native String nativeRead();
    private static native boolean nativeSave(int rw, int rh, int ow, int oh);
    private static native void nativeMenu(boolean open);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        nativeInit(new java.io.File(getFilesDir(), "graphics.cfg").getAbsolutePath());
        getWindow().getDecorView().setSystemUiVisibility(5894); // immersive landscape
        FrameLayout overlay = new FrameLayout(this);
        openButton = new Button(this);
        openButton.setText("Beeld  ☷");
        openButton.setFocusable(false);
        openButton.setTextColor(0xffeee7ce);
        openButton.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xe61b322f));
        openButton.setContentDescription("Grafische instellingen openen");
        FrameLayout.LayoutParams buttonParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.RIGHT);
        buttonParams.setMargins(16, 12, 20, 0);
        overlay.addView(openButton, buttonParams);
        menu = new WebView(this);
        menu.setBackgroundColor(Color.TRANSPARENT);
        menu.getSettings().setJavaScriptEnabled(true);
        menu.getSettings().setAllowFileAccess(false);
        menu.getSettings().setAllowContentAccess(false);
        menu.getSettings().setBlockNetworkLoads(true);
        menu.addJavascriptInterface(new Bridge(), "ThorSettings");
        menu.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, String url) { return true; }
        });
        overlay.addView(menu, new FrameLayout.LayoutParams(-1, -1));
        addContentView(overlay, new android.view.ViewGroup.LayoutParams(-1, -1));
        menu.setVisibility(View.GONE);
        menu.loadUrl("file:///android_asset/settings.html");
        openButton.setOnClickListener(v -> showMenu());
    }
    public void requestMenuFromNative() { runOnUiThread(() -> showMenu()); }
    private void showMenu() {
        menuOpen = true; nativeMenu(true);
        openButton.setVisibility(View.GONE); menu.setVisibility(View.VISIBLE); menu.requestFocus();
        menu.evaluateJavascript("window.thorOpen && window.thorOpen()", null);
    }
    private void hideMenu() {
        menuOpen = false; nativeMenu(false); menu.setVisibility(View.GONE);
        openButton.setVisibility(View.VISIBLE); getWindow().getDecorView().requestFocus();
    }
    private void key(String key) {
        menu.evaluateJavascript("window.thorKey && window.thorKey('" + key + "')", null);
    }
    public final class Bridge {
        @JavascriptInterface public String read() { return nativeRead(); }
        @JavascriptInterface public boolean save(int rw, int rh, int ow, int oh) { return nativeSave(rw, rh, ow, oh); }
        @JavascriptInterface public void close() { runOnUiThread(() -> hideMenu()); }
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        boolean toggle = code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_MENU;
        if (!menuOpen && toggle) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) showMenu();
            return true;
        }
        if (menuOpen) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                switch (code) {
                    case KeyEvent.KEYCODE_DPAD_UP: key("up"); break;
                    case KeyEvent.KEYCODE_DPAD_DOWN: key("down"); break;
                    case KeyEvent.KEYCODE_DPAD_LEFT: key("left"); break;
                    case KeyEvent.KEYCODE_DPAD_RIGHT: key("right"); break;
                    case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_DPAD_CENTER:
                    case KeyEvent.KEYCODE_ENTER: key("accept"); break;
                    case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BACK:
                    case KeyEvent.KEYCODE_ESCAPE: case KeyEvent.KEYCODE_BUTTON_START: key("back"); break;
                    default: break;
                }
            }
            // Menu input must never reach the native gamepad state.
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (!menuOpen) return super.dispatchGenericMotionEvent(event);
        float x = event.getAxisValue(MotionEvent.AXIS_HAT_X), y = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (Math.abs(x) < 0.5f) x = event.getAxisValue(MotionEvent.AXIS_X);
        if (Math.abs(y) < 0.5f) y = event.getAxisValue(MotionEvent.AXIS_Y);
        long now = android.os.SystemClock.uptimeMillis();
        if (now - lastDirection > 180) {
            if (Math.abs(x) > 0.6f) { key(x > 0 ? "right" : "left"); lastDirection = now; }
            else if (Math.abs(y) > 0.6f) { key(y > 0 ? "down" : "up"); lastDirection = now; }
        }
        return true;
    }
    @Override protected void onDestroy() {
        nativeMenu(false);
        if (menu != null) { menu.removeJavascriptInterface("ThorSettings"); menu.destroy(); }
        super.onDestroy();
    }
}
