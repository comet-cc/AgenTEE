#!/bin/bash
DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

cd $DIR/..

make -f opencca-build/docker/Makefile build
make -f opencca-build/docker/Makefile start
make -f opencca-build/docker/Makefile enter
