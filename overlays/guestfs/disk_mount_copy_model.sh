#!/bin/bash
set -x
mount -t 9p sh ./shared_with_VM/.
systemctl daemon-reload
mount -t tmpfs -o size=440M tmpfs /mnt/
systemctl daemon-reload
cp ./shared_with_VM/gpt2-medium-q8_0.gguf /mnt/.