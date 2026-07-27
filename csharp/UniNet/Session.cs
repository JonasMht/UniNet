// UniNet: the C# API. One call joins the network:
//
//     using var net = UniNet.Session.Join("MR Viewer", role: "headset");
//     net.PeerFound += p => Debug.Log($"found {p.Name} at {p.Address}");
//     net.Subscribe("domain.>", msg => Debug.Log(msg.Json));
//     net.Publish("domain.D1", "{\"code\":\"update\"}");
//
// No address, no port, no broker, no configuration file.
//
// ── THREADING, and why Unity needs the pump ───────────────────────────────
// Messages and presence events arrive on UniNet's network thread. Touching the
// Unity API from there throws or crashes the player, so by default this class
// queues every event and hands it to you when you call Update(), which you do
// from MonoBehaviour.Update(), on the main thread:
//
//     void Update() => net.Update();
//
// Pass marshalToCaller: false to opt out and receive events directly on the
// network thread (correct for a console app or a background service, never for
// Unity).
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace UniNet
{
    /// <summary>A device on the network, discovered rather than configured.</summary>
    public sealed class Peer
    {
        public string Uuid { get; }
        public string Name { get; }
        /// <summary>Observed endpoint, e.g. "tcp://192.168.1.31:35001".</summary>
        public string Address { get; }
        public string Role { get; }
        public string App { get; }

        internal Peer(string uuid, string name, string address, string role, string app)
        {
            Uuid = uuid; Name = name; Address = address; Role = role; App = app;
        }

        /// <summary>Just the host part of Address, for a device list.</summary>
        public string Host
        {
            get
            {
                var a = Address;
                int scheme = a.IndexOf("://", StringComparison.Ordinal);
                if (scheme >= 0) a = a.Substring(scheme + 3);
                int colon = a.LastIndexOf(':');
                return colon >= 0 ? a.Substring(0, colon) : a;
            }
        }

        public override string ToString() =>
            string.IsNullOrEmpty(Role) ? $"{Name} ({Host})" : $"{Name} ({Host}): {Role}";
    }

    /// <summary>One received message.</summary>
    public sealed class Message
    {
        public string Subject { get; }
        /// <summary>uuid of the sender: pass it as `dst` to reply privately.</summary>
        public string Src { get; }
        /// <summary>Payload as JSON text.</summary>
        public string Json { get; }

        internal Message(string subject, string src, string json)
        {
            Subject = subject; Src = src; Json = json;
        }

        public override string ToString() => $"{Subject}: {Json}";
    }

    public sealed class Session : IDisposable
    {
        private IntPtr _handle;
        private readonly bool _marshalToCaller;

        // Every delegate handed to native code is rooted here for the lifetime of
        // the native handle. Without this the GC is free to collect them while
        // the network thread still holds the pointers.
        private readonly List<Delegate> _rooted = new List<Delegate>();
        private readonly List<Action<Message>> _handlersKeepAlive = new List<Action<Message>>();

        // Events queued off the network thread, drained by Update().
        private readonly ConcurrentQueue<Action> _pending = new ConcurrentQueue<Action>();

        /// <summary>The native handle, for companion types such as Blob.</summary>
        internal IntPtr Handle => _handle;

        /// <summary>A device appeared. Also fires for devices already present.</summary>
        public event Action<Peer>? PeerFound;
        /// <summary>A device left the network.</summary>
        public event Action<Peer>? PeerLost;

        // .NET (Core and later) does not run finalizers at process exit, so
        // ~Session is dead code for the common case and an undisposed session
        // never announced its departure: peers waited out the full 30 s
        // expiry instead of seeing it leave at once. Measured: 0.0 s when
        // disposed, 29.1 s when leaked. Python solves this the same way.
        private static readonly List<WeakReference<Session>> _live =
            new List<WeakReference<Session>>();

        static Session()
        {
            AppDomain.CurrentDomain.ProcessExit += (_, __) => CloseAll();
        }

        private static void CloseAll()
        {
            lock (_live)
            {
                foreach (var weak in _live)
                    if (weak.TryGetTarget(out var s))
                    {
                        try { s.Close(); } catch { /* exiting; nothing useful to do */ }
                    }
                _live.Clear();
            }
        }

        private Session(IntPtr handle, bool marshalToCaller)
        {
            _handle = handle;
            _marshalToCaller = marshalToCaller;
            lock (_live) _live.Add(new WeakReference<Session>(this));
        }

        /// <summary>
        /// Join the network under <paramref name="name"/>. This is the whole setup.
        /// </summary>
        /// <param name="name">What other devices show for this one, e.g. "Headset".</param>
        /// <param name="role">Free-form label: "server", "headset", "viewer".</param>
        /// <param name="app">Owning application, for a device list.</param>
        /// <param name="realm">Devices only see devices in the same realm. Use it to
        /// keep a development machine out of a live session.</param>
        /// <param name="iface">Only needed on a machine with several networks
        /// ("eth0" or an IP), where discovery could otherwise pick the wrong one.</param>
        /// <param name="port">UDP discovery port; 0 keeps the default (5670).</param>
        /// <param name="marshalToCaller">Queue events for Update() instead of
        /// delivering them on the network thread. Keep this true in Unity.</param>
        /// <param name="gossipBind">Bind a rendezvous endpoint ("tcp://*:5670")
        /// instead of using the UDP beacon. For links with no multicast: a
        /// USB-tethered device behind a port forward, a VPN, a routed network.</param>
        /// <param name="gossipConnect">Dial another node's rendezvous endpoint.</param>
        /// <param name="endpoint">This node's own data endpoint, in gossip mode.</param>
        /// <param name="advertisedEndpoint">What to tell peers this node's
        /// endpoint is, when that differs from what it binds (a forwarded port).</param>
        public static Session Join(string name,
                                   string role = "",
                                   string app = "",
                                   string realm = "uninet",
                                   string iface = "",
                                   int port = 0,
                                   bool marshalToCaller = true,
                                   string gossipBind = "",
                                   string gossipConnect = "",
                                   string endpoint = "",
                                   string advertisedEndpoint = "")
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("a device name is required", nameof(name));

            IntPtr handle = Native.uninet_session_join_ex(
                name, role ?? "", app ?? "", realm ?? "uninet", iface ?? "", port,
                gossipBind ?? "", gossipConnect ?? "", endpoint ?? "",
                advertisedEndpoint ?? "");
            if (handle == IntPtr.Zero)
                throw new InvalidOperationException("UniNet: " + Native.LastError());

            var session = new Session(handle, marshalToCaller);
            session.HookPresence();
            return session;
        }

        private void HookPresence()
        {
            Native.PeerCallback found = (uuid, n, addr, role, app, _) =>
            {
                // A managed exception must never cross back over a reverse
                // P/Invoke boundary: it terminates the process.
                try
                {
                    var peer = new Peer(uuid, n, addr, role, app);
                    Dispatch(() => PeerFound?.Invoke(peer));
                }
                catch { }
            };
            Native.PeerCallback lost = (uuid, n, addr, role, app, _) =>
            {
                try
                {
                    var peer = new Peer(uuid, n, addr, role, app);
                    Dispatch(() => PeerLost?.Invoke(peer));
                }
                catch { }
            };
            _rooted.Add(found);
            _rooted.Add(lost);
            // Checked, not discarded: a failed registration would otherwise mean
            // presence events silently never arrive, with nothing to point at.
            if (Native.uninet_session_on_peer_found(_handle, found, IntPtr.Zero) != Status.Ok ||
                Native.uninet_session_on_peer_lost(_handle, lost, IntPtr.Zero) != Status.Ok)
                throw new InvalidOperationException(
                    "UniNet: could not register presence callbacks: " + Native.LastError());
        }


        internal void Dispatch(Action action)
        {
            if (!_marshalToCaller)
            {
                // Direct mode runs user code here. Guard it separately from the
                // P/Invoke boundary guard: without this, a throwing handler in
                // direct mode produced no output at all, while queued mode
                // logged it.
                try { action(); }
                catch (Exception e) { Console.Error.WriteLine("UniNet handler threw: " + e); }
                return;
            }
            // Bounded on purpose. An app whose Update() stalls would otherwise
            // grow this without limit and die of memory exhaustion with nothing
            // to point at. Dropping the oldest keeps the newest state, and the
            // counter makes the loss visible instead of silent.
            if (_pending.Count >= MaxPendingEvents)
            {
                if (_pending.TryDequeue(out _))
                    Interlocked.Increment(ref _droppedEvents);
            }
            _pending.Enqueue(action);
        }

        /// <summary>Most events held for Update() before the oldest are dropped.</summary>
        public const int MaxPendingEvents = 100_000;

        private int _droppedEvents;

        /// <summary>
        /// Events discarded because the queue was full, meaning Update() was not
        /// called often enough. Zero in a healthy application.
        /// </summary>
        public int DroppedEvents => Volatile.Read(ref _droppedEvents);

        /// <summary>
        /// Deliver queued messages and presence events on the calling thread.
        /// Call this from MonoBehaviour.Update() in Unity. A no-op when the
        /// session was created with marshalToCaller: false.
        /// </summary>
        public void Update()
        {
            // One drainer at a time. Two threads calling Update() would split the
            // queue and run handlers concurrently, which is exactly what this
            // pump exists to prevent; a second caller simply returns.
            if (Interlocked.CompareExchange(ref _draining, 1, 0) != 0) return;
            try
            {
                while (_pending.TryDequeue(out var action))
                {
                    // One bad handler must not stop the rest of the queue draining.
                    try { action(); }
                    catch (Exception e) { Console.Error.WriteLine("UniNet handler threw: " + e); }
                }
            }
            finally { Volatile.Write(ref _draining, 0); }
        }

        private int _draining;

        /// <summary>
        /// Events waiting for <see cref="Update"/>. Non-zero and climbing means
        /// Update() is not being called often enough.
        /// </summary>
        public int PendingEvents => _pending.Count;

        /// <summary>Receive messages. A subject ending in "&gt;" matches everything below it.</summary>
        public void Subscribe(string subject, Action<Message> handler)
        {
            ThrowIfDisposed();
            if (handler == null) throw new ArgumentNullException(nameof(handler));
            _handlersKeepAlive.Add(handler);

            Native.JsonCallback cb = (subj, src, json, _) =>
            {
                try
                {
                    var msg = new Message(subj, src, json);
                    Dispatch(() => handler(msg));
                }
                catch { }
            };
            _rooted.Add(cb);
            int rc = Native.uninet_session_subscribe_json(_handle, subject, cb, IntPtr.Zero);
            if (rc != Status.Ok)
                throw new InvalidOperationException("UniNet subscribe: " + Native.LastError());
        }

        /// <summary>Send JSON to everyone, or to one peer when <paramref name="dst"/> is a peer uuid.</summary>
        public void Publish(string subject, string json, string dst = "")
        {
            ThrowIfDisposed();
            int rc = Native.uninet_session_publish_json(_handle, subject, json, dst ?? "");
            if (rc != Status.Ok)
                throw new InvalidOperationException("UniNet publish: " + Native.LastError());
        }

        /// <summary>Send a payload that is already CBOR: skips the JSON conversion.</summary>
        public void PublishCbor(string subject, byte[] cbor, string dst = "")
        {
            ThrowIfDisposed();
            if (cbor == null) throw new ArgumentNullException(nameof(cbor));
            int rc = Native.uninet_session_publish_cbor(_handle, subject, cbor,
                                                        (UIntPtr)cbor.Length, dst ?? "");
            if (rc != Status.Ok)
                throw new InvalidOperationException("UniNet publish: " + Native.LastError());
        }

        /// <summary>Every device currently on the network.</summary>
        public IReadOnlyList<Peer> Peers()
        {
            ThrowIfDisposed();
            IntPtr snap = Native.uninet_session_peers(_handle);
            if (snap == IntPtr.Zero) return Array.Empty<Peer>();
            try
            {
                int n = Native.uninet_peers_count(snap);
                var list = new List<Peer>(n);
                for (int i = 0; i < n; ++i)
                {
                    list.Add(new Peer(
                        Native.Str(Native.uninet_peers_uuid(snap, i)),
                        Native.Str(Native.uninet_peers_name(snap, i)),
                        Native.Str(Native.uninet_peers_address(snap, i)),
                        Native.Str(Native.uninet_peers_role(snap, i)),
                        Native.Str(Native.uninet_peers_app(snap, i))));
                }
                return list;
            }
            finally
            {
                // The snapshot owns the strings we just copied, so it is freed
                // only after every copy is made.
                Native.uninet_peers_free(snap);
            }
        }

        /// <summary>True when this device is on the network.</summary>
        public bool Connected =>
            _handle != IntPtr.Zero && Native.uninet_session_connected(_handle) == 1;

        /// <summary>This device's address, for others to send it a private message.</summary>
        public string Uuid =>
            _handle == IntPtr.Zero
                ? string.Empty
                : Native.ReadBuffer((b, n) => Native.uninet_session_uuid(_handle, b, n));

        /// <summary>
        /// Leave the network without releasing the handle. Idempotent.
        /// Dispose() calls it; call it yourself when the shutdown order matters.
        /// </summary>
        public void Close()
        {
            if (_handle != IntPtr.Zero) Native.uninet_session_close(_handle);
        }

        /// <summary>False once Close() or Dispose() has run.</summary>
        public bool IsOpen =>
            _handle != IntPtr.Zero && Native.uninet_session_open(_handle) == 1;

        /// <summary>One plain sentence about the connection, for a status bar.</summary>
        public string Describe() =>
            _handle == IntPtr.Zero
                ? "Not connected."
                : Native.ReadBuffer((b, n) => Native.uninet_session_describe(_handle, b, n));

        // ── conversion helpers ──
        /// <summary>JSON text to CBOR bytes: the same bytes any other language produces.</summary>
        public static byte[] JsonToCbor(string json)
        {
            int rc = Native.uninet_json_to_cbor(json, null, UIntPtr.Zero, out UIntPtr needed);
            if (rc != Status.Ok) throw new FormatException("UniNet: " + Native.LastError());
            var buf = new byte[(int)needed];
            rc = Native.uninet_json_to_cbor(json, buf, (UIntPtr)buf.Length, out _);
            if (rc != Status.Ok) throw new FormatException("UniNet: " + Native.LastError());
            return buf;
        }

        /// <summary>CBOR bytes to JSON text.</summary>
        public static string CborToJson(byte[] cbor)
        {
            if (cbor == null) throw new ArgumentNullException(nameof(cbor));
            return Native.ReadBuffer(
                (b, n) => Native.uninet_cbor_to_json(cbor, (UIntPtr)cbor.Length, b, n),
                Math.Max(512, cbor.Length * 4));
        }

        // ── build info ──
        public static ushort ProtocolVersion => Native.uninet_protocol_version();
        public static bool   HasLz4 => Native.uninet_has_lz4() == 1;
        public static string Version => Native.Str(Native.uninet_version());

        private void ThrowIfDisposed()
        {
            if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(Session));
        }

        /// <summary>
        /// Leave the network. Peers are told immediately rather than waiting for
        /// a timeout, so a device disappears from their lists at once.
        /// </summary>
        public void Dispose()
        {
            if (_handle == IntPtr.Zero) return;
            IntPtr handle = _handle;
            _handle = IntPtr.Zero;

            // Free the native handle FIRST: that is what stops the network thread
            // calling back. Only then is it safe to let the delegates go: the
            // reverse order leaves a window where native code holds a pointer to
            // a delegate the GC may already have collected.
            Native.uninet_session_free(handle);
            _rooted.Clear();
            _handlersKeepAlive.Clear();
            GC.SuppressFinalize(this);
        }

        ~Session() => Dispose();
    }
}
