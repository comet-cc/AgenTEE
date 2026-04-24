#!/bin/bash
set -x
sudo nice -n -20 taskset -c 0 ./lkvm run --realm -c 1 -m 570 -k /home/user/VM_image/Image -d /home/user/VM_image/VM-fs.img \
--9p /home/user/shared_with_VM,sh --irqchip=gicv3-its --loglevel=debug --restricted_mem -s 440 \
--shmem 0x30000000:190M:file=/dev/shm/buf:create:mlock \
-p "root=/dev/vda1 rw rootwait console=ttyS0 cca_reserve=top,440M" --dump-dtb=dtb_master.dtb \
--pmu --pmu-counters 6
