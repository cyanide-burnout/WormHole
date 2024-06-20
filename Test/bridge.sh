#!/bin/bash

ip address add 192.168.100.1/24 dev $1
ip link set $1 up

sysctl net.ipv4.ip_forward=1

INTERFACE=$(ip address | grep -m 1 -o -E "enp[0-9a-z]+")
iptables -t nat -A POSTROUTING -o $INTERFACE -j MASQUERADE
iptables -A FORWARD -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
iptables -A FORWARD -o $INTERFACE -i $1 -j ACCEPT
