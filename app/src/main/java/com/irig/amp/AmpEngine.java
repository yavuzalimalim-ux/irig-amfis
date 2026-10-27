package com.irig.amp;

/** C++ ses motoruna (Oboe) köprü. */
public final class AmpEngine {
    static { System.loadLibrary("irigamp"); }
    private AmpEngine() {}
    public static native boolean nStart(int inputDeviceId, int outputDeviceId);
    public static native void nStop();
    public static native void nSet(int id, float value);
    /** [çalışıyor, örnekleme, gecikme ms, giriş tepe, çıkış tepe, takılma, burst, giriş ön ayarı] */
    public static native float[] nStatus();
    public static native float nPitch();
}
