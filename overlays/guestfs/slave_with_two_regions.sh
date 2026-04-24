#!/bin/bash
set -x
insmod /root/rsi-shared-memory-set.ko flag=1 rd=0x11 region_index=0
insmod /root/shared-memory-driver-reserved-ipa.ko
insmod /root/shmem-pci-driver.ko
