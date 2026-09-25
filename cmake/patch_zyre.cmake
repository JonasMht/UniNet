# Two fixes to zyre v2.0.1, applied to its source tree after it is fetched.
# Idempotent, so a re-populate or a second run is harmless.
#
#     cmake -P patch_zyre.cmake        (run in zyre's source directory)
#
# 1. Dial back the address a HELLO actually came from.
#
#    A node that discovers on every network binds its mailbox on every interface,
#    and zyre then advertises it as tcp://<hostname>:<port>. A peer that cannot
#    resolve that hostname (most of them: nothing registers it anywhere) cannot
#    connect back, so the introduction is one-sided and the peer never appears.
#    The TCP connection the HELLO arrived on already names an address that
#    works, since the sender just used it: libzmq reports it as the frame's
#    "Peer-Address". When the advertised host is not a numeric address, use that.
#
# 2. Drop the `distclean` helper.
#
#    czmq and zyre both create a `distclean` target and attach a command to it.
#    Built side by side in one tree, zyre attaches its command to czmq's target,
#    which lives in another directory. CMake 4 rejects that (CMP0040 can no
#    longer be set to OLD) and the configure stops. UniNet never uses the target.
#
# 3. Prefer the local network over a VPN.
#
#    UniNet beacons on every network, so two devices on the same Wi-Fi that are
#    both on a VPN hear each other twice: by broadcast on the Wi-Fi and by the
#    unicast sweep over the tunnel. Zyre connects to whichever beacon comes
#    first and keeps that connection, and a connection through the tunnel (out
#    to the VPN server and back) is slow and backs up under a video stream.
#    So a beacon that came through a point-to-point link from a peer not yet
#    known is held for 2.5 s, time for the same peer's local beacon to arrive;
#    a peer heard only through the tunnel is connected after that. A peer
#    already connected through the tunnel and later heard locally is
#    reconnected locally, once.

set(marker "UNINET PATCH")

# ── 1. src/zre_msg.c ──
set(zre_msg "${CMAKE_CURRENT_SOURCE_DIR}/src/zre_msg.c")
file(READ "${zre_msg}" text)
if(NOT text MATCHES "${marker}")
    set(helper [=[
//  UNINET PATCH: the HELLO's endpoint, made dialable. A node bound to every
//  interface advertises tcp://<hostname>:<port>, which the receiver usually
//  cannot resolve. The connection the HELLO came in on names an address that
//  works, so dial that instead. A numeric endpoint is left alone.
static void
s_dialable_endpoint (char *endpoint, size_t size, zmq_msg_t *frame)
{
    const char *peer = zmq_msg_gets (frame, "Peer-Address");
    if (!peer || strncmp (endpoint, "tcp://", 6) != 0)
        return;
    const char *host = endpoint + 6;
    const char *colon = strrchr (host, ':');
    if (!colon || host [0] == '[')
        return;
    char name [256];
    size_t length = (size_t) (colon - host);
    if (length >= sizeof (name))
        return;
    memcpy (name, host, length);
    name [length] = 0;
    struct in_addr ipv4;
    if (inet_pton (AF_INET, name, &ipv4) == 1 && strcmp (name, "0.0.0.0") != 0)
        return;
    char port [16];
    snprintf (port, sizeof (port), "%s", colon + 1);
    snprintf (endpoint, size, strchr (peer, ':') ? "tcp://[%s]:%s" : "tcp://%s:%s", peer, port);
}

int
zre_msg_recv (zre_msg_t *self, zsock_t *input)]=])
    string(FIND "${text}" "int\nzre_msg_recv (zre_msg_t *self, zsock_t *input)" at)
    string(FIND "${text}" "GET_STRING (self->endpoint);" use)
    if(at EQUAL -1 OR use EQUAL -1)
        message(FATAL_ERROR "patch_zyre: zre_msg.c is not the zyre v2.0.1 this patch was written for")
    endif()
    string(REPLACE "GET_STRING (self->endpoint);"
                   "GET_STRING (self->endpoint);\n            s_dialable_endpoint (self->endpoint, sizeof (self->endpoint), &frame);"
                   text "${text}")
    string(REPLACE "int\nzre_msg_recv (zre_msg_t *self, zsock_t *input)" "${helper}" text "${text}")
    file(WRITE "${zre_msg}" "${text}")
endif()

