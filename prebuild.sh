#!/bin/bash
DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

# Apply patches to the relevant repositories
cd $DIR/../linux
git am ../manifest/patches/host-linux/*.patch

cd $DIR/../linux-guest
git am ../manifest/patches/guest-linux/*.patch

cd $DIR/../debian-image-recipes
git am ../manifest/patches/debian-image-recipes/*.patch


cd $DIR/../opencca-build
git am ../manifest/patches/opencca-build/*.patch
# Update submodules
cd $DIR/../tf-rmm
git submodule update --init --recursive

cd $DIR/../debian-image-recipes
./download-rock5b-artifacts.sh

mkdir -p $DIR/../snapshot
mkdir -p $DIR/../tmp
mkdir -p $DIR/../debos-fs/overlay
mkdir -p $DIR/../debian-image-recipes/out
mkdir -p $DIR/../debian-image-recipes/overlays/AgenTEE
mkdir -p $DIR/../debian-image-recipes/overlays/AgenTEE/VM_image
mkdir -p $DIR/../debian-image-recipes/overlays/AgenTEE/shared_with_VM
