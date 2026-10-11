#!/usr/bin/env python3
#
# A stand-in for ivshmem-server where none is installed: protocol version 0, one vector,
# a region of 1M in a memfd, and the lowest free ID for every peer that connects.
#
#   server.py <path to socket>

import os
import sys
import socket
import struct
import select

def send(connection, value, handle = None):
    try:
        socket.send_fds(connection, [struct.pack("<q", value)], [handle] if handle is not None else [])
    except OSError:
        # The peer is going, and the server will see it shortly
        pass

path = sys.argv[1]

if os.path.exists(path):
    os.unlink(path)

listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.bind(path)
listener.listen(64)

memory = os.memfd_create("ivshmem")
os.ftruncate(memory, 1 << 20)

peers = { }  # ID -> (connection, eventfd)

while True:
    ready, _, _ = select.select([listener] + [connection for connection, _ in peers.values()], [ ], [ ])

    for item in ready:
        if item is listener:
            connection, _ = listener.accept()
            identifier    = min(set(range(len(peers) + 1)) - set(peers))
            handle        = os.eventfd(0)

            send(connection, 0)
            send(connection, identifier)
            send(connection, -1, memory)

            for other, (peer, eventfd) in peers.items():
                send(connection, other, eventfd)
                send(peer, identifier, handle)

            send(connection, identifier, handle)
            peers[identifier] = (connection, handle)
            continue

        for identifier, (connection, handle) in list(peers.items()):
            if connection is item:
                try:
                    gone = not connection.recv(1)
                except OSError:
                    gone = True

                if gone:
                    del peers[identifier]
                    os.close(handle)
                    connection.close()

                    for peer, _ in peers.values():
                        send(peer, identifier)
