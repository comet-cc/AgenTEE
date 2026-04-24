import os
import argparse
import time
import csv
from datetime import datetime

from langchain_community.llms import LlamaCpp
from CSM import CSM_send, CSM_receive, channel_layout

DEFAULT_REGION_SIZE = 4096
DEFAULT_PAYLOAD_OFF = 24


def parse_args():
    p = argparse.ArgumentParser(description="Itinerary model worker (multi-channel)")
    p.add_argument("--model", type=str, default="../models/gpt2-large-q8_0.gguf")
    p.add_argument("--device", type=str, default="/tmp/shmfile")
    p.add_argument("--channel", type=int, default=0)
    p.add_argument("--req-channel", type=int, default=None, help="Request mailbox channel (default: --channel)")
    p.add_argument("--resp-channel", type=int, default=None, help="Response mailbox channel (default: --channel + 1)")
    p.add_argument("--region-size", type=int, default=DEFAULT_REGION_SIZE)
    p.add_argument("--payload-off", type=int, default=DEFAULT_PAYLOAD_OFF)
    p.add_argument("--base0", type=int, default=0)
    p.add_argument("--echo", action="store_true", help="Echo prompt back (no inference), for Exp1")
    p.add_argument(
        "--echo-log-every",
        type=int,
        default=0,
        help="When --echo is set, log every N requests (0 = disable per-request logs)",
    )
    p.add_argument(
        "--max-inferences",
        type=int,
        default=0,
        help="Maximum number of requests to process before exit (0 = unlimited)",
    )
    p.add_argument("--infer-csv", type=str, default="", help="Append per-inference timings to CSV")
    return p.parse_args()


def ensure_backing_file(device: str, min_size: int) -> None:
    if not os.path.exists(device):
        with open(device, "wb") as f:
            f.write(b"\x00" * min_size)
        return
    try:
        st = os.stat(device)
        if os.path.isfile(device) and st.st_size < min_size:
            with open(device, "ab") as f:
                f.write(b"\x00" * (min_size - st.st_size))
    except Exception:
        pass


def append_infer_rows(csv_path: str, rows: list) -> None:
    if not csv_path or not rows:
        return
    write_header = not os.path.exists(csv_path)
    with open(csv_path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        if write_header:
            w.writeheader()
        w.writerows(rows)


def main():
    args = parse_args()
    req_channel = args.req_channel if args.req_channel is not None else args.channel
    resp_channel = args.resp_channel if args.resp_channel is not None else (args.channel + 1)
    if req_channel == resp_channel:
        raise ValueError("Request and response channels must be different")

    req_layout = channel_layout(req_channel, region_size=args.region_size)
    resp_layout = channel_layout(resp_channel, region_size=args.region_size)
    max_end = max(req_layout.base + req_layout.region_size, resp_layout.base + resp_layout.region_size)
    ensure_backing_file(args.device, max_end)

    llm = None
    if not args.echo:
        llm = LlamaCpp(
            model_path=args.model,
            n_ctx=1024,
            n_threads=4,
            max_tokens=384,
            temperature=0.3,
            top_p=0.9,
            stop=[],
            verbose=False,
        )

    fd = os.open(args.device, os.O_RDWR)
    served = 0
    rows = []

    print(
        f"[ItineraryModel] Ready (model={args.model}, device={args.device}, req_ch={req_channel}, resp_ch={resp_channel}, "
        f"region_size={args.region_size}, req_base=0x{req_layout.base:x}, resp_base=0x{resp_layout.base:x}, "
        f"req_cap={req_layout.capacity}, resp_cap={resp_layout.capacity}, echo={args.echo}, "
        f"max_inferences={args.max_inferences})"
    )

    try:
        while True:
            if args.max_inferences > 0 and served >= args.max_inferences:
                print(f"[ItineraryModel] max inferences reached ({served}); exiting.")
                break

            prompt_data = CSM_receive(fd, req_layout)
            prompt = prompt_data.decode("utf-8", errors="replace")

            t0 = time.perf_counter_ns()
            if args.echo:
                output = prompt
            else:
                try:
                    output = llm(prompt).strip()
                except Exception as e:
                    output = f"[error] inference failed: {e}"
            t1 = time.perf_counter_ns()

            served += 1
            infer_time_ns = t1 - t0
            should_log = (not args.echo) or (
                args.echo_log_every > 0 and (served % args.echo_log_every == 0)
            )
            if should_log:
                print(f"[ItineraryModel] inference={served} infer_time={infer_time_ns/1e9:.3f}s")
            rows.append(
                {
                    "agent": "itinerary_model",
                    "inference_id": served,
                    "timestamp": datetime.now().isoformat(timespec="seconds"),
                    "model": args.model,
                    "channel": req_channel,
                    "prompt_bytes": len(prompt_data),
                    "output_bytes": len(output.encode("utf-8", errors="replace")),
                    "infer_time_ns": int(infer_time_ns),
                    "infer_time_s": infer_time_ns / 1e9,
                }
            )

            CSM_send(fd, resp_layout, output.encode("utf-8"))
    finally:
        append_infer_rows(args.infer_csv, rows)
        os.close(fd)


if __name__ == "__main__":
    main()
