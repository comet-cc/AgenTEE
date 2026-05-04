#!/bin/bash
set -x
sudo nice -n -20 taskset -c 1 ./lkvm run --realm -c 1 -m 250 -k /home/user/VM_image/Image -d /home/user/VM_image/VM-fs2.img \
--9p /home/user/shared_with_VM,sh --irqchip=gicv3-its --loglevel=debug --restricted_mem -r 2 \
-p "root=/dev/vda1 rw rootwait console=ttyS0 cca_reserve=top,2M" --dump-dtb=dtb_master.dtb \
--pmu --pmu-counters 6
