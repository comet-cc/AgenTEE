import os
import argparse
import time
import csv
from statistics import median

from CSM import CSM_receive, CSM_send, channel_layout
try:
    from langchain.prompts import PromptTemplate  # older langchain
except Exception:
    from langchain_core.prompts import PromptTemplate  # langchain 1.x

DEFAULT_REGION_SIZE = 4096
DEFAULT_PAYLOAD_OFF = 24

prompt_tmpl = PromptTemplate(
    input_variables=["text"],
    template="Q: {text}\nA:",
)


def parse_args():
    parser = argparse.ArgumentParser(description="CSM agent (multi-channel)")
    parser.add_argument("--device", type=str, default="/tmp/shmfile")
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--req-channel", type=int, default=None, help="Request mailbox channel (default: --channel)")
    parser.add_argument("--resp-channel", type=int, default=None, help="Response mailbox channel (default: --channel + 1)")
    parser.add_argument("--region-size", type=int, default=DEFAULT_REGION_SIZE)
    parser.add_argument("--payload-off", type=int, default=DEFAULT_PAYLOAD_OFF)
    parser.add_argument("--base0", type=int, default=0)

    # Exp1 bench args
    parser.add_argument("--bench", action="store_true", help="Run IPC benchmark (Exp1)")
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--sizes", type=str, default="64,256,1024,4096,8192,16384")
    parser.add_argument("--csv", type=str, default="")

    # Exp2 workload args
    parser.add_argument("--workload", type=str, default="", help="Path to workload file (one prompt per line)")
    parser.add_argument("--repeat", type=int, default=1, help="Repeat workload N times")
    parser.add_argument("--e2e-csv", type=str, default="", help="Append Exp2 per-task timings to CSV")

    return parser.parse_args()


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


def p95_ns(values_ns):
    if not values_ns:
        return 0
    vals = sorted(values_ns)
    k = int(0.95 * (len(vals) - 1))
    return vals[k]


def run_bench(fd, req_layout, resp_layout, sizes, iters, warmup, csv_path):
    out_rows = []
    for sz in sizes:
        payload = b"a" * sz
        rtts = []
        for _ in range(iters):
            t0 = time.perf_counter_ns()
            CSM_send(fd, req_layout, payload)
            resp = CSM_receive(fd, resp_layout)
            text = resp.decode("utf-8", errors="replace")
            print(f"[Resp] bytes={len(resp)} head={text[:80]!r}")
            t1 = time.perf_counter_ns()
            rtts.append(t1 - t0)
            if len(resp) != len(payload):
                raise RuntimeError(f"Echo length mismatch: sent={len(payload)} recv={len(resp)}")

        rtts = rtts[warmup:] if warmup < len(rtts) else rtts
        med = median(rtts)
        p95 = p95_ns(rtts)
        print(f"[Bench chatbot] size={sz}B median={med/1000:.1f}us p95={p95/1000:.1f}us n={len(rtts)}")

        out_rows.append(
            {
                "agent": "chatbot",
                "size_bytes": sz,
                "iters": iters,
                "warmup": warmup,
                "median_ns": int(med),
                "p95_ns": int(p95),
            }
        )

    if csv_path:
        write_header = not os.path.exists(csv_path)
        with open(csv_path, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
            if write_header:
                w.writeheader()
            w.writerows(out_rows)


def load_chat_workload(path: str):
    prompts = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            prompts.append(s)
    return prompts


def append_e2e_rows(csv_path: str, rows: list):
    if not csv_path or not rows:
        return
    write_header = not os.path.exists(csv_path)
    with open(csv_path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        if write_header:
            w.writeheader()
        w.writerows(rows)


def run_workload_e2e(fd, req_layout, resp_layout, prompts, repeat, e2e_csv):
    timings = []
    task_idx = 0

    for r in range(repeat):
        for prompt_text in prompts:
            task_idx += 1
            prompt = prompt_tmpl.format(text=prompt_text)
            payload = prompt.encode("utf-8")

            t0 = time.perf_counter_ns()
            CSM_send(fd, req_layout, payload)
            resp = CSM_receive(fd, resp_layout)
            t1 = time.perf_counter_ns()

            elapsed_ns = t1 - t0
            timings.append(elapsed_ns)

            print(f"[Exp2 chatbot] task={task_idx} bytes={len(payload)} time={elapsed_ns/1e9:.3f}s")
            # Optional: keep output short in batch mode
            _ = resp  # you can print resp.decode(...) if you want

    avg_s = (sum(timings) / len(timings)) / 1e9 if timings else 0.0
    print(f"[Exp2 chatbot] done tasks={len(timings)} avg_time={avg_s:.3f}s")

    rows = []
    for i, ns in enumerate(timings, start=1):
        rows.append(
            {
                "agent": "chatbot",
                "task_id": i,
                "time_ns": int(ns),
                "time_s": ns / 1e9,
            }
        )
    append_e2e_rows(e2e_csv, rows)


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
    fd = os.open(args.device, os.O_RDWR)

    print(
        f"[Agent] Ready (device={args.device}, req_ch={req_channel}, resp_ch={resp_channel}, "
        f"region_size={args.region_size}, req_base=0x{req_layout.base:x}, resp_base=0x{resp_layout.base:x}, "
        f"req_cap={req_layout.capacity}, resp_cap={resp_layout.capacity})"
    )

    try:
        if args.bench:
            sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
            run_bench(fd, req_layout, resp_layout, sizes, args.iters, args.warmup, args.csv)
            return

        # Exp2 workload mode (batch)
        if args.workload:
            prompts = load_chat_workload(args.workload)
            if not prompts:
                raise RuntimeError(f"No prompts found in workload: {args.workload}")
            run_workload_e2e(fd, req_layout, resp_layout, prompts, args.repeat, args.e2e_csv)
            return

        # Exp2 interactive mode (single prompt)
        user_text = input("Ask something: ")
        prompt = prompt_tmpl.format(text=user_text)
        payload = prompt.encode("utf-8")

        t0 = time.perf_counter_ns()
        CSM_send(fd, req_layout, payload)
        resp = CSM_receive(fd, resp_layout)
        t1 = time.perf_counter_ns()

        print(f"[Exp2 chatbot] time={(t1 - t0)/1e9:.3f}s")
        print(resp.decode("utf-8", errors="replace"))

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
