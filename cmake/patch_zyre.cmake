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
