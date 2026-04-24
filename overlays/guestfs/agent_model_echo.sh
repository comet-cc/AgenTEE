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
if [[ -z "$DEVICE" ]]; then
    python3 itinerary_agent/itinerary_model.py --echo --channel 0 --region-size 20000 
else
    python3 itinerary_agent/itinerary_model.py --echo --channel 0 --region-size 20000 \
    --device "$DEVICE"
fi
