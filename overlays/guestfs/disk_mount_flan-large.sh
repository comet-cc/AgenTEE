#!/bin/bash
set -x
mount -t 9p sh ./shared_with_VM/.
systemctl daemon-reload
mount -t tmpfs -o size=900M tmpfs /mnt/
systemctl daemon-reload
cp ./shared_with_VM/flan-t5-large-q8_0.gguf /mnt/.
./inference_test_measure.sh /mnt/flan-t5-large-q8_0.gguf
