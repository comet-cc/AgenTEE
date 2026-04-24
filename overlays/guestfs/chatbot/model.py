import os
import argparse
import time
import csv
from datetime import datetime

from CSM import CSM_send, CSM_receive, channel_layout

DEFAULT_REGION_SIZE = 4096
DEFAULT_PAYLOAD_OFF = 24


def parse_args():
    parser = argparse.ArgumentParser(description="LLM mailbox worker (multi-channel)")
    parser.add_argument(
        "--model",
        type=str,
        default="../models/gpt2-large-q8_0.gguf",
        help="Path to GGUF model file",
    )
    parser.add_argument(
        "--device",
        type=str,
        default="/tmp/shmfile",
        help="Path to shared memory / device file",
    )
    parser.add_argument(
        "--channel",
        type=int,
        default=0,
        help="Mailbox channel id (each channel uses a different region on the device)",
    )
    parser.add_argument(
        "--req-channel",
        type=int,
        default=None,
        help="Request mailbox channel (default: --channel)",
    )
    parser.add_argument(
        "--resp-channel",
        type=int,
        default=None,
        help="Response mailbox channel (default: --channel + 1)",
    )
    parser.add_argument(
        "--region-size",
        type=int,
        default=DEFAULT_REGION_SIZE,
        help="Bytes reserved per channel (must match agent side)",
    )
    parser.add_argument(
        "--payload-off",
        type=int,
        default=DEFAULT_PAYLOAD_OFF,
        help="Payload offset within each channel region (>= 24). Use 24 to maximize payload space.",
    )
    parser.add_argument(
        "--base0",
        type=int,
        default=0,
        help="Base offset for channel 0 (advanced; usually keep 0)",
    )
    parser.add_argument(
        "--echo",
        action="store_true",
        help="Echo prompt back (no inference). Used for Exp1 IPC microbenchmark.",
    )
    parser.add_argument(
        "--n-ctx",
        type=int,
        default=1024,
        help="Context size for inference (Exp2).",
    )
    parser.add_argument(
        "--n-threads",
        type=int,
        default=4,
        help="Threads for inference (Exp2).",
    )
    parser.add_argument(
        "--max-tokens",
        type=int,
        default=256,
        help="Max tokens to generate (Exp2).",
    )
    parser.add_argument(
        "--max-inferences",
        type=int,
        default=0,
        help="Maximum number of requests to process before exit (0 = unlimited)",
    )
    parser.add_argument(
        "--infer-csv",
        type=str,
        default="",
        help="Append per-inference timings to CSV",
    )
    return parser.parse_args()


def ensure_backing_file(device: str, min_size: int) -> None:
    """
    For regular-file testing, make sure the file exists and is at least min_size bytes.
    If it's a real device, this may fail/irrelevant — we ignore safely.
    """
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
    device = args.device
    model_path = args.model

    req_channel = args.req_channel if args.req_channel is not None else args.channel
    resp_channel = args.resp_channel if args.resp_channel is not None else (args.channel + 1)
    if req_channel == resp_channel:
        raise ValueError("Request and response channels must be different")

    req_layout = channel_layout(req_channel, region_size=args.region_size)
    resp_layout = channel_layout(resp_channel, region_size=args.region_size)

    # Ensure file exists / big enough for both channel regions (regular-file testing)
    max_end = max(req_layout.base + req_layout.region_size, resp_layout.base + resp_layout.region_size)
    ensure_backing_file(device, max_end)

    llm = None
    if not args.echo:
        # Import only when needed (so Exp1 runs without llama deps)
        from langchain_community.llms import LlamaCpp

        llm = LlamaCpp(
            model_path=model_path,
            n_ctx=args.n_ctx,
            n_threads=args.n_threads,
            max_tokens=args.max_tokens,
            temperature=0.2,
            top_p=0.9,
            stop=[],
            verbose=False,
        )

    fd = os.open(device, os.O_RDWR)
    served = 0
    rows = []

    print(
        f"[Model] Ready (model={model_path}, device={device}, "
        f"req_ch={req_channel}, resp_ch={resp_channel}, "
        f"region_size={args.region_size}, req_base=0x{req_layout.base:x}, "
        f"resp_base=0x{resp_layout.base:x}, req_cap={req_layout.capacity}, "
        f"resp_cap={resp_layout.capacity}, echo={args.echo}, max_inferences={args.max_inferences})"
    )

    try:
        while True:
            if args.max_inferences > 0 and served >= args.max_inferences:
                print(f"[Model] max inferences reached ({served}); exiting.")
                break

            print(f"[Model req_ch={req_channel}] Waiting for prompt...")
            prompt_data = CSM_receive(fd, req_layout)
            prompt = prompt_data.decode("utf-8", errors="replace")
            print(f"[Model req_ch={req_channel}] Got prompt bytes={len(prompt_data)}")

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
            print(f"[Model] inference={served} infer_time={infer_time_ns/1e9:.3f}s")
            rows.append(
                {
                    "agent": "chatbot_model",
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
