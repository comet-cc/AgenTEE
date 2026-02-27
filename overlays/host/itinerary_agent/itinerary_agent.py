import os
import argparse
import time
import csv
import json
from datetime import datetime

from CSM import CSM_receive, CSM_send, channel_layout
try:
    from langchain.prompts import PromptTemplate  # older langchain
except Exception:
    from langchain_core.prompts import PromptTemplate  # langchain 1.x

DEFAULT_REGION_SIZE = 4096
DEFAULT_PAYLOAD_OFF = 24

itinerary_tmpl = PromptTemplate(
    input_variables=[
        "destination",
        "days",
        "start_date",
        "budget",
        "travel_style",
        "interests",
        "constraints",
    ],
    template=(
        "You are a travel itinerary planning assistant.\n"
        "Create a {days}-day itinerary for a trip to {destination} starting on {start_date}.\n"
        "Budget: {budget}\n"
        "Travel style: {travel_style}\n"
        "Interests: {interests}\n"
        "Constraints: {constraints}\n\n"
        "Output format:\n"
        "Summary (2-3 sentences)\n"
        "Day 1..Day {days} with Morning/Afternoon/Evening\n"
        "Cost Notes at the end\n\n"
        "Itinerary:\n"
    ),
)


def parse_args():
    p = argparse.ArgumentParser(description="Itinerary agent (CSM mailbox)")
    p.add_argument("--device", type=str, default="/tmp/shmfile")
    p.add_argument("--channel", type=int, default=0)
    p.add_argument("--req-channel", type=int, default=None, help="Request mailbox channel (default: --channel)")
    p.add_argument("--resp-channel", type=int, default=None, help="Response mailbox channel (default: --channel + 1)")
    p.add_argument("--region-size", type=int, default=DEFAULT_REGION_SIZE)
    p.add_argument("--payload-off", type=int, default=DEFAULT_PAYLOAD_OFF)
    p.add_argument("--base0", type=int, default=0)

    # Exp1 bench args
    p.add_argument("--bench", action="store_true", help="Run IPC benchmark (Exp1)")
    p.add_argument("--iters", type=int, default=1000)
    p.add_argument("--warmup", type=int, default=50)
    p.add_argument("--sizes", type=str, default="64,256,1024,4096,8192,16384")
    p.add_argument("--csv", type=str, default="")

    # Exp2 single-task args
    p.add_argument("--destination", type=str, default="")
    p.add_argument("--days", type=int, default=3)
    p.add_argument("--start-date", type=str, default="")
    p.add_argument("--budget", type=str, default="mid-range")
    p.add_argument("--travel-style", type=str, default="balanced")
    p.add_argument("--interests", type=str, default="food, culture")
    p.add_argument("--constraints", type=str, default="none")
    p.add_argument("--non-interactive", action="store_true")

    # Exp2 workload args
    p.add_argument("--workload", type=str, default="", help="Path to JSON workload file")
    p.add_argument("--repeat", type=int, default=1, help="Repeat workload N times")
    p.add_argument("--e2e-csv", type=str, default="", help="Append Exp2 per-task timings to CSV")

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


def run_bench(fd, req_layout, resp_layout, sizes, iters, warmup, csv_path):
    out_rows = []
    for sz in sizes:
        payload = b"b" * sz
        rtts = []
        for _ in range(iters):
            t0 = time.perf_counter_ns()
            CSM_send(fd, req_layout, payload)
            resp = CSM_receive(fd, resp_layout)
            t1 = time.perf_counter_ns()
            rtts.append(t1 - t0)
            if len(resp) != len(payload):
                raise RuntimeError(f"Echo length mismatch: sent={len(payload)} recv={len(resp)}")

        rtts = rtts[warmup:] if warmup < len(rtts) else rtts
        avg = (sum(rtts) / len(rtts)) if rtts else 0
        print(f"[Bench itinerary] size={sz}B avg={avg/1000:.1f}us n={len(rtts)}")

        out_rows.append(
            {
                "agent": "itinerary",
                "size_bytes": sz,
                "iters": iters,
                "warmup": warmup,
                "avg_ns": int(avg),
                "avg_us": avg / 1000,
                "samples": len(rtts),
            }
        )

    if csv_path:
        write_header = not os.path.exists(csv_path)
        with open(csv_path, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
            if write_header:
                w.writeheader()
            w.writerows(out_rows)


def _prompt_if_empty(label: str, current: str) -> str:
    if current.strip():
        return current.strip()
    return input(label).strip()


def load_itinerary_workload(path: str):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list):
        raise ValueError("Itinerary workload must be a JSON list of objects")
    return data


