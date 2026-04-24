#!/bin/bash
set -x
insmod /root/caec-module.ko flag=0 rid=0x22 dbg=3
insmod /root/pmu_enable.ko
insmod /root/shmem-pci-driver.ko
