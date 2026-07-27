// UniNet — a peer on the network. Discovered, never configured.
//
// A Peer is what one node learns about another when it appears on the LAN: who
// it is, what it calls itself, where it is, and whatever it chose to advertise.
// Nobody types any of it — it arrives with the ZRE discovery beacon.
#pragma once

#include <map>
#include <string>

namespace uninet {

struct Peer {
    // Assigned by ZRE, stable for the life of the remote process. This is the
    // address you `publish(..., dst)` to for a private message.
    std::string uuid;
    // Human-facing name the peer chose, e.g. "OR Headset".
    std::string name;
    // Observed endpoint, e.g. "tcp://192.168.1.31:35001". Taken from the
    // connection, not from anything the peer claims about itself.
    std::string address;

    // Free-form key/value the peer advertised at startup. UniNet gives three of
    // them a name because every consumer wants them; the rest are yours.
    std::map<std::string, std::string> headers;

    std::string role() const { return header("role"); }   // "server" / "headset" / "viewer"
    std::string app()  const { return header("app");  }   // owning application
    std::string host() const { return header("host"); }   // the machine's hostname

    // Just the address part of `address`: "tcp://192.168.1.31:35001" ->
    // "192.168.1.31". What a device list shows; the port is an ephemeral detail.
    std::string endpoint() const;

    std::string header(const std::string& key) const {
        auto it = headers.find(key);
        return it == headers.end() ? std::string{} : it->second;
    }

    // "OR Headset (192.168.1.31) — headset", for a device list or a log line.
    std::string describe() const;
};

}  // namespace uninet
