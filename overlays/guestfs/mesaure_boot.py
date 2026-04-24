#!/usr/bin/env python3
import argparse, shlex, subprocess, time, selectors, signal, sys, os, re, textwrap

def start_vm(cmd):
    # Run VM with stdout captured (stderr merged so we don't miss console output)
    proc = subprocess.Popen(
        cmd, shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True, bufsize=1  # line-buffered
    )
    return proc

def start_perf(pid, events):
    # Attach perf to the VM process; it will run until we send SIGINT.
    # CSV output (-x ,) goes to stderr; stdout is unused.
    perf_cmd = ["perf", "stat", "-x", ",", "-e", events, "-p", str(pid), "--", "sleep", "1000000000"]
    perf = subprocess.Popen(
        perf_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )
    return perf

def stop_perf_and_get_counts(perf_proc):
    # Politely ask perf to summarize now
    try:
        perf_proc.send_signal(signal.SIGINT)
    except ProcessLookupError:
        pass
    try:
        _, err = perf_proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        perf_proc.kill()
        _, err = perf_proc.communicate()
    return err or ""

def parse_perf_csv(perf_stderr, wanted_event="cycles"):
    """
    Parse perf stat CSV (-x ,) lines. We look for the row whose 'event' matches wanted_event.
    Typical CSV columns: value,unit,event,?,?  (varies with perf version)
    We'll match by ',<event>,' pattern and return the numeric value as int if possible.
    """
    val = None
    for line in perf_stderr.splitlines():
        if f",{wanted_event}," in line:
            cols = [c.strip() for c in line.split(",")]
            if cols:
                raw = cols[0]
                # perf may print <not supported> or <not counted>
                if not raw or raw.startswith("<"):
                    return None
                # remove thousand separators if present
                raw = raw.replace(" ", "").replace(",", "") if raw.count(",")>1 else raw.replace(" ", "")
                # Some perf builds already used comma as separator; we accounted above.
                try:
                    val = int(raw)
                except ValueError:
                    # Try float fallback
                    try:
                        val = int(float(raw))
                    except ValueError:
                        val = None
                return val
    return None

def main():
    ap = argparse.ArgumentParser(
        description="Launch two kvmtool VMs, wait for a token on their consoles, measure latency and CPU cycles.")
    ap.add_argument("--vm1-cmd", required=True, help="Full kvmtool command for VM1 (e.g. 'lkvm run ...').")
    ap.add_argument("--vm2-cmd", required=True, help="Full kvmtool command for VM2.")
    ap.add_argument("--token", default="VM_DONE", help="Line the guest prints when finished (default: VM_DONE).")
    ap.add_argument("--events", default="cycles", help="perf events list (default: cycles).")
    ap.add_argument("--timeout", type=float, default=120.0, help="Overall timeout in seconds (default: 120).")
    ap.add_argument("--grace", type=float, default=3.0, help="Seconds to wait after TERM before KILL (default: 3).")
    args = ap.parse_args()

    # Preflight checks
    for tool in ("perf",):
        if not shutil.which(tool) if 'shutil' in globals() else None:
            import shutil as _shutil
            if not _shutil.which(tool):
                print(f"ERROR: '{tool}' not found in PATH.", file=sys.stderr)
                sys.exit(2)

    # Start both VMs
    vm_specs = [
        {"name": "vm1", "cmd": args.vm1_cmd},
        {"name": "vm2", "cmd": args.vm2_cmd},
    ]

    sel = selectors.DefaultSelector()
    for spec in vm_specs:
        spec["proc"] = start_vm(spec["cmd"])
        spec["start_ns"] = time.monotonic_ns()
        spec["perf"] = start_perf(spec["proc"].pid, args.events)
        spec["buf"] = ""
        spec["done"] = False
        spec["latency_ns"] = None
        spec["cycles"] = None
        if spec["proc"].stdout:
            sel.register(spec["proc"].stdout, selectors.EVENT_READ, spec)

    deadline = time.monotonic() + args.timeout
    token_pattern = re.compile(rf"\b{re.escape(args.token)}\b")

    # Event loop: read from both consoles until token observed or timeout
    while time.monotonic() < deadline and not all(s["done"] for s in vm_specs):
        timeout = max(0, deadline - time.monotonic())
        events = sel.select(timeout=timeout)
        if not events:
            break
        for key, _ in events:
            spec = key.data
            line = key.fileobj.readline()
            if line == "":
                # VM console closed; unregister
                try:
                    sel.unregister(key.fileobj)
                except Exception:
                    pass
                continue
            spec["buf"] += line
            if token_pattern.search(line) and not spec["done"]:
                spec["latency_ns"] = time.monotonic_ns() - spec["start_ns"]
                # Stop perf and get cycles
                perf_out = stop_perf_and_get_counts(spec["perf"])
                spec["cycles"] = parse_perf_csv(perf_out, "cycles")
                spec["done"] = True

    # Cleanup: stop any remaining perf and VMs
    for spec in vm_specs:
        if spec.get("perf") and spec["perf"].poll() is None:
            try:
                spec["perf"].send_signal(signal.SIGINT)
                spec["perf"].wait(timeout=1)
            except Exception:
                spec["perf"].kill()
        if spec.get("proc") and spec["proc"].poll() is None:
            spec["proc"].terminate()
            try:
                spec["proc"].wait(timeout=args.grace)
            except subprocess.TimeoutExpired:
                spec["proc"].kill()

    # Report
    print("\n=== Results ===")
    header = f"{'VM':<6} {'Latency (ms)':>14} {'CPU cycles':>16} {'PID':>8}"
    print(header)
    print("-" * len(header))
    for spec in vm_specs:
        lat_ms = f"{(spec['latency_ns']/1e6):.3f}" if spec["latency_ns"] is not None else "TIMEOUT"
        cycles = f"{spec['cycles']:,}" if spec["cycles"] is not None else "N/A"
        pid = spec["proc"].pid if spec.get("proc") else "-"
        print(f"{spec['name']:<6} {lat_ms:>14} {cycles:>16} {pid:>8}")

    # If either timed out, exit nonzero
    if not all(s["done"] for s in vm_specs):
        sys.exit(1)

if __name__ == "__main__":
    # Lazy import to avoid global dependency before main()
    import shutil
    main()
