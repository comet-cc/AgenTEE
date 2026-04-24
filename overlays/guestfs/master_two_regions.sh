#!/bin/bash
set -x
insmod /root/rsi-shared-memory-set.ko flag=0 rd=0x22 region_index=0
rmmod /root/rsi-shared-memory-set.ko
insmod /root/rsi-shared-memory-set.ko flag=0 rd=0x33 region_index=1
insmod /root/pmu_enable.ko
insmod /root/shared-memory-driver-reserved-ipa.ko
insmod /root/shmem-pci-driver.ko
