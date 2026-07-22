// UniNet — in-process transport + high-level Node (C# over the native C ABI).
using System;
using System.Runtime.InteropServices;

namespace UniNet
{
    /// <summary>In-process bus. Two Nodes sharing one LoopbackTransport talk to
    /// each other. The always-available backend (no broker, no network).</summary>
    public sealed class LoopbackTransport : IDisposable
    {
        internal IntPtr Handle { get; private set; }

        public LoopbackTransport()
        {
            Handle = Native.uninet_loopback_new();
            if (Handle == IntPtr.Zero) throw new InvalidOperationException("uninet_loopback_new failed");
            Native.uninet_loopback_connect(Handle);
        }

        public void Dispose()
        {
            if (Handle != IntPtr.Zero) { Native.uninet_loopback_free(Handle); Handle = IntPtr.Zero; }
            GC.SuppressFinalize(this);
        }
        ~LoopbackTransport() => Dispose();
    }

    /// <summary>A peer on a transport. Publishes/subscribes envelopes; suppresses
    /// its own echo and honors dst-UUID targeting (the protocol logic the three
    /// ThermoNav peers each re-implement, written once in the C++ core).</summary>
    public sealed class Node : IDisposable
    {
        private IntPtr _handle;
        // Pin the callback delegates for the lifetime of the node so the native
        // side never calls into a moved/freed thunk.
        private System.Collections.Generic.List<Native.TextCallback> _keepalive =
            new System.Collections.Generic.List<Native.TextCallback>();

        public string Uuid { get; }

        public Node(string name, LoopbackTransport bus, Compression compression = Compression.None)
        {
            if (bus == null) throw new ArgumentNullException(nameof(bus));
            _handle = Native.uninet_node_new(name ?? "csnode", bus.Handle, (int)compression);
            if (_handle == IntPtr.Zero) throw new InvalidOperationException("uninet_node_new failed");
            Native.uninet_node_connect(_handle);
            Uuid = Marshal.PtrToStringAnsi(Native.uninet_node_uuid_raw(_handle)) ?? "";
        }

        public void Publish(string subject, string text, string dstUuid = "")
            => Native.uninet_node_publish_text(_handle, subject ?? "", dstUuid ?? "", text ?? "");

        public void Subscribe(string subject, Action<string, string> handler)
        {
            Native.TextCallback cb = (subj, text, _) => handler(subj, text);
            _keepalive.Add(cb);
            Native.uninet_node_subscribe_text(_handle, subject ?? ">", cb, IntPtr.Zero);
        }

        public void Dispose()
        {
            _keepalive.Clear();
            if (_handle != IntPtr.Zero) { Native.uninet_node_free(_handle); _handle = IntPtr.Zero; }
            GC.SuppressFinalize(this);
        }
        ~Node() => Dispose();
    }
}