# ── 2. CMakeLists.txt ──
set(lists "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
file(READ "${lists}" text)
string(REGEX REPLACE "add_custom_command[ \t]*\\([^()]*TARGET[ \t]+distclean[^()]*\\)" "" patched "${text}")
if(NOT patched STREQUAL text)
    file(WRITE "${lists}" "${patched}")
endif()

# ── 3. src/zyre_node.c ──
set(zyre_node "${CMAKE_CURRENT_SOURCE_DIR}/src/zyre_node.c")
file(READ "${zyre_node}" text)
if(NOT text MATCHES "${marker}")
    set(fields_old "    char *zap_domain;           // ZAP domain if any\n};")
    set(fields_new "    char *zap_domain;           // ZAP domain if any\n    zhash_t *uninet_tunnel_seen;    //  UNINET PATCH: uuid -> when first heard only through a tunnel\n    zhash_t *uninet_moved_local;    //  UNINET PATCH: uuids already reconnected locally\n};")
    set(init_old "    self->zap_domain = strdup(ZAP_DOMAIN_DEFAULT);")
    set(init_new "    self->zap_domain = strdup(ZAP_DOMAIN_DEFAULT);\n    self->uninet_tunnel_seen = zhash_new ();\n    zhash_autofree (self->uninet_tunnel_seen);\n    self->uninet_moved_local = zhash_new ();\n    zhash_autofree (self->uninet_moved_local);")
    set(free_old "        zhash_destroy (&self->headers);")
    set(free_new "        zhash_destroy (&self->headers);\n        zhash_destroy (&self->uninet_tunnel_seen);\n        zhash_destroy (&self->uninet_moved_local);")
    set(head_old "//  Handle beacon data\n\nstatic void\nzyre_node_recv_beacon (zyre_node_t *self)")
    set(head_new [=[//  UNINET PATCH: prefer the local network over a VPN (see patch_zyre.cmake).

#if !defined (_WIN32)
#include <ifaddrs.h>        //  IFF_* come with czmq's prelude
#endif

#define UNINET_TUNNEL_HOLD_MS 2500

//  True when the IPv4 host `address` (a bare address, or "tcp://a.b.c.d:port")
//  is on one of this machine's point-to-point links, a VPN such as WireGuard:
//  a beacon from it came through the tunnel.
static bool
s_uninet_via_tunnel (const char *address)
{
#if defined (_WIN32)
    (void) address;
    return false;
#else
    if (!address)
        return false;
    char host [64];
    if (strncmp (address, "tcp://", 6) == 0)
        address += 6;
    size_t length = strcspn (address, ":");
    if (length == 0 || length >= sizeof (host))
        return false;
    memcpy (host, address, length);
    host [length] = 0;
    struct in_addr source;
    if (inet_pton (AF_INET, host, &source) != 1)
        return false;
    struct ifaddrs *list = NULL;
    if (getifaddrs (&list) != 0)
        return false;
    bool tunnel = false;
    struct ifaddrs *i;
    for (i = list; i && !tunnel; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET
        || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_POINTOPOINT))
            continue;
        uint32_t own = ((struct sockaddr_in *) i->ifa_addr)->sin_addr.s_addr;
        uint32_t mask = i->ifa_netmask? ((struct sockaddr_in *) i->ifa_netmask)->sin_addr.s_addr: 0xFFFFFFFF;
        if ((own & mask) == (source.s_addr & mask))
            tunnel = true;
        else
        if (i->ifa_dstaddr && i->ifa_dstaddr->sa_family == AF_INET
        &&  ((struct sockaddr_in *) i->ifa_dstaddr)->sin_addr.s_addr == source.s_addr)
            tunnel = true;
    }
    freeifaddrs (list);
    return tunnel;
#endif
}

static void
s_uninet_empty (zhash_t *hash)
{
    zlist_t *keys = zhash_keys (hash);
    char *key;
    for (key = (char *) zlist_first (keys); key; key = (char *) zlist_next (keys))
        zhash_delete (hash, key);
    zlist_destroy (&keys);
}

//  Whether to act on a beacon from `uuid` at `ipaddress` now. A peer not yet
//  known and heard only through a tunnel waits UNINET_TUNNEL_HOLD_MS for its
//  local beacon. A peer connected through a tunnel and now heard locally is
//  dropped here (once) so the caller reconnects it at the local address.
static bool
s_uninet_beacon_ok (zyre_node_t *self, const char *uuid, const char *ipaddress)
{
    zyre_peer_t *peer = (zyre_peer_t *) zhash_lookup (self->peers, uuid);
    if (peer) {
        if (zhash_lookup (self->uninet_moved_local, uuid)
        ||  s_uninet_via_tunnel (ipaddress)
        || !s_uninet_via_tunnel (zyre_peer_endpoint (peer)))
            return true;
        if (zhash_size (self->uninet_moved_local) > 256)
            s_uninet_empty (self->uninet_moved_local);
        zhash_insert (self->uninet_moved_local, uuid, "1");
        zsys_info ("(%s) %s is on the local network too: reconnecting at %s instead of %s",
                   self->name, zyre_peer_name (peer)? zyre_peer_name (peer): uuid, ipaddress, zyre_peer_endpoint (peer));
        zyre_node_remove_peer (self, peer);
        return true;
    }
    if (!s_uninet_via_tunnel (ipaddress)) {
        zhash_delete (self->uninet_tunnel_seen, uuid);
        return true;
    }
    int64_t now = zclock_mono ();
    char *first = (char *) zhash_lookup (self->uninet_tunnel_seen, uuid);
    int64_t since = first? now - (int64_t) strtoll (first, NULL, 10): -1;
    if (since >= UNINET_TUNNEL_HOLD_MS && since < 60000) {
        zhash_delete (self->uninet_tunnel_seen, uuid);
        return true;
    }
    if (since < 0 || since >= 60000) {
        //  First heard now (or so long ago that it is a new visit)
        if (zhash_size (self->uninet_tunnel_seen) > 256)
            s_uninet_empty (self->uninet_tunnel_seen);
        char stamp [32];
        snprintf (stamp, sizeof (stamp), "%lld", (long long) now);
        zhash_update (self->uninet_tunnel_seen, uuid, stamp);
    }
    return false;
}

//  Handle beacon data

static void
zyre_node_recv_beacon (zyre_node_t *self)]=])
    set(port_old "    if (beacon.port) {\n        char endpoint [NI_MAXHOST];")
    set(port_new "    if (beacon.port && !s_uninet_beacon_ok (self, zuuid_str (uuid), ipaddress)) {\n        //  UNINET PATCH: held, waiting for the peer's local beacon\n    }\n    else\n    if (beacon.port) {\n        char endpoint [NI_MAXHOST];")
    foreach(pair fields init free head port)
        string(FIND "${text}" "${${pair}_old}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "patch_zyre: zyre_node.c is not the zyre v2.0.1 this patch was written for (${pair})")
        endif()
        string(REPLACE "${${pair}_old}" "${${pair}_new}" text "${text}")
    endforeach()
    file(WRITE "${zyre_node}" "${text}")
endif()
