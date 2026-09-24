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
// Messages and presence events arrive on UniNet's delivery thread. Touching the
// Unity API from there throws or crashes the player, so by default this class
// queues every event and hands it to you when you call Update(), which you do
// from MonoBehaviour.Update(), on the main thread:
//
//     void Update() => net.Update();
//
// Pass marshalToCaller: false to opt out and receive events directly on the
// delivery thread (correct for a console app or a background service).
//
// ── decoding off Unity's main thread ──────────────────────────────────────
// With the pump, every message is also DECODED on the main thread, inside
// Update(), and a frame that receives a mesh pays for parsing it. To move that
// work off the main thread, join with marshalToCaller: false. Handlers then run
// on UniNet's delivery thread (never the main thread, one at a time, in
// arrival order), where decoding is safe; only the RESULT goes to the main
// thread, through a queue you drain in Update():
//
//     readonly ConcurrentQueue<Mesh3> _ready = new ConcurrentQueue<Mesh3>();
//
//     net = Session.Join("MR", role: "headset", marshalToCaller: false);
//     net.SubscribeCbor("thermonav.v1.>", (subject, src, cbor) =>
//     {
//         var mesh = MyCbor.DecodeMesh(cbor);   // delivery thread: no Unity API
//         _ready.Enqueue(mesh);                 // hand the result over
//     });
//
//     void Update()                             // main thread
//     {
//         while (_ready.TryDequeue(out var mesh)) ApplyToScene(mesh);
//     }
//
// The rules that make it safe:
//   * No Unity API in the handler: no GameObject, Transform or Mesh (Debug.Log
//     is the thread-safe exception). Build plain C# data (arrays, structs,
//     Vector3 is fine as a value); create Unity objects in Update().
//   * The handler owns `cbor`: it is a fresh array per message, so it may be
//     kept or passed on without copying.
//   * Handlers run one at a time, so a slow decode delays the next message
//     (the native queue absorbs it; see Delivery). Decode, enqueue, return.
//   * The switch is per session: PeerFound and PeerLost ALSO arrive on the
//     delivery thread, and Update() no longer delivers anything. Queue them
//     the same way.
//   * Bound your own queue if the main thread can stall for long: nothing
//     else does, now that the pump is out of the path.
// tests/interop/InteropCs checks that these handlers run off the calling
// thread and that the main-thread handoff sees every decoded message.
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

        /// <summary>Just the address part of <see cref="Address"/>, without the
        /// port: "tcp://192.168.1.31:35001" -> "192.168.1.31". What a device list
        /// shows. Same as `peer.endpoint()` in C++ and `peer.endpoint` in Python.</summary>
        public string Endpoint
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

        /// <summary>The machine's hostname, as the peer advertised it. Same as
        /// `peer.host()` in C++ and `peer.host` in Python.</summary>
        /// <remarks>Before 0.2.1 this property returned the IP address, which is
        /// what <see cref="Endpoint"/> now returns - the same name meant two
        /// different things in C# and in the other two languages. If you were
        /// printing an address, that is <see cref="Endpoint"/>.</remarks>
        public string Host => Header("host");

        public override string ToString() =>
            string.IsNullOrEmpty(Role) ? $"{Name} ({Endpoint})" : $"{Name} ({Endpoint}): {Role}";
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

        // Every delegate handed to native code is a static thunk rooted in a
        // static field below, so the GC can never collect one while the network
        // thread still holds the pointer. What varies per registration - which
        // session, which handler - travels through the `user` pointer as a
        // GCHandle, and those are the handles kept here.
        private readonly List<GCHandle> _contexts = new List<GCHandle>();
        private readonly List<object> _contextRoots = new List<object>();

        /// <summary>Pin <paramref name="context"/> and return the `user` pointer for it.</summary>
        /// <remarks>
        /// The handle is Weak, and an ordinary reference in _contextRoots is what
        /// actually keeps the context alive. A strong GCHandle would be a GC root,
        /// so a session the application forgot to dispose could never be collected,
        /// its finalizer would never run, and it would sit on the network
        /// advertising itself forever. In the Unity editor that shows up as
        /// phantom peers piling up across play/stop cycles. This way an
        /// unreachable session still finalizes, and until it does the weak handle
        /// resolves normally.
        /// </remarks>
        private IntPtr Pin(object context)
        {
            var handle = GCHandle.Alloc(context, GCHandleType.Weak);
            lock (_contexts)
            {
                _contexts.Add(handle);
                _contextRoots.Add(context);
            }
            return GCHandle.ToIntPtr(handle);
        }

        /// <summary>
        /// Release the pinned contexts. Only ever called after the native handle
        /// is freed: doing it earlier leaves the network thread dereferencing a
        /// handle that no longer exists, which is a hard crash rather than an
        /// exception.
        /// </summary>
        private void FreeContexts()
        {
            lock (_contexts)
            {
                foreach (var handle in _contexts)
                    if (handle.IsAllocated) handle.Free();
                _contexts.Clear();
            }
        }

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
        /// <remarks>
        /// Every header the peer advertised is on the <see cref="Peer"/>, as in
        /// <see cref="Peers"/>. On a session joined with
        /// <c>marshalToCaller: false</c>, adding a handler first replays the
        /// devices already present to it (on a UniNet thread, like every other
        /// event in that mode, while the caller waits): a device can appear
        /// within milliseconds of <see cref="Join"/>, before the next line of
        /// the application has run, and without the replay that event had
        /// nowhere to go. The queued mode needs no replay: its events wait for
        /// <see cref="Update"/>.
        /// </remarks>
        public event Action<Peer>? PeerFound
        {
            add
            {
                if (value == null) return;
                if (_marshalToCaller)
                {
                    lock (_presence) _peerFound += value;
                    return;
                }
                Action addAndReplay = () =>
                {
                    // Under the lock, so a device is announced to this handler
                    // exactly once: by this replay if it was already here, by
                    // OnPeerFound if it arrives afterwards, and never lost
                    // between the two or followed by a stale PeerLost.
                    lock (_presence)
                    {
                        _peerFound += value;
                        foreach (var p in _present.Values)
                        {
                            try { value(p); }
                            catch (Exception e) { Console.Error.WriteLine("UniNet handler threw: " + e); }
                        }
                    }
                };
                // Added from inside a presence handler, which already holds the
                // lock on a UniNet thread: a helper thread would wait for it
                // forever, and this thread is not the application's anyway.
                if (Monitor.IsEntered(_presence)) addAndReplay();
                else RunOffCaller(addAndReplay);
            }
            remove { lock (_presence) _peerFound -= value; }
        }

        /// <summary>A device left the network.</summary>
        public event Action<Peer>? PeerLost
        {
            add { lock (_presence) _peerLost += value; }
            remove { lock (_presence) _peerLost -= value; }
        }

        private Action<Peer>? _peerFound;
        private Action<Peer>? _peerLost;
        // The devices announced so far, for the direct-mode replay above.
        private readonly object _presence = new object();
        private readonly Dictionary<string, Peer> _present = new Dictionary<string, Peer>();

        private void Presence(Peer p, bool found)
        {
            if (_marshalToCaller)
            {
                // Handlers are read when Update() runs the event, as always.
                Dispatch(() => (found ? _peerFound : _peerLost)?.Invoke(p));
                return;
            }
            lock (_presence)
            {
                if (found) _present[p.Uuid] = p;
                else _present.Remove(p.Uuid);
                Dispatch(() => (found ? _peerFound : _peerLost)?.Invoke(p));
            }
        }

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
        /// delivering them on UniNet's delivery thread. Keep this true in Unity
        /// unless you want to decode off the main thread, and then hand the
        /// results over yourself: see "decoding off Unity's main thread" at the
        /// top of Session.cs.</param>
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
        /// <param name="maxDeliveryBytes">Cap on the native delivery queue that
        /// keeps subscription handlers off the thread reading the network.
        /// -1 keeps the default (256 MiB), 0 removes the cap. Distinct from
        /// <see cref="MaxPendingEvents"/>, which bounds the managed queue that
        /// <see cref="Update"/> drains: a message crosses both.</param>
        /// <param name="maxDeliveryMessages">The same cap counted in messages.
        /// -1 keeps the default (unlimited).</param>
        /// <param name="deliveryBlockMs">How long the native network thread
        /// waits for room before discarding the oldest queued message.
        /// -1 keeps the default (5000). While it waits it reads nothing, on
        /// any subject; a session carrying only live state (poses, drags)
        /// should pass 0 together with
        /// <see cref="UniNet.DeliveryOverflow.OldestSameSubject"/>.</param>
        /// <param name="deliveryOverflow">Which message the native queue
        /// discards once it is full and the wait is over. null keeps the
        /// default, <see cref="UniNet.DeliveryOverflow.Oldest"/>.</param>
        /// <param name="evasiveMs">Milliseconds of silence before a peer is
        /// pinged. -1 keeps the default (5000).</param>
        /// <param name="expiredMs">Milliseconds of silence before a peer is
        /// reported lost (<see cref="PeerLost"/>). -1 keeps the default
        /// (30000). Lower both, e.g. 2000 / 6000, to notice a headset that
        /// dropped off Wi-Fi within seconds; much lower reports a device whose
        /// Wi-Fi is merely dozing as lost. Must end up above evasiveMs.</param>
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
                                   int compression = -1,
                                   long maxDeliveryBytes = -1,
                                   long maxDeliveryMessages = -1,
                                   int deliveryBlockMs = -1,
                                   DeliveryOverflow? deliveryOverflow = null,
                                   int evasiveMs = -1,
                                   int expiredMs = -1)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("a device name is required", nameof(name));

            // Turns .NET's bare "DllNotFoundException: uninet_c" into a message
            // that says what UniNet is and where the file goes.
            Native.EnsureLoadable();

            IntPtr handle;
            bool wantsDeliveryConfig = maxDeliveryBytes >= 0 || maxDeliveryMessages >= 0 ||
                                       deliveryBlockMs >= 0;
            bool wantsTimeouts = evasiveMs >= 0 || expiredMs >= 0;
            if (headers != null || compression >= 0 || wantsDeliveryConfig ||
                deliveryOverflow.HasValue || wantsTimeouts)
            {
                // The config-handle path, which is the only way to reach headers,
                // compression, the queue and the timeouts. join_ex cannot express
                // any of them.
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
                    // Passed together, and -1 for any of them means "leave that
                    // one at its default", so setting one knob does not silently
                    // reset the other two.
                    if (wantsDeliveryConfig)
                        Native.uninet_config_set_delivery(cfg, maxDeliveryBytes,
                                                          maxDeliveryMessages, deliveryBlockMs);
                    if (deliveryOverflow.HasValue &&
                        Native.uninet_config_set_delivery_overflow(cfg, (int)deliveryOverflow.Value) != Status.Ok)
                        throw new ArgumentOutOfRangeException(
                            nameof(deliveryOverflow), "UniNet: " + Native.LastError());
                    // -1 for either keeps that one at its default, as above.
                    if (wantsTimeouts &&
                        Native.uninet_config_set_timeouts(cfg, evasiveMs < 0 ? -1 : evasiveMs,
                                                          expiredMs < 0 ? -1 : expiredMs) != Status.Ok)
                        throw new ArgumentOutOfRangeException(
                            evasiveMs >= 0 ? nameof(evasiveMs) : nameof(expiredMs),
                            "UniNet: " + Native.LastError());
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
            IntPtr self = Pin(this);
            // Checked, not discarded: a failed registration would otherwise mean
            // presence events silently never arrive, with nothing to point at.
            //
            // The _ex forms, so the Peer handed to PeerFound carries every header
            // the peer advertised, as Peers() always did: a protocol version or a
            // capability list is needed at the moment a device appears, not on
            // the next poll.
            if (Native.uninet_session_on_peer_found_ex(_handle, PeerFoundThunk, self) != Status.Ok ||
                Native.uninet_session_on_peer_lost_ex(_handle, PeerLostThunk, self) != Status.Ok)
                throw new InvalidOperationException(
                    "UniNet: could not register presence callbacks: " + Native.LastError());
        }

        // ── native callbacks ──────────────────────────────────────────────────
        // Static and attributed so IL2CPP can reach them, and rooted in static
        // fields so the GC cannot collect them. See rule 3 in Native.cs for why
        // a lambda here would compile fine and then fail on a Quest.
        private static readonly Native.PeerExCallback PeerFoundThunk = OnPeerFound;
        private static readonly Native.PeerExCallback PeerLostThunk  = OnPeerLost;
        private static readonly Native.JsonCallback JsonThunk      = OnJson;
        private static readonly Native.CborCallback CborThunk      = OnCbor;

        [AOT.MonoPInvokeCallback(typeof(Native.PeerExCallback))]
        private static void OnPeerFound(IntPtr peer, IntPtr user)
        {
            // A managed exception must never cross back over a reverse P/Invoke
            // boundary: it terminates the process.
            try
            {
                var self = Native.Context<Session>(user);
                if (self == null) return;
                self.Presence(ReadPeer(peer, 0), found: true);
            }
            catch { }
        }

        [AOT.MonoPInvokeCallback(typeof(Native.PeerExCallback))]
        private static void OnPeerLost(IntPtr peer, IntPtr user)
        {
            try
            {
                var self = Native.Context<Session>(user);
                if (self == null) return;
                self.Presence(ReadPeer(peer, 0), found: false);
            }
            catch { }
        }

        /// <summary>
        /// Copy entry <paramref name="i"/> of a native peer snapshot, every
        /// header included. Shared by <see cref="Peers"/> and the presence
        /// events so the two can never disagree about what a Peer holds.
        /// </summary>
        private static Peer ReadPeer(IntPtr snap, int i)
        {
            // Every header the peer advertised, not just the three UniNet names
            // itself: a consumer that publishes headers: {"study": "..."} has to
            // be able to read it back.
            var headers = new Dictionary<string, string>();
            int headerCount = Native.uninet_peers_header_count(snap, i);
            for (int h = 0; h < headerCount; ++h)
            {
                string key = Native.Str(Native.uninet_peers_header_key(snap, i, h));
                if (key.Length > 0)
                    headers[key] = Native.Str(Native.uninet_peers_header(snap, i, key));
            }
            return new Peer(
                Native.Str(Native.uninet_peers_uuid(snap, i)),
                Native.Str(Native.uninet_peers_name(snap, i)),
                Native.Str(Native.uninet_peers_address(snap, i)),
                Native.Str(Native.uninet_peers_role(snap, i)),
                Native.Str(Native.uninet_peers_app(snap, i)),
                headers);
        }

        // One of these is pinned per Subscribe call: the static thunk has no
        // other way to know which handler the message belongs to.
        private sealed class JsonSub
        {
            internal readonly Session Session;
            internal readonly Action<Message> Handler;
            internal JsonSub(Session session, Action<Message> handler)
            {
                Session = session; Handler = handler;
            }
        }

        private sealed class CborSub
        {
            internal readonly Session Session;
            internal readonly Action<string, string, byte[]> Handler;
            internal CborSub(Session session, Action<string, string, byte[]> handler)
            {
                Session = session; Handler = handler;
            }
        }

        [AOT.MonoPInvokeCallback(typeof(Native.JsonCallback))]
        private static void OnJson(IntPtr subject, IntPtr src, IntPtr json, IntPtr user)
        {
            try
            {
                var sub = Native.Context<JsonSub>(user);
                if (sub == null) return;
                var message = new Message(Native.Str(subject), Native.Str(src), Native.Str(json));
                var handler = sub.Handler;
                sub.Session.Dispatch(() => handler(message));
            }
            catch { }
        }

        [AOT.MonoPInvokeCallback(typeof(Native.CborCallback))]
        private static void OnCbor(IntPtr subject, IntPtr src, IntPtr data, UIntPtr len, IntPtr user)
        {
            try
            {
                var sub = Native.Context<CborSub>(user);
                if (sub == null) return;
                ulong total = len.ToUInt64();
                if (total > int.MaxValue)
                {
                    // Reported rather than dropped: a payload this large used to
                    // vanish with no handler call and nothing logged.
                    Console.Error.WriteLine(
                        $"UniNet: a {total}-byte message is larger than a .NET array can hold; dropped");
                    return;
                }
                int n = (int)total;
                var bytes = new byte[n];
                if (n > 0) Marshal.Copy(data, bytes, 0, n);
                string subj = Native.Str(subject), from = Native.Str(src);
                var handler = sub.Handler;
                sub.Session.Dispatch(() => handler(subj, from, bytes));
            }
            catch { }
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

        /// <summary>
        /// How the NATIVE delivery queue is coping: the queue that keeps
        /// subscription handlers off the thread reading the network.
        /// </summary>
        /// <remarks>
        /// A message crosses two queues on its way to a Unity script: this one,
        /// inside the native library, and the managed one that
        /// <see cref="Update"/> drains. They fail differently and this is how to
        /// tell them apart when messages go missing:
        ///
        /// <list type="bullet">
        /// <item><description><c>Dropped</c> here, with
        /// <see cref="DroppedEvents"/> at zero: the native handler could not
        /// keep up. On this binding that handler only copies bytes and
        /// enqueues, so this should never happen; if it does, the machine is
        /// starved rather than the code being slow.</description></item>
        /// <item><description><see cref="DroppedEvents"/> climbing: Update() is
        /// not being called often enough, or the game loop is stalling. This is
        /// the usual one.</description></item>
        /// <item><description>Both zero, and a message still missing: it was
        /// never received. Look at the sender, the realm, and the network.
        /// </description></item>
        /// </list>
        /// </remarks>
        public DeliveryStats Delivery
        {
            get
            {
                ThrowIfDisposed();
                int rc = Native.uninet_session_delivery_stats(
                    _handle, out ulong queued, out ulong queuedBytes, out ulong peak,
                    out ulong delivered, out ulong dropped, out ulong blockedUs,
                    out ulong slowestUs, out int threaded);
                if (rc != Status.Ok)
                    throw new InvalidOperationException("UniNet: " + Native.LastError());
                return new DeliveryStats(queued, queuedBytes, peak, delivered, dropped,
                                         blockedUs, slowestUs, threaded != 0);
            }
        }

        /// <summary>Receive messages. A subject ending in "&gt;" matches everything below it.</summary>
        public void Subscribe(string subject, Action<Message> handler)
        {
            ThrowIfDisposed();
            if (handler == null) throw new ArgumentNullException(nameof(handler));

            IntPtr user = Pin(new JsonSub(this, handler));
            string? err = OffCaller(() => Native.uninet_session_subscribe_json(
                _handle, subject, JsonThunk, user));
            if (err != null)
                throw new InvalidOperationException("UniNet subscribe: " + err);
        }

        /// <summary>
        /// Register a native subscription. Returns null on success, else why.
        /// </summary>
        /// <remarks>
        /// Messages that arrived before a subscription existed are held natively
        /// and handed to the first matching one DURING the registering call, on
        /// the registering thread. In queued mode that is harmless: the handler
        /// only enqueues. In direct mode it would run the application's handler
        /// on its main thread, the one thing marshalToCaller: false promises not
        /// to do, so the registration is made from a short-lived thread instead
        /// and the caller waits for it. Handlers then never run on the thread
        /// that called Subscribe, buffered or not.
        /// </remarks>
        private string? OffCaller(Func<int> register)
        {
            if (_marshalToCaller)
                return register() == Status.Ok ? null : Native.LastError();
            string? err = null;
            // uninet_last_error is per thread, so it is read on the thread that failed.
            RunOffCaller(() => { if (register() != Status.Ok) err = Native.LastError(); });
            return err;
        }

        /// <summary>Run <paramref name="work"/> on a short-lived thread and wait for it.</summary>
        private static void RunOffCaller(Action work)
        {
            Exception? failure = null;
            var t = new Thread(() =>
            {
                try { work(); }
                catch (Exception e) { failure = e; }
            }) { IsBackground = true, Name = "UniNet" };
            t.Start();
            t.Join();
            if (failure != null)
                throw new InvalidOperationException("UniNet: " + failure.Message, failure);
        }

        /// <summary>
        /// Receive messages with the payload as raw CBOR bytes rather than JSON.
        /// Use this when you already have a CBOR library, or to skip the text
        /// conversion on a large payload.
        /// </summary>
        /// <remarks>
        /// The handler receives (subject, sender uuid, payload). On a session
        /// joined with <c>marshalToCaller: false</c> it runs on UniNet's
        /// delivery thread, which is where a Unity app should decode a large
        /// payload; see "decoding off Unity's main thread" at the top of this
        /// file for the handoff back to the main thread.
        /// </remarks>
        public void SubscribeCbor(string subject, Action<string, string, byte[]> handler)
        {
            ThrowIfDisposed();
            if (handler == null) throw new ArgumentNullException(nameof(handler));

            IntPtr user = Pin(new CborSub(this, handler));
            string? err = OffCaller(() => Native.uninet_session_subscribe_cbor(
                _handle, subject, CborThunk, user));
            if (err != null)
                throw new InvalidOperationException("UniNet subscribe: " + err);
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

        /// <summary>
        /// Send one JSON message to a chosen set of peers: encoded once, the
        /// same bytes whispered to each uuid in <paramref name="dsts"/>, nobody
        /// else receives it.
        /// </summary>
        /// <returns>How many peers it was handed to. A uuid that is no longer a
        /// peer is skipped and not counted; empty and repeated entries are
        /// ignored, so an empty list sends nothing and never broadcasts.</returns>
        /// <remarks>What a loop of <see cref="Publish"/> calls with a
        /// <c>dst</c> would do, without encoding and compressing the payload
        /// once per peer. Receivers see an ordinary message: nothing on arrival
        /// says it was addressed.</remarks>
        /// <exception cref="InvalidOperationException">Not on the network, or
        /// the JSON is malformed.</exception>
        public int PublishMany(string subject, string json, IEnumerable<string> dsts)
        {
            ThrowIfDisposed();
            if (dsts == null) throw new ArgumentNullException(nameof(dsts));
            var list = new List<string>(dsts);
            IntPtr[] native = Native.Utf8Array(list);
            try
            {
                int rc = Native.uninet_session_publish_many_json(
                    _handle, subject, json, native, (UIntPtr)native.Length, out UIntPtr sent);
                if (rc != Status.Ok)
                    throw new InvalidOperationException("UniNet publish: " + Native.LastError());
                return (int)sent.ToUInt64();
            }
            finally { Native.FreeUtf8Array(native); }
        }

        /// <summary>
        /// <see cref="PublishMany"/> for a payload that is already CBOR.
        /// </summary>
        public int PublishManyCbor(string subject, byte[] cbor, IEnumerable<string> dsts)
        {
            ThrowIfDisposed();
            if (cbor == null) throw new ArgumentNullException(nameof(cbor));
            if (dsts == null) throw new ArgumentNullException(nameof(dsts));
            var list = new List<string>(dsts);
            IntPtr[] native = Native.Utf8Array(list);
            try
            {
                int rc = Native.uninet_session_publish_many_cbor(
                    _handle, subject, cbor, (UIntPtr)cbor.Length, native,
                    (UIntPtr)native.Length, out UIntPtr sent);
                if (rc != Status.Ok)
                    throw new InvalidOperationException("UniNet publish: " + Native.LastError());
                return (int)sent.ToUInt64();
            }
            finally { Native.FreeUtf8Array(native); }
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
                for (int i = 0; i < n; ++i) list.Add(ReadPeer(snap, i));
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

        /// <summary>
        /// Why the last UniNet call on this thread failed. Empty when healthy.
        /// </summary>
        /// <remarks>
        /// Per thread, like the C ABI it reads: two threads failing at once do
        /// not overwrite each other's explanation. Most calls throw on failure,
        /// so this is mainly for the ones that return false rather than
        /// throwing, and for logging a reason alongside a caught exception.
        /// C++ and Python have had last_error() since v0.2; C# did not, which
        /// left a binding unable to say why something had failed.
        /// </remarks>
        public static string LastError() => Native.LastError();

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

        /// <summary>
        /// What UniNet is doing right now, as text: version, compression tiers,
        /// every network on this machine and which one discovery chose, and every
        /// live session with its identity, peers and reconnect count.
        /// </summary>
        /// <remarks>
        /// Put this behind a diagnostics button. Nearly every "the headset cannot
        /// see the server" report is answered by the networks section, and none of
        /// it was reachable from inside an application before.
        /// </remarks>
        public static string Diagnostics() =>
            Native.ReadBuffer((b, n) => Native.uninet_diagnostics(b, n), 4096);

        /// <summary>
        /// Write a crash report to <paramref name="path"/> if the process dies on
        /// a fatal signal. Off unless called.
        /// </summary>
        /// <remarks>
        /// Worth enabling in a Unity player on a headset, where there is no
        /// terminal and the state at the moment of the crash is otherwise lost.
        /// Any handler already installed is chained to, so Unity's own crash
        /// reporting keeps working.
        /// </remarks>
        public static bool EnableCrashLog(string path) =>
            Native.uninet_enable_crash_log(path) == Status.Ok;

        public static void DisableCrashLog() => Native.uninet_disable_crash_log();

        // ── build info ──
        // These three are usually the first thing an application calls - the demo
        // prints them before joining - so they explain a missing native library
        // too rather than letting the raw DllNotFoundException out.
        public static ushort ProtocolVersion
        {
            get { Native.EnsureLoadable(); return Native.uninet_protocol_version(); }
        }

        public static bool HasLz4
        {
            get { Native.EnsureLoadable(); return Native.uninet_has_lz4() == 1; }
        }

        public static string Version
        {
            get { Native.EnsureLoadable(); return Native.Str(Native.uninet_version()); }
        }

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
            // Only now: while the native handle lived, the network thread could
            // still be inside a thunk holding one of these.
            FreeContexts();
            // Drop ourselves from the ProcessExit list, which otherwise grew by
            // one entry per session for the life of the process.
            lock (_live) _live.RemoveAll(w => !w.TryGetTarget(out var t) || ReferenceEquals(t, this));
            // Anything still queued refers to a session that no longer exists.
            while (_pending.TryDequeue(out _)) { }
            GC.SuppressFinalize(this);
        }

        ~Session() => Dispose();
    }

    /// <summary>
    /// Which message the native delivery queue discards once it is full and
    /// the wait for room (<c>deliveryBlockMs</c>) is over. Same values as the
    /// C++ <c>uninet::DeliveryOverflow</c>.
    /// </summary>
    public enum DeliveryOverflow
    {
        /// <summary>The oldest queued item, whatever it is. The default.</summary>
        Oldest = 0,
        /// <summary>The oldest queued message on the subject of the one
        /// arriving, and only when there is none, the oldest item: a flooding
        /// stream evicts its own stale copies rather than another subject's
        /// message.</summary>
        OldestSameSubject = 1,
    }

    /// <summary>
    /// A snapshot of the native delivery queue: what the network thread has
    /// handed over and what the handlers have done with it.
    /// </summary>
    /// <remarks>
    /// This is what settles a "we are losing messages" report without a packet
    /// capture. <see cref="Dropped"/> is non-zero only when THIS process
    /// discarded messages because a handler stopped draining the queue; all
    /// zero means the receiving side is healthy and the loss is elsewhere.
    /// See <see cref="Session.Delivery"/> for how it relates to the managed
    /// queue that <see cref="Session.Update"/> drains.
    /// </remarks>
    public readonly struct DeliveryStats
    {
        internal DeliveryStats(ulong queued, ulong queuedBytes, ulong peakQueued,
                               ulong delivered, ulong dropped, ulong blockedUs,
                               ulong slowestHandlerUs, bool threaded)
        {
            Queued = queued;
            QueuedBytes = queuedBytes;
            PeakQueued = peakQueued;
            Delivered = delivered;
            Dropped = dropped;
            BlockedMicroseconds = blockedUs;
            SlowestHandlerMicroseconds = slowestHandlerUs;
            Threaded = threaded;
        }

        /// <summary>Messages waiting for a handler right now.</summary>
        public ulong Queued { get; }
        /// <summary>Their payload bytes.</summary>
        public ulong QueuedBytes { get; }
        /// <summary>High-water mark of <see cref="Queued"/> since the join.</summary>
        public ulong PeakQueued { get; }
        /// <summary>Messages handed to the handlers.</summary>
        public ulong Delivered { get; }
        /// <summary>
        /// Messages discarded at the cap, never delivered. Non-zero means a
        /// handler stopped draining the queue for longer than the configured
        /// block budget.
        /// </summary>
        public ulong Dropped { get; }
        /// <summary>
        /// How long the network thread has spent waiting for room, in
        /// microseconds. Non-zero with <see cref="Dropped"/> at zero is a
        /// healthy loaded transfer: backpressure did its job and nothing was
        /// lost.
        /// </summary>
        public ulong BlockedMicroseconds { get; }
        /// <summary>
        /// The longest single handler run seen, in microseconds. Names the
        /// handler responsible for a backed-up queue without a profiler.
        /// </summary>
        public ulong SlowestHandlerMicroseconds { get; }
        /// <summary>
        /// False when handlers run on the network thread, where a slow handler
        /// costs messages rather than latency. True in every default setup.
        /// </summary>
        public bool Threaded { get; }

        /// <summary>One line for a log or a status overlay.</summary>
        public override string ToString() =>
            $"queued {Queued} ({QueuedBytes / 1024} KiB), peak {PeakQueued}, " +
            $"delivered {Delivered}, dropped {Dropped}, " +
            $"blocked {BlockedMicroseconds / 1000} ms, " +
            $"slowest handler {SlowestHandlerMicroseconds / 1000} ms" +
            (Threaded ? "" : ", ON THE NETWORK THREAD");
    }
}
