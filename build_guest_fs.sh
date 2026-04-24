#!/bin/bash

DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )
OVERLAY="$DIR/debos-fs/overlay"
rsync -av --delete $DIR/External_modules/*.ko $OVERLAY/.
rsync -av --delete $DIR/External_modules/user-space/out/* $OVERLAY/.
rsync -av --delete $DIR/manifest/overlays/guestfs/* $OVERLAY/.
rsync -av --delete $DIR/SecGPT $OVERLAY/.
#rsync -av --delete /home/netsys1/Multi-Realm-LLM-source/Multi-Realm-LLM/suplementary-binaries/out/* $OVERLAY/.

cd $DIR/debos-fs
sudo ./build.sh --imgsize 4000MB --format ext4 \
  --custom-script ./secgpt-arm64-install.sh \
  --py-enable 0 --imgname VM-fs.img

sudo cp $DIR/debos-fs/out/VM-fs.img $DIR/debos-fs/out/VM-fs2.img
