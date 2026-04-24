#!/bin/bash
# Measure CPU time (self+children), wall time, and CPU cycles for 10 inferences.

set -euo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if [ $# -lt 1 ]; then
  echo "Usage: $0 <model>"
  exit 1
fi
model=$1

# ----- queries -----
QUERY1="How can we reduce air pollution?"
QUERY2="Describe the structure of an atom."
QUERY3="Give three tips for staying healthy."
QUERY4="What are the three primary colors?"
QUERY5="What is the capital of France?"
QUERY6="Explain the process of photosynthesis."
QUERY7="What causes lightning during a thunderstorm?"
QUERY8="Define artificial intelligence in simple terms."
QUERY9="List three major types of renewable energy."
QUERY10="Why does the moon have phases?"

# ----- helpers -----
HERTZ=$(getconf CLK_TCK)

cpu_ticks_total() {
  read -r ut st cut cst < <(awk '{print $14,$15,$16,$17}' "/proc/$$/stat")
  echo $((ut + st + cut + cst))
}

wall_now_ns() { date +%s%N; }

run_block() {
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY1" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY2" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY3" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY4" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY5" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY6" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY7" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY8" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY9" -n 100
  nice -n -20 "$DIR/llama-cli" -m "$model" -p "$QUERY10" -n 100
}

# Export function & env so a subshell (perf) can run it
export -f run_block
export DIR model QUERY1 QUERY2 QUERY3 QUERY4 QUERY5 QUERY6 QUERY7 QUERY8 QUERY9 QUERY10

# ----- take t0 snapshots -----
t0_ticks=$(cpu_ticks_total)
t0_wall_ns=$(wall_now_ns)

# ----- run block under perf if available -----
cycles=""; instructions=""
perf_out=""
if command -v perf >/dev/null 2>&1; then
  perf_out="$(mktemp)"
  LC_ALL=C perf stat --no-big-num -x, -e cycles,instructions -o "$perf_out" \
    bash -c run_block >/dev/null
else
  run_block
fi

# ----- take t1 snapshots -----
t1_ticks=$(cpu_ticks_total)
t1_wall_ns=$(wall_now_ns)

# ----- compute CPU and wall time -----
delta_ticks=$((t1_ticks - t0_ticks))
cpu_seconds=$(awk -v t="$delta_ticks" -v h="$HERTZ" 'BEGIN { printf "%.6f", t / h }')
wall_seconds=$(awk -v dns="$((t1_wall_ns - t0_wall_ns))" 'BEGIN { printf "%.6f", dns/1e9 }')

cpu_util_pct=$(awk -v c="$cpu_seconds" -v w="$wall_seconds" '
  BEGIN { if (w>0) printf "%.1f", 100.0*c/w; else print "0.0" }')

# ----- parse perf results (if present) -----
if [[ -n "${perf_out}" && -s "${perf_out}" ]]; then
  cycles=$(awk -F, '$3=="cycles"{gsub(/[[:space:]]/,"",$1); print $1}' "$perf_out")
  instructions=$(awk -F, '$3=="instructions"{gsub(/[[:space:]]/,"",$1); print $1}' "$perf_out")
  rm -f "$perf_out"
fi

# Compute IPC if both cycles & instructions are known
ipc=""
if [[ -n "$cycles" && -n "$instructions" ]]; then
  ipc=$(awk -v i="$instructions" -v c="$cycles" 'BEGIN { if (c>0) printf "%.3f", i/c; }')
fi

# ----- print summary -----
echo "================ Summary ================"
printf "Wall time:         %s s\n" "$wall_seconds"
printf "CPU time:          %s s (self + children)\n" "$cpu_seconds"
printf "CPU utilization:   %s %% of 1 core\n" "$cpu_util_pct"
if [[ -n "$cycles" ]]; then
  printf "CPU cycles:        %s\n" "$cycles"
fi
if [[ -n "$instructions" ]]; then
  printf "Instructions:      %s\n" "$instructions"
fi
if [[ -n "$ipc" ]]; then
  printf "IPC:               %s\n" "$ipc"
fi
echo "========================================="