def append_e2e_rows(csv_path: str, rows: list):
    if not csv_path or not rows:
        return
    write_header = not os.path.exists(csv_path)
    with open(csv_path, "a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        if write_header:
            w.writeheader()
        w.writerows(rows)


def run_workload_e2e(fd, req_layout, resp_layout, jobs, repeat, e2e_csv):
    timings = []
    rows = []
    task_id = 0

    for r in range(repeat):
        for job in jobs:
            task_id += 1
            destination = str(job.get("destination", "Paris"))
            days = int(job.get("days", 3))
            start_date = str(job.get("start_date", "")) or datetime.now().strftime("%Y-%m-%d")
            budget = str(job.get("budget", "mid-range"))
            travel_style = str(job.get("travel_style", "balanced"))
            interests = str(job.get("interests", "food, culture"))
            constraints = str(job.get("constraints", "none"))

            prompt = itinerary_tmpl.format(
                destination=destination,
                days=days,
                start_date=start_date,
                budget=budget,
                travel_style=travel_style,
                interests=interests,
                constraints=constraints,
            )
            payload = prompt.encode("utf-8")

            t0 = time.perf_counter_ns()
            CSM_send(fd, req_layout, payload)
            resp = CSM_receive(fd, resp_layout)
            t1 = time.perf_counter_ns()

            elapsed_ns = t1 - t0
            timings.append(elapsed_ns)

            print(f"[Exp2 itinerary] task={task_id} dest={destination} days={days} bytes={len(payload)} time={elapsed_ns/1e9:.3f}s")
            _ = resp  # keep output short in batch mode

            rows.append(
                {
                    "agent": "itinerary",
                    "task_id": task_id,
                    "destination": destination,
                    "days": days,
                    "time_ns": int(elapsed_ns),
                    "time_s": elapsed_ns / 1e9,
                }
            )

    avg_s = (sum(timings) / len(timings)) / 1e9 if timings else 0.0
    print(f"[Exp2 itinerary] done tasks={len(timings)} avg_time={avg_s:.3f}s")
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
        f"[ItineraryAgent] Ready (device={args.device}, req_ch={req_channel}, resp_ch={resp_channel}, "
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
            jobs = load_itinerary_workload(args.workload)
            if not jobs:
                raise RuntimeError(f"No jobs found in workload: {args.workload}")
            run_workload_e2e(fd, req_layout, resp_layout, jobs, args.repeat, args.e2e_csv)
            return

        # Exp2 single task (interactive or non-interactive)
        if args.non_interactive:
            destination = args.destination.strip() or "Paris"
            days = int(args.days)
            start_date = args.start_date.strip() or datetime.now().strftime("%Y-%m-%d")
            budget = args.budget.strip()
            travel_style = args.travel_style.strip()
            interests = args.interests.strip()
            constraints = args.constraints.strip()
        else:
            destination = _prompt_if_empty("Destination: ", args.destination)
            days = int(_prompt_if_empty("Days (e.g., 3): ", str(args.days)))
            start_date = _prompt_if_empty("Start date YYYY-MM-DD (optional): ", args.start_date) or datetime.now().strftime("%Y-%m-%d")
            budget = _prompt_if_empty("Budget (low/mid-range/luxury): ", args.budget)
            travel_style = _prompt_if_empty("Style (relaxed/balanced/packed): ", args.travel_style)
            interests = _prompt_if_empty("Interests: ", args.interests)
            constraints = _prompt_if_empty("Constraints: ", args.constraints)

        prompt = itinerary_tmpl.format(
            destination=destination,
            days=days,
            start_date=start_date,
            budget=budget,
            travel_style=travel_style,
            interests=interests,
            constraints=constraints,
        )
        payload = prompt.encode("utf-8")

        t0 = time.perf_counter_ns()
        CSM_send(fd, req_layout, payload)
        resp = CSM_receive(fd, resp_layout)
        t1 = time.perf_counter_ns()

        print(f"[Exp2 itinerary] time={(t1 - t0)/1e9:.3f}s")
        print(resp.decode("utf-8", errors="replace"))

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
