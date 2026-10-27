package com.irig.amp;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONObject;

/**
 * iRig Amfi: gitar / saz → iRig → telefonun 3,5 mm kulaklık-mikrofon girişi → efektler → iRig'in kulaklık çıkışı.
 * Arayüz assets/index.html; ses işleme C++ (Oboe) ile yapılır.
 */
public class MainActivity extends Activity {
    private static final int REQ_MIC = 7;
    private WebView web;
    private AudioManager am;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean running = false;
    private volatile boolean startAfterPermission = false;
    private volatile boolean allowNoMic = false;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        am = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        web = new WebView(this);
        web.setBackgroundColor(Color.BLACK);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient());
        web.addJavascriptInterface(new Bridge(), "AMP");
        web.loadUrl("file:///android_asset/index.html");
        setContentView(web);
        am.registerAudioDeviceCallback(deviceCb, ui);
    }

    private final AudioDeviceCallback deviceCb = new AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(AudioDeviceInfo[] added) { js("window.onDevices&&onDevices()"); }
        @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            // Kulaklık çıkarıldıysa sesi hemen kes: hoparlöre geçip geri besleme yapmasın
            if (running && pickOutput() == null) { stopEngine(); js("window.onStopped&&onStopped('Kulaklık / iRig çıkarıldı, ses kapatıldı.')"); }
            js("window.onDevices&&onDevices()");
        }
    };

    private void js(String code) { ui.post(() -> { if (web != null) web.evaluateJavascript(code, null); }); }

    private static boolean isWiredIn(int t) {
        return t == AudioDeviceInfo.TYPE_WIRED_HEADSET || t == AudioDeviceInfo.TYPE_USB_HEADSET || t == AudioDeviceInfo.TYPE_USB_DEVICE;
    }
    private static boolean isWiredOut(int t) {
        return t == AudioDeviceInfo.TYPE_WIRED_HEADSET || t == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
            || t == AudioDeviceInfo.TYPE_USB_HEADSET || t == AudioDeviceInfo.TYPE_USB_DEVICE || t == AudioDeviceInfo.TYPE_LINE_ANALOG;
    }
    private AudioDeviceInfo pickInput() {
        AudioDeviceInfo best = null;
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            if (!isWiredIn(d.getType())) continue;
            if (best == null || d.getType() == AudioDeviceInfo.TYPE_WIRED_HEADSET) best = d;
        }
        return best;
    }
    private AudioDeviceInfo pickOutput() {
        AudioDeviceInfo best = null;
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (!isWiredOut(d.getType())) continue;
            if (best == null || d.getType() == AudioDeviceInfo.TYPE_WIRED_HEADSET) best = d;
        }
        return best;
    }
    private boolean hasBluetoothOut() {
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int t = d.getType();
            if (t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) return true;
        }
        return false;
    }
    private static String typeName(int t) {
        switch (t) {
            case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "Kulaklık girişi (mikrofonlu, iRig)";
            case AudioDeviceInfo.TYPE_WIRED_HEADPHONES: return "Kulaklık (mikrofonsuz)";
            case AudioDeviceInfo.TYPE_USB_HEADSET: return "USB kulaklık / dönüştürücü";
            case AudioDeviceInfo.TYPE_USB_DEVICE: return "USB ses cihazı";
            case AudioDeviceInfo.TYPE_LINE_ANALOG: return "Hat çıkışı";
            default: return "Ses cihazı";
        }
    }

    private String startEngine() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            startAfterPermission = true;
            ui.post(() -> requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC));
            return "perm";
        }
        AudioDeviceInfo out = pickOutput();
        if (out == null) return "noout";               // hoparlörden asla çalma
        AudioDeviceInfo in = pickInput();
        if (in == null && !allowNoMic) return "nomic";
        boolean ok = AmpEngine.nStart(in != null ? in.getId() : 0, out.getId());
        running = ok;
        ui.post(() -> {
            if (ok) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        });
        return ok ? "ok" : "fail";
    }

    private void stopEngine() {
        running = false;
        AmpEngine.nStop();
        ui.post(() -> getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code != REQ_MIC) return;
        boolean g = res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED;
        js("window.onPerm&&onPerm(" + g + ")");
    }

    /** Arka plana geçince mikrofon kesileceği için motoru durdur. */
    @Override
    protected void onStop() {
        super.onStop();
        if (running && !isChangingConfigurations()) { stopEngine(); js("window.onStopped&&onStopped('Uygulama arka plana alındı, ses durduruldu.')"); }
    }

    @Override
    protected void onDestroy() {
        try { am.unregisterAudioDeviceCallback(deviceCb); } catch (Exception ignored) {}
        if (running) stopEngine();
        if (web != null) { web.destroy(); web = null; }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        web.evaluateJavascript("(window.goBack&&goBack())?'1':'0'", v -> { if (v == null || !v.contains("1")) MainActivity.super.onBackPressed(); });
    }

    private class Bridge {
        @JavascriptInterface public String start() { return startEngine(); }
        @JavascriptInterface public void stop() { stopEngine(); }
        @JavascriptInterface public void set(int id, float v) { AmpEngine.nSet(id, v); }
        @JavascriptInterface public void allowNoMic(boolean b) { allowNoMic = b; }
        @JavascriptInterface public float pitch() { return running ? AmpEngine.nPitch() : -1f; }
        @JavascriptInterface public String status() {
            try {
                float[] a = AmpEngine.nStatus();
                running = a[0] > 0.5f;
                JSONObject o = new JSONObject();
                o.put("on", a[0] > 0.5f); o.put("sr", (int) a[1]); o.put("lat", Math.round(a[2]));
                o.put("inPk", a[3]); o.put("outPk", a[4]); o.put("xr", (int) a[5]); o.put("burst", (int) a[6]); o.put("preset", (int) a[7]);
                return o.toString();
            } catch (Exception e) { return "{}"; }
        }
        @JavascriptInterface public String devices() {
            try {
                AudioDeviceInfo in = pickInput(), out = pickOutput();
                JSONObject o = new JSONObject();
                o.put("in", in != null); o.put("inName", in != null ? typeName(in.getType()) : "");
                o.put("out", out != null); o.put("outName", out != null ? typeName(out.getType()) : "");
                o.put("bt", hasBluetoothOut());
                PackageManager pm = getPackageManager();
                o.put("lowLat", pm.hasSystemFeature(PackageManager.FEATURE_AUDIO_LOW_LATENCY));
                o.put("pro", Build.VERSION.SDK_INT >= 23 && pm.hasSystemFeature(PackageManager.FEATURE_AUDIO_PRO));
                o.put("unproc", "true".equals(am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED)));
                o.put("model", Build.MANUFACTURER + " " + Build.MODEL + " · Android " + Build.VERSION.RELEASE);
                o.put("perm", checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED);
                o.put("android", true);
                return o.toString();
            } catch (Exception e) { return "{}"; }
        }
        @JavascriptInterface public boolean pendingStart() { boolean p = startAfterPermission; startAfterPermission = false; return p; }
    }
}
