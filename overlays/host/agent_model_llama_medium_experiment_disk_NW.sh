#!/bin/bash
set -x
sudo nice -n -20 taskset -c 0 ./lkvm run -c 1 -m 1200 -k /home/user/VM_image/Image -d /home/user/VM_image/VM-fs.img \
--9p /home/user/shared_with_VM,sh --irqchip=gicv3-its --loglevel=debug \
--shmem 0x30000000:10M:file=/dev/shm/buf:create:mlock \
-p "root=/dev/vda1 rw rootwait console=ttyS0 cca_reserve=top,2M" --dump-dtb=dtb_master.dtb 
