package org.macnativegames.totk.gameprobe;

import android.app.Dialog;
import android.app.NativeActivity;
import android.os.Bundle;
import android.graphics.Color;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.view.Window;
import android.view.WindowManager;

/** Video sub-screen opened exclusively by the original game's Options row. */
public final class VideoSettingsActivity extends NativeActivity {
    static { System.loadLibrary("totkgame"); }
    private WebView menu;
    private Dialog panel; // own window above the game surface (the game draws into the activity window itself)
    private boolean open, ready, destroyed;
    private long lastDirection;
    private static native void nativeInit(String path);
    private static native String nativeRead();
    private static native boolean nativeSave(int rw,int rh,int ow,int oh,int fps);
    private static native void nativeMenu(boolean open);
    private java.io.File languageFile() {return new java.io.File(getFilesDir(),"language.cfg");}

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        nativeInit(new java.io.File(getFilesDir(),"graphics.cfg").getAbsolutePath());
        getWindow().getDecorView().setSystemUiVisibility(5894);
        menu=new WebView(this);
        menu.setBackgroundColor(Color.TRANSPARENT);
        menu.getSettings().setJavaScriptEnabled(true);
        menu.getSettings().setAllowFileAccess(false);
        menu.getSettings().setAllowContentAccess(false);
        menu.getSettings().setBlockNetworkLoads(true);
        menu.addJavascriptInterface(new Bridge(),"VideoSettings");
        menu.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view,String url) {return true;}
            @Override public void onPageFinished(WebView view,String url) {
                ready=true;if(open)show();
            }
        });
        panel=new Dialog(this,android.R.style.Theme_Black_NoTitleBar_Fullscreen) {
            @Override public boolean dispatchKeyEvent(KeyEvent event) {return handleKey(event);}
            @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {return handleMotion(event);}
        };
        panel.requestWindowFeature(Window.FEATURE_NO_TITLE);
        panel.setContentView(menu,new FrameLayout.LayoutParams(-1,-1));
        panel.setCancelable(false);
        panel.getWindow().setLayout(-1,-1);
        panel.getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        menu.loadUrl("file:///android_asset/video-settings.html");
    }
    /** Called on a wrapped native guest callback; actual UI work stays on UI thread. */
    public void requestVideoSettingsFromGame() {
        runOnUiThread(()->{if(!destroyed)show();});
    }
    private void show() {
        open=true;
        if(!ready)return; // shown (and input blocked) only once the panel is really on screen
        panel.show();panel.getWindow().getDecorView().setSystemUiVisibility(5894);
        nativeMenu(true);menu.requestFocus();
        menu.evaluateJavascript("window.videoOpen && window.videoOpen()",null);
    }
    private void close() {
        open=false;nativeMenu(false);if(panel.isShowing())panel.dismiss();
        getWindow().getDecorView().requestFocus();
    }
    private void key(String key) {if(ready)menu.evaluateJavascript("window.videoKey && window.videoKey('"+key+"')",null);}
    public final class Bridge {
        @JavascriptInterface public String read() {return nativeRead();}
        @JavascriptInterface public boolean save(int rw,int rh,int ow,int oh,int fps) {return nativeSave(rw,rh,ow,oh,fps);}
        /** Text language for the next start (language.cfg, read by the loader); "" follows the system. */
        @JavascriptInterface public String language() {
            try {return new String(java.nio.file.Files.readAllBytes(languageFile().toPath()),"UTF-8").trim();}
            catch(java.io.IOException e) {return "";}
        }
        @JavascriptInterface public boolean setLanguage(String code) {
            if(code==null||!code.matches("|[a-z]{2}(-[A-Za-z0-9]{2,4})?"))return false;
            java.io.File file=languageFile(),temp=new java.io.File(file.getPath()+".tmp");
            try(java.io.FileOutputStream out=new java.io.FileOutputStream(temp)) {
                out.write(code.getBytes("UTF-8"));out.getFD().sync();
            } catch(java.io.IOException e) {temp.delete();return false;}
            return temp.renameTo(file);
        }
        @JavascriptInterface public void close() {runOnUiThread(()->{if(!destroyed)VideoSettingsActivity.this.close();});}
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if(!open)return super.dispatchKeyEvent(event); // Start/+ keeps its original function.
        return handleKey(event);
    }
    private boolean handleKey(KeyEvent event) {
        if(event.getAction()==KeyEvent.ACTION_DOWN) {
            switch(event.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_UP:key("up");break;
                case KeyEvent.KEYCODE_DPAD_DOWN:key("down");break;
                case KeyEvent.KEYCODE_DPAD_LEFT:key("left");break;
                case KeyEvent.KEYCODE_DPAD_RIGHT:key("right");break;
                case KeyEvent.KEYCODE_BUTTON_A:case KeyEvent.KEYCODE_DPAD_CENTER:case KeyEvent.KEYCODE_ENTER:key("accept");break;
                case KeyEvent.KEYCODE_BUTTON_B:case KeyEvent.KEYCODE_BACK:case KeyEvent.KEYCODE_ESCAPE:key("back");break;
                default:break;
            }
        }
        return true;
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if(!open)return super.dispatchGenericMotionEvent(event);
        return handleMotion(event);
    }
    private boolean handleMotion(MotionEvent event) {
        float x=event.getAxisValue(MotionEvent.AXIS_HAT_X),y=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if(Math.abs(x)<.5f)x=event.getAxisValue(MotionEvent.AXIS_X);
        if(Math.abs(y)<.5f)y=event.getAxisValue(MotionEvent.AXIS_Y);
        long now=android.os.SystemClock.uptimeMillis();
        if(now-lastDirection>180) {
            if(Math.abs(y)>.6f){key(y>0?"down":"up");lastDirection=now;}
            else if(Math.abs(x)>.6f){key(x>0?"right":"left");lastDirection=now;}
        }
        return true;
    }
    @Override protected void onDestroy() {
        destroyed=true;nativeMenu(false);
        if(panel!=null&&panel.isShowing())panel.dismiss();
        if(menu!=null){menu.removeJavascriptInterface("VideoSettings");menu.destroy();}
        super.onDestroy();
    }
}
