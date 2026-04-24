#!/bin/bash
set -x
mount -t 9p sh ./shared_with_VM/.
systemctl daemon-reload
./master_caec.sh
./inference_test_measure.sh /dev/shmem0_confidential
