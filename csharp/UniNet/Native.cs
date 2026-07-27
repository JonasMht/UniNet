// UniNet: raw P/Invoke declarations. Nothing here is meant to be called by an
// application; use Session (Session.cs), which owns the lifetimes and the
// threading. This file exists so every marshalling decision sits in one place.
//
// Two rules govern everything below:
//
//  1. **UTF-8, never ANSI.** The C ABI is byte-transparent UTF-8. .NET's default
//     string marshalling is ANSI, which on Windows means the active code page -
//     so a device named "Röntgen" round-tripped as mojibake. Every string in and
//     out is explicitly LPUTF8Str / PtrToStringUTF8.
//
//  2. **Delegates must be rooted.** A delegate handed to native code is not kept
//     alive by the native pointer holding it. If the GC collects it while
//     UniNet's network thread still has that pointer, the next message is a hard
//     crash. Session keeps every delegate in a field for as long as the native
//     handle lives.
using System;
using System.Runtime.InteropServices;

namespace UniNet
{
    /// <summary>Status codes from the C ABI. 0 is success; every error is negative.</summary>
    public static class Status
    {
        public const int Ok       = 0;
        public const int Arg      = -1;
        public const int State    = -2;
        public const int Parse    = -3;
        public const int Buffer   = -4;
        public const int Internal = -5;
    }

    internal static class Native
    {
        // The library name without prefix or extension: .NET appends
        // "lib"/".so"/".dll"/".dylib" per platform. In Unity, put the built
        // library in Assets/Plugins/<platform>/.
        internal const string Lib = "uninet_c";

        // ── callbacks ──
        // Invoked from UniNet's network thread. See Session for the main-thread
        // marshalling that makes them safe to use from Unity.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void JsonCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string srcUuid,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string json,
            IntPtr user);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void CborCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string srcUuid,
            IntPtr data, UIntPtr len, IntPtr user);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void PeerCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string uuid,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string address,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string role,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string app,
            IntPtr user);

        // ── session ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_session_join(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string role,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string app,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string realm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string iface,
            int port);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_session_join_ex(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string role,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string app,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string realm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string iface,
            int port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string gossipBind,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string gossipConnect,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string endpoint,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string advertisedEndpoint);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void uninet_session_free(IntPtr session);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_connected(IntPtr session);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_uuid(IntPtr session, byte[] buf, UIntPtr buflen);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_describe(IntPtr session, byte[] buf, UIntPtr buflen);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_publish_json(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string json,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dst);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_publish_cbor(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            byte[] cbor, UIntPtr len,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dst);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_subscribe_json(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            JsonCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_subscribe_cbor(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string subject,
            CborCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_on_peer_found(IntPtr session, PeerCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_on_peer_lost(IntPtr session, PeerCallback cb, IntPtr user);

        // ── configuration ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_config_new();
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void uninet_config_free(IntPtr cfg);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_header(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_compression(IntPtr cfg, int compression);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_realm(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string realm);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_role(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string role);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_app(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string app);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_interface(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string iface);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_port(IntPtr cfg, int port);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_config_set_gossip(IntPtr cfg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string bind,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string connect,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string endpoint,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string advertised);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_session_join_cfg(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name, IntPtr cfg);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_peers_has_header(IntPtr peers, int index,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key);

        // ── session lifetime ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void uninet_session_close(IntPtr session);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_session_open(IntPtr session);

        // ── large payloads ──
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void BlobCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string src,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string metaJson,
            IntPtr data, UIntPtr len, IntPtr user);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void BlobProgressCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            UIntPtr done, UIntPtr total, IntPtr user);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void BlobFailedCallback(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string reason, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_blob_new(
            IntPtr session, [MarshalAs(UnmanagedType.LPUTF8Str)] string subject);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void uninet_blob_free(IntPtr blob);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_send(
            IntPtr blob, [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            byte[] data, UIntPtr len,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string metaJson,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dst,
            byte[] idBuf, UIntPtr idBufLen);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_send_file(
            IntPtr blob, [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string metaJson,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dst,
            byte[] idBuf, UIntPtr idBufLen);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_on_received(IntPtr blob, BlobCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_on_progress(IntPtr blob, BlobProgressCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_on_failed(IntPtr blob, BlobFailedCallback cb, IntPtr user);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_blob_incoming_count(IntPtr blob);

        // ── peers ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_session_peers(IntPtr session);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_peers_count(IntPtr peers);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_uuid(IntPtr peers, int index);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_name(IntPtr peers, int index);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_address(IntPtr peers, int index);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_role(IntPtr peers, int index);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_app(IntPtr peers, int index);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_peers_header(
            IntPtr peers, int index,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void uninet_peers_free(IntPtr peers);

        // ── conversion ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_json_to_cbor(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string json,
            byte[]? outBuf, UIntPtr outLen, out UIntPtr written);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_cbor_to_json(byte[] cbor, UIntPtr len,
                                                       byte[] outBuf, UIntPtr outLen);

        // ── diagnostics ──
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_last_error();

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern ushort uninet_protocol_version();

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int uninet_has_lz4();

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr uninet_version();

        // ── helpers ──
        /// <summary>A NUL-terminated UTF-8 pointer from native code, as a string.</summary>
        internal static string Str(IntPtr p) =>
            p == IntPtr.Zero ? string.Empty : (Marshal.PtrToStringUTF8(p) ?? string.Empty);

        /// <summary>Why the last call on this thread failed.</summary>
        internal static string LastError() => Str(uninet_last_error());

        /// <summary>
        /// Read a caller-buffer out-parameter, growing until it fits.
        /// Throws on a real error rather than returning an empty string.
        /// </summary>
        /// <remarks>
        /// The previous version retried once with a fixed 4096-byte buffer. When
        /// the caller's first guess was already larger than that, the retry
        /// SHRANK the buffer and failed again, and every failure was mapped to
        /// "" — so any JSON over about 4 KB came back as an empty string with no
        /// error. Doubling until it fits removes both halves of that bug.
        /// </remarks>
        internal static string ReadBuffer(Func<byte[], UIntPtr, int> call, int initial = 256)
        {
            int size = Math.Max(initial, 64);
            for (int attempt = 0; attempt < 24; ++attempt)   // 64 B up to ~1 GB
            {
                var buf = new byte[size];
                int n = call(buf, (UIntPtr)buf.Length);
                if (n >= 0) return System.Text.Encoding.UTF8.GetString(buf, 0, n);
                if (n != Status.Buffer)
                    throw new InvalidOperationException("UniNet: " + LastError());
                size *= 2;
            }
            throw new InvalidOperationException(
                "UniNet: the value did not fit in any reasonable buffer: " + LastError());
        }
    }
}
