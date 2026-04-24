#!/bin/bash
set -x
insmod /root/caec-module.ko flag=1 rid=0x11 dbg=3
insmod /root/shmem-pci-driver.ko
