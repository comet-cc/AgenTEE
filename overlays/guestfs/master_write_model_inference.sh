#!/bin/bash
set -x
mount -t 9p sh ./shared_with_VM/.
systemctl daemon-reload
./user_test write /dev/shmem0_confidential \
./shared_with_VM/gpt2-q8_0.gguf
./inference_test_measure.sh /dev/shmem0_confidential
