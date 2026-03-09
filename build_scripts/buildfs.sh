#!/bin/bash

DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
set -x
cd $DIR
sudo debos --artifactdir=out -t architecture:arm64 ospack-debian.yaml
sudo debos --artifactdir=out -t architecture:arm64 -t platform:rock5b-rk3588 opencca-image-rockchip-rk3588.yaml --memory 8Gb
