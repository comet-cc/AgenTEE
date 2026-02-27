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
    python3 itinerary_agent/itinerary_agent.py --workload workloads/itinerary_workload.json \
    --repeat 3 --e2e-csv exp2_itin_csm.csv --channel 0 
    else
        python3 itinerary_agent/itinerary_agent.py --workload workloads/itinerary_workload.json \
        --repeat 3 --e2e-csv exp2_itin_csm.csv --channel 0 --device "$DEVICE"
    fi  
