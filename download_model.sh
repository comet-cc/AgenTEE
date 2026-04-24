#!/bin/bash
set -x
DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
TARGET_DIR="$DIR/../debian-image-recipes/overlays/CAEC/shared_with_VM"
while getopts "t:m:" opt; do
	case $opt in
	m)
		model=$OPTARG
		;;
	t)
		HF_TOKEN=$OPTARG
		;;
	esac
done

if [ -z "${model}" ]; then
        echo "Error: -m option is required. model repository must be provided"
        exit 1
fi

if [ -z "${HF_TOKEN}" ]; then
	echo "Error: -t option is required, token must be provided"
	exit 1
fi

python3 $DIR/convert_gguf.py $model $HF_TOKEN --outtype q8_0 --llama_cpp_dir $DIR/../llamacpp
cp $DIR/../tmp/* $TARGET_DIR/.

