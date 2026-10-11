#!/usr/bin/env python3
#
# Peers that come and go as fast as the server lets them, for as many seconds as asked.
#
#   churn.py <path to socket> <seconds>

import sys
import time
import socket

moment = time.time() + float(sys.argv[2])

while time.time() < moment:
    try:
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        peer.connect(sys.argv[1])
        time.sleep(0.0005)
        peer.close()
    except OSError:
        # The server is not there yet or has gone
        time.sleep(0.01)
