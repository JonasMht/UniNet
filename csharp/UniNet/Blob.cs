// UniNet: large payload transfer for C#.
//
// Publish sends a message that has to fit in memory whole on both ends. A file,
// a 3D volume or a large mesh is not a message: Blob chunks it, streams it,
// reassembles it on the far side, and reports progress at both ends.
//
//     using var net  = Session.Join("Recorder");
//     using var blob = new Blob(net, "files");
//
//     blob.Received += (info, data) => File.WriteAllBytes(info.Name, data);
//     blob.Progress += (info, done) => Debug.Log($"{100.0 * done / info.Size:F0}%");
//
//     blob.SendFile("/path/to/dataset.zip");
//
// Metadata travels with the payload, so a typed transfer needs no side channel:
// put the array shape and dtype in `metaJson` and the receiver rebuilds it.
//
// THREADING. Like Session, callbacks arrive on the network thread by default.
// Pass the owning session and it will marshal them for you if that session was
// created with marshalToCaller: true (the default), so a Unity app receives them
// on the main thread from Session.Update().
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace UniNet
{
    /// <summary>Describes one large transfer.</summary>
    public sealed class BlobInfo
    {
        /// <summary>Unique per transfer.</summary>
        public string Id { get; }
        /// <summary>Logical name: a filename, a volume id, whatever you used.</summary>
        public string Name { get; }
        /// <summary>uuid of the sender.</summary>
        public string Src { get; }
        /// <summary>Whatever the sender attached, as JSON. "null" when none.</summary>
        public string MetaJson { get; }
        /// <summary>Total bytes.</summary>
        public long Size { get; }

        internal BlobInfo(string id, string name, string src, string metaJson, long size)
        {
            Id = id; Name = name; Src = src; MetaJson = metaJson; Size = size;
        }

        public override string ToString() => $"{Name} ({Size} bytes) from {Src}";
    }

    public sealed class Blob : IDisposable
    {
        private IntPtr _handle;
        private readonly Session _session;    // held so it cannot be finalized first
        private readonly List<Delegate> _rooted = new List<Delegate>();

        /// <summary>A transfer completed. `data` is valid only inside the handler.</summary>
        public event Action<BlobInfo, byte[]>? Received;
        /// <summary>Chunks are arriving. The second argument is bytes so far.</summary>
        public event Action<BlobInfo, long>? Progress;
        /// <summary>A transfer was abandoned, with the reason.</summary>
        public event Action<BlobInfo, string>? Failed;

        /// <summary>
        /// Open a transfer channel on <paramref name="subject"/>. The transfer runs
        /// beneath that subject, so it never collides with your own messages on the
        /// same prefix. Both ends must use the same subject.
        /// </summary>
        public Blob(Session session, string subject)
        {
            _session = session ?? throw new ArgumentNullException(nameof(session));
            if (string.IsNullOrEmpty(subject))
                throw new ArgumentException("a subject is required", nameof(subject));

            _handle = Native.uninet_blob_new(session.Handle, subject);
            if (_handle == IntPtr.Zero)
                throw new InvalidOperationException("UniNet: " + Native.LastError());

            Native.BlobCallback received = (id, name, src, meta, data, len, _) =>
            {
                // A managed exception must never cross back over a reverse P/Invoke.
                try
                {
                    ulong total = len.ToUInt64();
                    // A .NET array cannot exceed int.MaxValue. Left to throw
                    // inside the catch below, a transfer over 2 GiB vanished
                    // with no Received, no Failed and no log.
                    if (total > int.MaxValue)
                    {
                        var big = new BlobInfo(id, name, src, meta, (long)total);
                        session.Dispatch(() => Failed?.Invoke(big,
                            $"the transfer is {total} bytes, larger than a .NET array can hold"));
                        return;
                    }
                    int n = (int)total;
                    var bytes = new byte[n];
                    if (n > 0) Marshal.Copy(data, bytes, 0, n);
                    var info = new BlobInfo(id, name, src, meta, n);
                    session.Dispatch(() => Received?.Invoke(info, bytes));
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine("UniNet blob receive failed: " + e);
                }
            };
            Native.BlobProgressCallback progress = (id, name, done, total, _) =>
            {
                try
                {
                    long d = (long)done.ToUInt64(), t = (long)total.ToUInt64();
                    var info = new BlobInfo(id, name, string.Empty, "null", t);
                    session.Dispatch(() => Progress?.Invoke(info, d));
                }
                catch { }
            };
            Native.BlobFailedCallback failed = (id, name, reason, _) =>
            {
                try
                {
                    var info = new BlobInfo(id, name, string.Empty, "null", 0);
                    session.Dispatch(() => Failed?.Invoke(info, reason));
                }
                catch { }
            };

            // Rooted for as long as the native handle lives: the GC does not know
            // native code is holding these function pointers.
            _rooted.Add(received);
            _rooted.Add(progress);
            _rooted.Add(failed);

            Check(Native.uninet_blob_on_received(_handle, received, IntPtr.Zero), "on_received");
            Check(Native.uninet_blob_on_progress(_handle, progress, IntPtr.Zero), "on_progress");
            Check(Native.uninet_blob_on_failed(_handle, failed, IntPtr.Zero), "on_failed");

            // The native blob holds a raw C++ reference to the session. Telling
            // the session about it lets Dispose() neutralise this object first;
            // otherwise a session disposed before its blob left that reference
            // dangling and the next Send() segfaulted with no exception.
            // Must come after the native blob exists, and must fail if the
            // session is already disposing: otherwise the registration lands
            // after DisposeBlobs has run and the native Blob keeps a reference
            // to a freed Session.
            try
            {
                session.Register(this);
            }
            catch
            {
                Native.uninet_blob_free(Interlocked.Exchange(ref _handle, IntPtr.Zero));
                throw;
            }
        }

        private static void Check(int rc, string what)
        {
            if (rc != Status.Ok)
                throw new InvalidOperationException($"UniNet blob {what}: {Native.LastError()}");
        }

        /// <summary>
        /// Send bytes. Returns the transfer id, or an empty string if it could not
        /// start (not on the network).
        /// </summary>
        /// <param name="metaJson">Arbitrary metadata as JSON, or null.</param>
        /// <param name="dst">A peer uuid to send privately, or null to broadcast.</param>
        public string Send(string name, byte[] data, string? metaJson = null, string? dst = null)
        {
            ThrowIfDisposed();
            if (data == null) throw new ArgumentNullException(nameof(data));
            var idBuf = new byte[256];
            int n = Native.uninet_blob_send(_handle, name, data, (UIntPtr)data.Length,
                                            metaJson ?? "", dst ?? "", idBuf,
                                            (UIntPtr)idBuf.Length);
            if (n < 0) throw new InvalidOperationException("UniNet blob send: " + Native.LastError());
            return System.Text.Encoding.UTF8.GetString(idBuf, 0, n);
        }

        /// <summary>Read a file and send it. The name defaults to its basename.</summary>
        public string SendFile(string path, string? metaJson = null, string? dst = null)
        {
            ThrowIfDisposed();
            var idBuf = new byte[256];
            int n = Native.uninet_blob_send_file(_handle, path, metaJson ?? "", dst ?? "",
                                                 idBuf, (UIntPtr)idBuf.Length);
            if (n < 0) throw new InvalidOperationException("UniNet blob send: " + Native.LastError());
            return System.Text.Encoding.UTF8.GetString(idBuf, 0, n);
        }

        /// <summary>Transfers currently being reassembled.</summary>
        public int IncomingCount =>
            _handle == IntPtr.Zero ? 0 : Native.uninet_blob_incoming_count(_handle);

        private void ThrowIfDisposed()
        {
            if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(Blob));
        }

        public void Dispose()
        {
            // Interlocked, not read-test-clear: two threads could both see a
            // non-zero handle and both free it. The realistic trigger is the
            // finalizer racing Session.DisposeBlobs, which resurrects a
            // finalizable Blob through its weak reference.
            IntPtr handle = Interlocked.Exchange(ref _handle, IntPtr.Zero);
            if (handle == IntPtr.Zero) return;
            // Free the native handle first: that is what stops the network thread
            // calling back. Only then may the delegates become collectable.
            Native.uninet_blob_free(handle);
            _rooted.Clear();
            _session.Unregister(this);
            GC.SuppressFinalize(this);
        }

        ~Blob() => Dispose();
    }
}
