#!/bin/bash
DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )
OVERLAY="$DIR/debian-image-recipes/overlays/AgenTEE"

cd $DIR/debian-image-recipes
rsync -av --delete $DIR/manifest/overlays/hostfs/* $OVERLAY/.
sudo rsync -av --delete $DIR/debos-fs/out/* $OVERLAY/VM_image/.
rsync -av --delete $DIR/snapshot/Image-guest $OVERLAY/VM_image/Image

sudo ./buildfs.sh
