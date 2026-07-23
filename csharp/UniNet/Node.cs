// UniNet — transports + high-level Node (C# over the native C ABI).
using System;
using System.Collections.Generic;
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
        public void Dispose() { if (Handle != IntPtr.Zero) { Native.uninet_loopback_free(Handle); Handle = IntPtr.Zero; } GC.SuppressFinalize(this); }
        ~LoopbackTransport() => Dispose();
    }

    /// <summary>The production brokered backend. Connect to nats://host:port.
    /// Requires a NATS-enabled libuninet_c (Build.HasNats).</summary>
    public sealed class NatsTransport : IDisposable
    {
        internal IntPtr Handle { get; private set; }
        public bool Connected { get; private set; }

        public NatsTransport(string url = "nats://127.0.0.1:4222")
        {
            Handle = Native.uninet_nats_new(url);
            if (Handle == IntPtr.Zero) throw new InvalidOperationException("uninet_nats_new failed");
        }
        public bool Connect() => Connected = Native.uninet_nats_connect(Handle) != 0;
        public void Dispose() { if (Handle != IntPtr.Zero) { Native.uninet_nats_free(Handle); Handle = IntPtr.Zero; } GC.SuppressFinalize(this); }
        ~NatsTransport() => Dispose();
    }

    /// <summary>A peer on a transport. Publishes/subscribes envelopes; suppresses
    /// its own echo and honors dst-UUID targeting (done once in the C++ core).</summary>
    public sealed class Node : IDisposable
    {
        private IntPtr _handle;
        // Pin callback delegates for the node's lifetime so the native side never
        // calls into a moved/freed thunk.
        private readonly List<object> _pins = new List<object>();

        public string Uuid { get; }

        public Node(string name, LoopbackTransport bus, Compression compression = Compression.Lz4)
            : this(name, bus?.Handle ?? IntPtr.Zero, compression, nats: false) { }
        public Node(string name, NatsTransport bus, Compression compression = Compression.Lz4)
            : this(name, bus?.Handle ?? IntPtr.Zero, compression, nats: true) { }

        private Node(string name, IntPtr bus, Compression compression, bool nats)
        {
            _handle = nats ? Native.uninet_node_new_nats(name ?? "csnode", bus, (int)compression)
                           : Native.uninet_node_new(name ?? "csnode", bus, (int)compression);
            if (_handle == IntPtr.Zero) throw new InvalidOperationException("uninet_node_new failed");
            Native.uninet_node_connect(_handle);
            Uuid = Marshal.PtrToStringAnsi(Native.uninet_node_uuid_raw(_handle)) ?? "";
        }

        public void Dispose() { _pins.Clear(); if (_handle != IntPtr.Zero) { Native.uninet_node_free(_handle); _handle = IntPtr.Zero; } GC.SuppressFinalize(this); }
        ~Node() => Dispose();

        // ── text payload ──
        public void Publish(string subject, string text, string dstUuid = "")
            => Native.uninet_node_publish_text(_handle, subject ?? "", dstUuid ?? "", text ?? "");
        public void Subscribe(string subject, Action<string, string> handler)
        {
            var cb = new Native.TextCallback((subj, text, _) => handler(subj, text));
            _pins.Add(cb);
            Native.uninet_node_subscribe_text(_handle, subject ?? ">", cb, IntPtr.Zero);
        }

        // ── raw-CBOR payload: exchange the envelope `data` field as CBOR bytes.
        //    Lets a binding with its own CBOR lib (PeterO.Cbor on the MR headset)
        //    keep using it end-to-end. The callback's bytes are valid only during
        //    the invocation — copied here.
        public void PublishCbor(string subject, byte[] cborData, string dstUuid = "")
            => Native.uninet_node_publish_cbor(_handle, subject ?? "", dstUuid ?? "", cborData ?? Array.Empty<byte>(),
                                               (UIntPtr)(cborData?.Length ?? 0));
        public void SubscribeCbor(string subject, Action<string, string, byte[]> handler)
        {
            var cb = new Native.CborCallback((subjPtr, srcPtr, dataPtr, len, _) => {
                int n = (int)(uint)len;
                byte[] copy = new byte[n];
                if (n > 0) Marshal.Copy(dataPtr, copy, 0, n);
                string subj = Marshal.PtrToStringAnsi(subjPtr) ?? "";
                string src = Marshal.PtrToStringAnsi(srcPtr) ?? "";
                handler(subj, src, copy);
            });
            _pins.Add(cb);
            Native.uninet_node_subscribe_cbor(_handle, subject ?? ">", cb, IntPtr.Zero);
        }
    }
}
