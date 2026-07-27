// Android Wi-Fi multicast lock: required for UniNet LAN peer discovery.
//
// WHY: Android's Wi-Fi driver filters out multicast and subnet-broadcast frames
// before they ever reach userspace, to save power. An app only sees them while it
// holds a WifiManager.MulticastLock (which in turn needs the manifest permission
// android.permission.CHANGE_WIFI_MULTICAST_STATE: see
// Assets/Plugins/Android/AndroidManifest.xml). Unicast is unaffected, so the NATS
// connection to the broker works with or without this.
//
// WHAT BREAKS WITHOUT IT: UniNet's Zyre/ZRE discovery beacon is a UDP broadcast on
// port 5670. On the headset the beacons are silently dropped on the way in, so the
// MR sees no peers, and, because it never answers a beacon, no peer sees the MR
// either. There is no error anywhere: discovery just "mysteriously finds nothing",
// which is exactly the symptom this class exists to prevent. Hence the warnings
// below: if the lock cannot be taken we want it in the log, not in silence.
//
// This is a no-op in the Editor and on desktop, where the OS does no such filtering.
using UnityEngine;
using System;

public static class UniNetMulticastLock
{
    // Reference counted so several subsystems can ask for the lock independently;
    // the Android lock itself is created once and set to non-reference-counted
    // (setReferenceCounted(false)), so acquire/release on it are idempotent.
    private static int _refCount;
    private static readonly object _mutex = new object();

#if UNITY_ANDROID && !UNITY_EDITOR
    // Shows up in `adb shell dumpsys wifi`: keep it recognisable.
    private const string LOCK_TAG = "UniNetZreDiscovery";
    private static AndroidJavaObject _lock;
#endif

    // True when this app is (believed to be) holding the multicast lock.
    public static bool IsHeld
    {
        get { lock (_mutex) { return _refCount > 0; } }
    }

    // Take the lock. Safe to call repeatedly and from any platform.
    public static void Acquire()
    {
        lock (_mutex)
        {
            _refCount++;
            if (_refCount > 1) return;   // already held: nothing to do
#if UNITY_ANDROID && !UNITY_EDITOR
            AcquireNative();
#endif
        }
    }

    // Give the lock back. Balanced against Acquire(); the last release wins.
    public static void Release()
    {
        lock (_mutex)
        {
            if (_refCount == 0) return;  // never acquired (or acquire failed)
            _refCount--;
            if (_refCount > 0) return;
#if UNITY_ANDROID && !UNITY_EDITOR
            ReleaseNative();
#endif
        }
    }

#if UNITY_ANDROID && !UNITY_EDITOR
    private static void AcquireNative()
    {
        try
        {
            // Go through the APPLICATION context, not the Activity: a WifiManager
            // obtained from an Activity keeps a reference to it and leaks it.
            using (var player = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
            using (var activity = player.GetStatic<AndroidJavaObject>("currentActivity"))
            using (var appContext = activity.Call<AndroidJavaObject>("getApplicationContext"))
            using (var wifi = appContext.Call<AndroidJavaObject>("getSystemService", "wifi"))
            {
                if (wifi == null)
                {
                    _refCount = 0;
                    Debug.LogWarning("[UniNet] no WifiManager (getSystemService(\"wifi\") returned null): " +
                                     "multicast lock NOT held, LAN peer discovery will find nothing.");
                    return;
                }

                _lock = wifi.Call<AndroidJavaObject>("createMulticastLock", LOCK_TAG);
                if (_lock == null)
                {
                    _refCount = 0;
                    Debug.LogWarning("[UniNet] createMulticastLock returned null: multicast lock NOT held, " +
                                     "LAN peer discovery will find nothing.");
                    return;
                }

                _lock.Call("setReferenceCounted", false);
                _lock.Call("acquire");
                Debug.Log("[UniNet] Wi-Fi multicast lock acquired (ZRE beacons on udp/5670 will now be delivered).");
            }
        }
        catch (Exception e)
        {
            // Almost always a missing CHANGE_WIFI_MULTICAST_STATE permission in the
            // shipped manifest (SecurityException), so say so explicitly.
            _refCount = 0;
            if (_lock != null) { try { _lock.Dispose(); } catch { } _lock = null; }
            Debug.LogWarning("[UniNet] could not acquire the Wi-Fi multicast lock: LAN peer discovery " +
                             "will receive no beacons. Check that android.permission.CHANGE_WIFI_MULTICAST_STATE " +
                             $"is in the shipped manifest. Cause: {e}");
        }
    }

    private static void ReleaseNative()
    {
        try
        {
            if (_lock != null)
            {
                if (_lock.Call<bool>("isHeld")) _lock.Call("release");
                _lock.Dispose();
                Debug.Log("[UniNet] Wi-Fi multicast lock released.");
            }
        }
        catch (Exception e)
        {
            Debug.LogWarning($"[UniNet] error releasing the Wi-Fi multicast lock: {e}");
        }
        finally
        {
            _lock = null;
        }
    }
#endif
}
