// UniNet — large payload transfer.
//
// publish() is for messages. A message has to fit comfortably in memory on both
// ends and arrive as one unit, which makes it the wrong tool for a 200 MB CT
// volume or a case file: the sender blocks while it is serialized, the receiver
// has no idea anything is happening until it all lands, and a single lost peer
// wastes the whole transfer.
//
// Blob does the heavy lifting instead. It splits the payload into chunks, streams
// them, reassembles on the far side, and reports progress at both ends:
//
//     uninet::Blob blob(*net, "files");
//     blob.on_received([](const uninet::BlobInfo& info, const uninet::Bytes& data) {
//         save(info.name, data);
//     });
//     blob.send_file("/path/to/case.zip");
//
// Arbitrary metadata rides with the payload, which is what makes a typed
// transfer (a numpy array's shape and dtype, a mesh's vertex count) a one-liner
// rather than a second protocol you have to invent.
//
// Reassembly is bounded: a transfer that stalls is dropped, and there are hard
// caps on concurrent transfers and total buffered bytes. An unauthenticated peer
// on the LAN must not be able to exhaust memory by starting a thousand transfers
// and never finishing them.
#pragma once

#include "uninet/cbor.h"
#include "uninet/session.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace uninet {

struct BlobInfo {
    std::string id;      // unique per transfer
    std::string name;    // logical name — a filename, a volume id, whatever you use
    std::string src;     // uuid of the sender
    Cbor        meta;    // yours: array shape and dtype, mesh counts, case id …
    size_t      size = 0;   // total bytes
};

// Fired as chunks arrive (receiver) or are sent (sender). `done` counts bytes so
// far; `info.size` is the total. Called on the network thread.
using BlobProgress = std::function<void(const BlobInfo& info, size_t done)>;
// Fired once, when a transfer is complete.
using BlobHandler  = std::function<void(const BlobInfo& info, const Bytes& data)>;
// Fired when a transfer is abandoned — the sender vanished, or it stalled.
using BlobFailed   = std::function<void(const BlobInfo& info, const std::string& why)>;

struct BlobConfig {
    // Chunk size on the wire. 256 KiB keeps each message comfortably inside
    // ZeroMQ's defaults while amortising per-message overhead; larger chunks buy
    // little and make progress reporting coarser.
    size_t chunk_bytes = 256 * 1024;

    // A transfer with no chunk for this long is abandoned, so a sender that
    // crashes mid-file does not pin its buffer forever.
    std::chrono::seconds stall_timeout{30};

    // Hard ceilings on what an incoming transfer may cost us. A peer on the LAN
    // is unauthenticated, so these are a memory-exhaustion guard, not a tuning
    // knob — raise them deliberately.
    size_t max_blob_bytes = size_t(2) * 1024 * 1024 * 1024;   // 2 GiB per transfer
    size_t max_total_bytes = size_t(4) * 1024 * 1024 * 1024;  // 4 GiB in flight
    size_t max_concurrent = 32;
};

class Blob {
public:
    // `subject` is the base subject; the transfer uses "<subject>.blob" beneath
    // it, so it never collides with your own messages on the same prefix.
    Blob(Session& session, std::string subject, BlobConfig cfg = {});
    ~Blob();

    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;

    // ── sending ──
    // Send bytes. `dst` empty broadcasts to every device; otherwise only that
    // peer receives it. Returns the transfer id, or "" if it could not start.
    std::string send(const std::string& name, const uint8_t* data, size_t len,
                     Cbor meta = Cbor::null(), const std::string& dst = "");
    std::string send(const std::string& name, const Bytes& data,
                     Cbor meta = Cbor::null(), const std::string& dst = "") {
        return send(name, data.data(), data.size(), std::move(meta), dst);
    }
    // Read a file and send it. The name defaults to the file's basename.
    std::string send_file(const std::string& path, Cbor meta = Cbor::null(),
                          const std::string& dst = "", const std::string& name = "");

    // ── receiving ──
    void on_received(BlobHandler cb);
    void on_progress(BlobProgress cb);   // receiver side
    void on_failed(BlobFailed cb);

    // Transfers currently being reassembled.
    size_t incoming_count() const;

    // Drop stalled transfers. Called automatically whenever a chunk arrives;
    // call it yourself if you have long idle periods and want the memory back.
    void sweep();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uninet
