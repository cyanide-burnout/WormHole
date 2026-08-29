#!/bin/bash

INTERFACE=$(ip a | grep -m 1 -o -E "ens[0-9a-z]+")

ip address add 192.168.100.2/24 dev $INTERFACE
ip link set $INTERFACE up
ip route add default via 192.168.100.1 dev $INTERFACE
echo nameserver 8.8.8.8 > /etc/resolv.conf

apt-get update
apt-get install -y build-essential pkg-config liburing-dev linux-headers-$(uname -r)

cd /opt/WormHole/Driver

make clean
make
make install

cd /opt/WormHole/Test

make FASTRING=../Ring WORMHOLE=../Library clean
make FASTRING=../Ring WORMHOLE=../Library

./test /dev/uio0

# shutdown -hP 0
