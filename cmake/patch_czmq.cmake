# A fix to czmq v4.2.1's zbeacon, applied to its source tree after it is
# fetched. Idempotent.
#
#     cmake -P patch_czmq.cmake        (run in czmq's source directory)
#
# zbeacon opens two UDP sockets, one to receive and one only to send, and binds
# both to the beacon port with SO_REUSEPORT. A broadcast reaches both, but
# Linux hands each UNICAST datagram to one of them, chosen by a hash of its
# source. Discovery on a VPN is unicast (UniNet sweeps the subnet), so a peer
# whose beacons hash to the send socket, which is never read, is never found:
# its datagrams pile up unread in that socket. Seen on the ThermoNav server,
# which never discovered a headset whose beacons all went there.
#
# The send socket only sends, and receivers take the port from the beacon's
# payload, not from the datagram, so it is bound to an ephemeral port instead.

set(file "${CMAKE_CURRENT_SOURCE_DIR}/src/zbeacon.c")
file(READ "${file}" text)
if(text MATCHES "UNINET PATCH")
    return()
endif()

set(old "        if (bind (self->udpsock_send, bind_to->ai_addr, bind_to->ai_addrlen) ||")
set(new [=[        //  UNINET PATCH: bind the send-only socket to an ephemeral port. On the
        //  beacon port it would share unicast datagrams with udpsock and never
        //  read its share (see UniNet cmake/patch_czmq.cmake).
        struct sockaddr_storage send_address;
        memcpy (&send_address, bind_to->ai_addr, bind_to->ai_addrlen);
        if (send_address.ss_family == AF_INET6)
            ((struct sockaddr_in6 *) &send_address)->sin6_port = 0;
        else
            ((struct sockaddr_in *) &send_address)->sin_port = 0;
        if (bind (self->udpsock_send, (struct sockaddr *) &send_address, bind_to->ai_addrlen) ||]=])
string(FIND "${text}" "${old}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "patch_czmq: zbeacon.c is not the czmq v4.2.1 this patch was written for")
endif()
string(REPLACE "${old}" "${new}" text "${text}")
file(WRITE "${file}" "${text}")
