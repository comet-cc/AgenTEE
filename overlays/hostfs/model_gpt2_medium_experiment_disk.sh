#!/bin/bash
set -x
sudo nice -n -20 taskset -c 0 ./lkvm run --realm -c 1 -m 1000 -k /home/user/VM_image/Image -d /home/user/VM_image/VM-fs.img \
--9p /home/user/shared_with_VM,sh --irqchip=gicv3-its --loglevel=debug --restricted_mem -s 2 \
-p "root=/dev/vda1 rw rootwait console=ttyS0 cca_reserve=top,2M" --dump-dtb=dtb_master.dtb \
--pmu --pmu-counters 6
