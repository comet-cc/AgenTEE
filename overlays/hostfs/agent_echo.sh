#!/bin/bash
DEVICE="${DEVICE:-}"
usage() {
  cat <<EOF
Usage: $(basename "$0") [options]
Options:
  --device PATH             Path to the device file (default: $DEVICE)
EOF
}   
while [[ $# -gt 0 ]]; do
    case $1 in
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

ITERS="${ITERS:-1000}"
WARMUP="${WARMUP:-50}"
SIZES="${SIZES:-64,256,1024,4096,8192,16384}"

if [[ -z "$DEVICE" ]]; then
  python3 itinerary_agent/itinerary_agent.py --bench --iters "$ITERS" --warmup "$WARMUP" \
  --sizes "$SIZES" --csv exp1.csv --channel 0 --region-size 20000
else
    python3 itinerary_agent/itinerary_agent.py --bench --iters "$ITERS" --warmup "$WARMUP" \
    --sizes "$SIZES" --csv exp1.csv --channel 0 --device "$DEVICE" --region-size 20000
fi
