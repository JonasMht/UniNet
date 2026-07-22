// UniNet — native interop. P/Invoke into libuninet_c (the C ABI of the compiled
// C++ core). The native library resolves to libuninet_c.so / uninet_c.dll /
// libuninet_c.dylib depending on platform; ship the matching build next to your app
// (or into your Unity Plugins/ folder for HoloLens/ARM64).
//
// v0.1 surface: loopback bus + Node at the text-payload level (enough for end-to-end
// pub/sub from C# without a managed CBOR library). Raw-CBOR and NATS variants staged.
using System;
using System.Runtime.InteropServices;

namespace UniNet
{
    public enum Compression : int { None = 0, Zlib = 1, Lz4 = 2 }

    internal static class Native
    {
        private const string Lib = "uninet_c";

        [DllImport(Lib)] public static extern IntPtr uninet_loopback_new();
        [DllImport(Lib)] public static extern void uninet_loopback_free(IntPtr bus);
        [DllImport(Lib)] public static extern int uninet_loopback_connect(IntPtr bus);

        [DllImport(Lib)] public static extern IntPtr uninet_node_new(string name, IntPtr bus, int compression);
        [DllImport(Lib)] public static extern void uninet_node_free(IntPtr node);
        [DllImport(Lib)] public static extern int uninet_node_connect(IntPtr node);
        [DllImport(Lib, EntryPoint = "uninet_node_uuid")] public static extern IntPtr uninet_node_uuid_raw(IntPtr node);
        [DllImport(Lib)] public static extern int uninet_node_publish_text(IntPtr node, string subject, string dstUuid, string text);

        public delegate void TextCallback(string subject, string text, IntPtr user);
        [DllImport(Lib)] public static extern int uninet_node_subscribe_text(IntPtr node, string subject, TextCallback cb, IntPtr user);

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
