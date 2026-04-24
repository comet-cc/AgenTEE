#!/bin/bash
set -x
sudo nice -n -20 taskset -c 0 ./lkvm run --realm -c 1 -m 650 -k /home/user/VM_image/Image -i /home/user/VM_image/VM-fs.cpio.gz \
--9p /home/user/shared_with_VM,sh --irqchip=gicv3-its --loglevel=debug --restricted_mem -s 340 \
--shmem 0x30000000:32M:file=/dev/shm/buf:create:mlock \
-p "rdinit=/sbin/init console=ttyS0"  --dump-dtb=dtb.dtb \
--pmu --pmu-counters 6
