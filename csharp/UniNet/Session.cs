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

        private readonly Dictionary<string, string> _headers;

        internal Peer(string uuid, string name, string address, string role, string app,
                      Dictionary<string, string>? headers = null)
        {
            Uuid = uuid; Name = name; Address = address; Role = role; App = app;
            _headers = headers ?? new Dictionary<string, string>();
        }

        /// <summary>
        /// A key/value this peer advertised, or "" when it did not advertise one.
        /// Use <see cref="HasHeader"/> to tell an absent header from an empty one.
        /// </summary>
        public string Header(string key) =>
            _headers.TryGetValue(key, out var v) ? v : string.Empty;

        /// <summary>True when the peer advertised <paramref name="key"/> at all.</summary>
        public bool HasHeader(string key) => _headers.ContainsKey(key);

        /// <summary>Every key/value this peer advertised.</summary>
        public IReadOnlyDictionary<string, string> Headers => _headers;

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

        // Blob holds a raw C++ reference to this session. Disposing the session
        // first left that reference dangling and the next Send() segfaulted with
        // no managed exception, so the session neutralises its blobs on the way
        // out. Weak, so a dropped Blob is still collectable.
        private readonly List<WeakReference<Blob>> _blobs = new List<WeakReference<Blob>>();
        private bool _blobsClosed;

        internal void Register(Blob blob)
        {
            lock (_blobs)
            {
                // Refuse once teardown has begun. Registering after DisposeBlobs
                // has run left the native Blob holding a reference to a Session
                // that was about to be freed.
                if (_blobsClosed)
                    throw new ObjectDisposedException(nameof(Session),
                        "the session is closing; a Blob cannot be created on it");
                // Compact dead entries so a create/dispose loop does not grow
                // this list forever.
                _blobs.RemoveAll(w => !w.TryGetTarget(out _));
                _blobs.Add(new WeakReference<Blob>(blob));
            }
        }

        internal void Unregister(Blob blob)
        {
            // Never blocks on the blob itself: DisposeBlobs calls Dispose with
            // the list already detached, so this cannot re-enter that walk.
            lock (_blobs) _blobs.RemoveAll(w => !w.TryGetTarget(out var t) || ReferenceEquals(t, blob));
        }

        private void DisposeBlobs()
        {
            List<WeakReference<Blob>> snapshot;
            lock (_blobs)
            {
                _blobsClosed = true;
                snapshot = new List<WeakReference<Blob>>(_blobs);
                _blobs.Clear();
            }
            // Disposed outside the lock: Blob.Dispose calls Unregister, which
            // takes the same lock.
            foreach (var weak in snapshot)
                if (weak.TryGetTarget(out var b))
                {
                    try { b.Dispose(); } catch { }
                }
        }

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
            lock (_live)
            {
                _live.RemoveAll(w => !w.TryGetTarget(out _));
                _live.Add(new WeakReference<Session>(this));
            }
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
        /// <param name="headers">Extra key/value advertised to every peer, readable
        /// through <see cref="Peer.Header"/>.</param>
        /// <param name="compression">Wire compression: 0 none, 1 zlib, 2 LZ4.
        /// -1 keeps the build's default, which is the fastest tier available.</param>
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
                                   string advertisedEndpoint = "",
                                   IReadOnlyDictionary<string, string>? headers = null,
                                   int compression = -1)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("a device name is required", nameof(name));

            IntPtr handle;
            if (headers != null || compression >= 0)
            {
                // The config-handle path, which is the only way to reach headers
                // and compression. join_ex cannot express either.
                IntPtr cfg = Native.uninet_config_new();
                if (cfg == IntPtr.Zero)
                    throw new InvalidOperationException("UniNet: " + Native.LastError());
                try
                {
                    Native.uninet_config_set_role(cfg, role ?? "");
                    Native.uninet_config_set_app(cfg, app ?? "");
                    Native.uninet_config_set_realm(cfg, realm ?? "uninet");
                    Native.uninet_config_set_interface(cfg, iface ?? "");
                    if (port > 0) Native.uninet_config_set_port(cfg, port);
                    Native.uninet_config_set_gossip(cfg, gossipBind ?? "", gossipConnect ?? "",
                                                    endpoint ?? "", advertisedEndpoint ?? "");
                    if (headers != null)
                        foreach (var kv in headers)
                            if (Native.uninet_config_set_header(cfg, kv.Key, kv.Value) != Status.Ok)
                                throw new InvalidOperationException(
                                    "UniNet header '" + kv.Key + "': " + Native.LastError());
                    if (compression >= 0 &&
                        Native.uninet_config_set_compression(cfg, compression) != Status.Ok)
                        throw new ArgumentOutOfRangeException(
                            nameof(compression), "UniNet: " + Native.LastError());
                    handle = Native.uninet_session_join_cfg(name, cfg);
                }
                finally { Native.uninet_config_free(cfg); }
            }
            else
            {
                handle = Native.uninet_session_join_ex(
                    name, role ?? "", app ?? "", realm ?? "uninet", iface ?? "", port,
                    gossipBind ?? "", gossipConnect ?? "", endpoint ?? "",
                    advertisedEndpoint ?? "");
            }
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

        /// <summary>
        /// Receive messages with the payload as raw CBOR bytes rather than JSON.
        /// Use this when you already have a CBOR library, or to skip the text
        /// conversion on a large payload.
        /// </summary>
        public void SubscribeCbor(string subject, Action<string, string, byte[]> handler)
        {
            ThrowIfDisposed();
            if (handler == null) throw new ArgumentNullException(nameof(handler));

            Native.CborCallback cb = (subj, src, data, len, _) =>
            {
                try
                {
                    ulong total = len.ToUInt64();
                    if (total > int.MaxValue) return;
                    int n = (int)total;
                    var bytes = new byte[n];
                    if (n > 0) Marshal.Copy(data, bytes, 0, n);
                    Dispatch(() => handler(subj, src, bytes));
                }
                catch { }
            };
            _rooted.Add(cb);
            int rc = Native.uninet_session_subscribe_cbor(_handle, subject, cb, IntPtr.Zero);
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
            // A null snapshot is an error, not an empty network; reporting it as
            // "no peers" hid the reason entirely.
            if (snap == IntPtr.Zero)
                throw new InvalidOperationException("UniNet peers: " + Native.LastError());
            try
            {
                int n = Native.uninet_peers_count(snap);
                var list = new List<Peer>(n);
                for (int i = 0; i < n; ++i)
                {
                    var headers = new Dictionary<string, string>();
                    foreach (var key in new[] { "role", "app", "host" })
                        if (Native.uninet_peers_has_header(snap, i, key) == 1)
                            headers[key] = Native.Str(Native.uninet_peers_header(snap, i, key));
                    list.Add(new Peer(
                        Native.Str(Native.uninet_peers_uuid(snap, i)),
                        Native.Str(Native.uninet_peers_name(snap, i)),
                        Native.Str(Native.uninet_peers_address(snap, i)),
                        Native.Str(Native.uninet_peers_role(snap, i)),
                        Native.Str(Native.uninet_peers_app(snap, i)),
                        headers));
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
            // Interlocked, not read-test-clear: two threads could both see a
            // non-zero handle and both call uninet_session_free on it.
            IntPtr handle = Interlocked.Exchange(ref _handle, IntPtr.Zero);
            if (handle == IntPtr.Zero) return;

            // Blobs first: each holds a raw C++ reference to this session, so
            // freeing the session under them is a dangling pointer.
            DisposeBlobs();
            // Then the native handle: that is what stops the network thread
            // calling back. Only then is it safe to let the delegates go: the
            // reverse order leaves a window where native code holds a pointer to
            // a delegate the GC may already have collected.
            Native.uninet_session_free(handle);
            // Drop ourselves from the ProcessExit list, which otherwise grew by
            // one entry per session for the life of the process.
            lock (_live) _live.RemoveAll(w => !w.TryGetTarget(out var t) || ReferenceEquals(t, this));
            // Anything still queued refers to a session that no longer exists.
            while (_pending.TryDequeue(out _)) { }
            _rooted.Clear();
            _handlersKeepAlive.Clear();
            GC.SuppressFinalize(this);
        }

        ~Session() => Dispose();
    }
}
