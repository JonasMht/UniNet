package org.uninet.demo;

/** The native side of the demo. See jni/uninet_jni.cpp. */
public final class UniNet {
    static {
        // Order matters: the JNI shim links against the C ABI, so the C ABI has
        // to be in the process first. Android's loader will not find it on its
        // own for a library loaded this way.
        System.loadLibrary("uninet_c");
        System.loadLibrary("uninet_jni");
    }

    private UniNet() {}

    /**
     * Join the network. Returns "" on success or the reason it failed.
     *
     * @param gossip   "" for ordinary Wi-Fi discovery; a rendezvous endpoint
     *                 such as "tcp://127.0.0.1:31337" for the USB path, where
     *                 there is no multicast to beacon over.
     * @param endpoint this device's own data endpoint, needed in gossip mode.
     */
    public static native String nativeJoin(String name, String gossip, String endpoint);

    public static native String  nativeDescribe();
    public static native int     nativePeerCount();
    public static native boolean nativePublish(String text);
    /** Log lines produced since the last call, oldest first. */
    public static native String[] nativeDrain();
    public static native void    nativeLeave();
    public static native String  nativeVersion();
    public static native boolean nativeHasLz4();
}
