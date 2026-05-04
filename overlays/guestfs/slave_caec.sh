#!/bin/bash
set -x
insmod /root/caec-module.ko flag=1 region_id=0x11122 dbg=3
insmod /root/shmem-pci-driver.ko
