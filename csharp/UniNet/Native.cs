// UniNet — native interop. P/Invoke into libuninet_c (the C ABI of the compiled
// C++ core). The native library resolves to libuninet_c.so / uninet_c.dll /
// libuninet_c.dylib; ship the matching build next to your app (or into your Unity
// Plugins/ folder for HoloLens/ARM64).
using System;
using System.Runtime.InteropServices;

namespace UniNet
{
    public enum Compression : int { None = 0, Zlib = 1, Lz4 = 2 }

    internal static class Native
    {
        private const string Lib = "uninet_c";

        // ── loopback ──
        [DllImport(Lib)] public static extern IntPtr uninet_loopback_new();
        [DllImport(Lib)] public static extern void uninet_loopback_free(IntPtr bus);
        [DllImport(Lib)] public static extern int uninet_loopback_connect(IntPtr bus);

        // ── NATS ──
        [DllImport(Lib)] public static extern IntPtr uninet_nats_new(string url);
        [DllImport(Lib)] public static extern void uninet_nats_free(IntPtr nats);
        [DllImport(Lib)] public static extern int uninet_nats_connect(IntPtr nats);

        // ── node (over either transport) ──
        [DllImport(Lib)] public static extern IntPtr uninet_node_new(string name, IntPtr loopbackBus, int compression);
        [DllImport(Lib)] public static extern IntPtr uninet_node_new_nats(string name, IntPtr natsBus, int compression);
        [DllImport(Lib)] public static extern void uninet_node_free(IntPtr node);
        [DllImport(Lib)] public static extern int uninet_node_connect(IntPtr node);
        [DllImport(Lib, EntryPoint = "uninet_node_uuid")] public static extern IntPtr uninet_node_uuid_raw(IntPtr node);

        [DllImport(Lib)] public static extern int uninet_node_publish_text(IntPtr node, string subject, string dstUuid, string text);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void TextCallback(string subject, string text, IntPtr user);
        [DllImport(Lib)] public static extern int uninet_node_subscribe_text(IntPtr node, string subject, TextCallback cb, IntPtr user);

        // raw-CBOR exchange (the envelope `data` field, as CBOR bytes)
        [DllImport(Lib)] public static extern int uninet_node_publish_cbor(IntPtr node, string subject,
            string dstUuid, byte[] cbor, UIntPtr len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void CborCallback(IntPtr subject, IntPtr src, IntPtr data, UIntPtr len, IntPtr user);
        [DllImport(Lib)] public static extern int uninet_node_subscribe_cbor(IntPtr node, string subject, CborCallback cb, IntPtr user);

        [DllImport(Lib)] public static extern ushort uninet_protocol_version();
        [DllImport(Lib)] public static extern int uninet_has_lz4();
        [DllImport(Lib)] public static extern int uninet_has_nats();
    }

    public static class Build
    {
        public static int ProtocolVersion => Native.uninet_protocol_version();
        public static bool HasLz4 => Native.uninet_has_lz4() != 0;
        public static bool HasNats => Native.uninet_has_nats() != 0;
    }
}
