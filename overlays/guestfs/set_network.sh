#!/bin/bash
set -x
ip link set enp0s2 up
ip addr flush dev enp0s2
ip addr add 192.168.100.2/24 dev enp0s2
ip route flush default
ip route add default via 192.168.100.1 dev enp0s2
printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' > /etc/resolv.conf
date -s "2026-04-06 12:00:00"
