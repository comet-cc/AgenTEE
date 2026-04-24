#!/bin/bash
MODEL="${MODEL:-/mnt/gpt2-medium-q8_0.gguf}"
DEVICE="${DEVICE:-}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]
Options:
  --model PATH              Path to the model file (default: $MODEL)
  --device PATH             Path to the device file (default: $DEVICE)
EOF
} 

while [[ $# -gt 0 ]]; do
    case $1 in
        --model)
        MODEL="$2"
        shift 2
        ;;
        --device)
        DEVICE="$2"
        shift 2
        ;;
        *)
        echo "Unknown option: $1"
        usage
        exit 1
        ;;
    esac
done

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"
if [[ -z "$DEVICE" ]]; then
python3 ./chatbot/model.py --model "$MODEL" --channel 0 \
--infer-csv ./inference_chatbot.csv --max-inferences 9 
else
python3 ./chatbot/model.py --model "$MODEL" --channel 0 \
--infer-csv ./inference_chatbot.csv --max-inferences 9 --device "$DEVICE"
fi