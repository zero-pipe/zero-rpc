#!/usr/bin/env python3
"""Blocking-workload matrix for the poller + handler-executor model.

Starts bench_server (fast method + blocking method) and drives it with one or
more loadgen processes. The blocking ratio is controlled by the fast/block
request rates. Results are written as JSON and printed as a table.

Example:
  python3 tools/perf/block_matrix.py --build build --duration 5
"""

from __future__ import annotations

import argparse
import collections
import json
import signal
import subprocess
import sys
import time
from pathlib import Path


def percentile(hist, p):
    total = sum(hist.values())
    if total == 0:
        return 0.0
    target = max(1, int(total * p + 0.999999))
    acc = 0
    for upper in sorted(hist):
        acc += hist[upper]
        if acc >= target:
            return float(upper)
    return float(max(hist))


def load_json(path):
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return {}


def aggregate(paths):
    attempted = succeeded = failed = 0
    duration = 0.0
    hist = collections.Counter()
    errors = collections.Counter()
    for path in paths:
        data = load_json(path)
        attempted += int(data.get("attempted", 0))
        succeeded += int(data.get("succeeded", 0))
        failed += int(data.get("failed", 0))
        duration = max(duration, float(data.get("duration_seconds", 0.0)))
        for key, value in (data.get("latency_histogram_us") or {}).items():
            hist[int(key)] += int(value)
        for key, value in (data.get("errors") or {}).items():
            errors[str(key)] += int(value)
    return {
        "attempted": attempted,
        "succeeded": succeeded,
        "failed": failed,
        "duration_seconds": duration,
        "qps": succeeded / duration if duration > 0 else 0.0,
        "p50_us": percentile(hist, 0.50),
        "p95_us": percentile(hist, 0.95),
        "p99_us": percentile(hist, 0.99),
        "max_us": percentile(hist, 1.0),
        "errors": dict(errors),
    }


def start_server(args, scenario, port, log_path):
    cmd = [
        str(args.server),
        "--mode", "static",
        "--transport", scenario["transport"],
        "--bind", "127.0.0.1",
        "--data-port", str(port),
        "--io-threads", str(scenario["io_threads"]),
        "--handler-threads", str(scenario["handler_threads"]),
        "--handler-max-concurrency", str(scenario["max_conc"]),
        "--service", "OrderService",
        "--method", "get",
        "--block-method", "slow",
        "--block-delay-ms", str(scenario["block_ms"]),
    ]
    if scenario["mode"] == "worker":
        cmd.append("--block-worker")
    elif scenario["mode"] == "executor":
        cmd += ["--block-executor", str(scenario.get("executor_threads", 4))]
    if scenario.get("echo_request"):
        cmd.append("--echo-request")
    log = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    return proc, log


def start_loadgen(args, scenario, port, method, rate, out_path, log_path):
    cmd = [
        str(args.loadgen),
        "--mode", "static",
        "--transport", scenario["transport"],
        "--target", f"127.0.0.1:{port}",
        "--service", "OrderService",
        "--method", method,
        "--duration-seconds", str(scenario["duration"]),
        "--timeout-ms", "5000",
        "--output", str(out_path),
    ]
    if scenario["window"]:
        cmd += ["--window", str(scenario["window"])]
    else:
        cmd += ["--rate", f"{rate:.3f}"]
    if scenario.get("payload_bytes"):
        cmd += ["--payload-bytes", str(scenario["payload_bytes"])]
    log = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    return proc, log


def run_scenario(args, scenario, port, out_dir):
    name = scenario["name"]
    server, server_log = start_server(args, scenario, port, out_dir / f"{name}.server.log")
    loadgens = []
    fast_paths = []
    block_paths = []
    returncodes = []
    try:
        # Service registration is posted to the server poller; the loadgen
        # warmup retries ERR_NOTFOUND, so a short settle is enough.
        time.sleep(0.4)
        if server.poll() is not None:
            raise RuntimeError(f"{name}: server exited early (see {name}.server.log)")

        for index in range(scenario["fast_clients"]):
            out = out_dir / f"{name}.fast{index}.json"
            log = out_dir / f"{name}.fast{index}.log"
            rate = scenario["fast_rate"] / scenario["fast_clients"]
            proc, fh = start_loadgen(args, scenario, port, "get", rate, out, log)
            loadgens.append((proc, fh))
            fast_paths.append(out)
        for index in range(scenario["block_clients"]):
            out = out_dir / f"{name}.block{index}.json"
            log = out_dir / f"{name}.block{index}.log"
            rate = scenario["block_rate"] / scenario["block_clients"]
            proc, fh = start_loadgen(args, scenario, port, "slow", rate, out, log)
            loadgens.append((proc, fh))
            block_paths.append(out)

        for proc, _ in loadgens:
            returncodes.append(proc.wait(timeout=scenario["duration"] + 30))
    finally:
        for proc, fh in loadgens:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            fh.close()
        if server.poll() is None:
            server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        server_log.close()

    if any(code != 0 for code in returncodes):
        raise RuntimeError(f"{name}: loadgen failed: {returncodes}")

    return {
        "scenario": name,
        "group": scenario["group"],
        "config": {key: scenario[key] for key in (
            "transport", "io_threads", "handler_threads", "max_conc", "mode",
            "block_ms", "ratio", "fast_clients", "block_clients", "fast_rate",
            "block_rate", "window", "duration", "executor_threads", "payload_bytes")
            if key in scenario},
        "fast": aggregate(fast_paths),
        "block": aggregate(block_paths),
    }


def build_scenarios(duration):
    scenarios = []
    total_qps = 2000.0
    # Keep the blocking request rate below 80% of one connection's capacity so
    # the loadgen itself is not the bottleneck.
    max_block_rate = 1000.0 / 2.0 * 0.8

    for mode, handler_threads in (("worker", (1, 2, 4)), ("inline", (0,))):
        for threads in handler_threads:
            for ratio in (0, 5, 20, 50, 100):
                if ratio == 0:
                    fast_rate, block_rate = total_qps, 0.0
                else:
                    block_rate = min(total_qps * ratio / 100.0, max_block_rate)
                    fast_rate = block_rate * (100 - ratio) / ratio
                scenarios.append({
                    "name": f"mix-{mode}-h{threads}-r{ratio}",
                    "group": "mix",
                    "transport": "tcp",
                    "io_threads": 1,
                    "handler_threads": threads,
                    "max_conc": 0,
                    "mode": mode,
                    "block_ms": 2,
                    "ratio": ratio,
                    "fast_clients": 1 if ratio < 100 else 0,
                    "block_clients": 1 if ratio else 0,
                    "fast_rate": fast_rate,
                    "block_rate": block_rate,
                    "window": 0,
                    "duration": duration,
                })

    ratio = 20
    block_rate = min(total_qps * ratio / 100.0, max_block_rate)
    fast_rate = block_rate * (100 - ratio) / ratio
    for mode, threads in (("worker", 4), ("inline", 0)):
        for io_threads in (1, 2, 4, 8):
            scenarios.append({
                "name": f"poller-{mode}-io{io_threads}",
                "group": "poller",
                "transport": "tcp",
                "io_threads": io_threads,
                "handler_threads": threads,
                "max_conc": 0,
                "mode": mode,
                "block_ms": 2,
                "ratio": ratio,
                "fast_clients": 4,
                "block_clients": 4,
                "fast_rate": fast_rate,
                "block_rate": block_rate,
                "window": 0,
                "duration": duration,
            })

    for threads in (1, 2, 4, 8):
        scenarios.append({
            "name": f"pure-h{threads}",
            "group": "pure",
            "transport": "tcp",
            "io_threads": 1,
            "handler_threads": threads,
            "max_conc": 0,
            "mode": "worker",
            "block_ms": 2,
            "ratio": 100,
            "fast_clients": 0,
            "block_clients": 1,
            "fast_rate": 0.0,
            "block_rate": 0.0,
            # Just enough concurrency to saturate the pool without building a
            # long queue, so latency reflects service time rather than queueing.
            "window": max(2, threads * 2),
            "duration": duration,
        })

    for threads in (1, 4):
        scenarios.append({
            "name": f"queue-h{threads}",
            "group": "queue",
            "transport": "tcp",
            "io_threads": 1,
            "handler_threads": threads,
            "max_conc": 0,
            "mode": "worker",
            "block_ms": 2,
            "ratio": 100,
            "fast_clients": 0,
            "block_clients": 1,
            "fast_rate": 0.0,
            "block_rate": 0.0,
            "window": 64,
            "duration": duration,
        })

    for max_conc in (0, 8):
        scenarios.append({
            "name": f"overload-mc{max_conc}",
            "group": "overload",
            "transport": "tcp",
            "io_threads": 1,
            "handler_threads": 2,
            "max_conc": max_conc,
            "mode": "worker",
            "block_ms": 5,
            "ratio": 100,
            "fast_clients": 0,
            "block_clients": 1,
            "fast_rate": 0.0,
            "block_rate": 0.0,
            "window": 16,
            "duration": duration,
        })

    # 10% requests block 1ms; blocking method uses a custom executor or inline.
    # 1 KiB payloads, fast method inline. One poller to isolate head-of-line
    # blocking, two fast clients + one blocking client, 3000 qps total.
    for mode, executor_threads in (("executor", 4), ("inline", 0)):
        scenarios.append({
            "name": f"block10-{mode}",
            "group": "executor",
            "transport": "tcp",
            "io_threads": 1,
            "handler_threads": 0,
            "max_conc": 0,
            "mode": mode,
            "executor_threads": executor_threads,
            "block_ms": 1,
            "ratio": 10,
            "fast_clients": 2,
            "block_clients": 1,
            "fast_rate": 2700.0,
            "block_rate": 300.0,
            "window": 0,
            "duration": duration,
            "payload_bytes": 1024,
            "echo_request": True,
        })
    scenarios.append({
        "name": "block10-baseline",
        "group": "executor",
        "transport": "tcp",
        "io_threads": 1,
        "handler_threads": 0,
        "max_conc": 0,
        "mode": "inline",
        "executor_threads": 0,
        "block_ms": 1,
        "ratio": 0,
        "fast_clients": 2,
        "block_clients": 0,
        "fast_rate": 3000.0,
        "block_rate": 0.0,
        "window": 0,
        "duration": duration,
        "payload_bytes": 1024,
        "echo_request": True,
    })
    return scenarios


def print_table(results):
    header = (
        f"{'group':9} {'scenario':22} {'fast p50/p99 us':>18} "
        f"{'block p50/p99 us':>18} {'fast qps':>9} {'block qps':>9} {'blk fail':>9}"
    )
    print(header)
    print("-" * len(header))
    for result in results:
        fast = result["fast"]
        block = result["block"]
        print(
            f"{result['group']:9} {result['scenario']:22} "
            f"{fast['p50_us']:8.0f}/{fast['p99_us']:<8.0f} "
            f"{block['p50_us']:8.0f}/{block['p99_us']:<8.0f} "
            f"{fast['qps']:9.0f} {block['qps']:9.0f} {block['failed']:9d}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build", help="build directory")
    parser.add_argument("--server", help="path to bench_server")
    parser.add_argument("--loadgen", help="path to loadgen")
    parser.add_argument("--out", help="output directory")
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--base-port", type=int, default=47000)
    parser.add_argument("--group", action="append", help="run only these groups")
    args = parser.parse_args()

    build = Path(args.build)
    args.server = Path(args.server) if args.server else build / "tools" / "bench_server"
    args.loadgen = Path(args.loadgen) if args.loadgen else build / "tools" / "loadgen"
    if not args.server.exists() or not args.loadgen.exists():
        print(f"missing binaries: {args.server} / {args.loadgen}", file=sys.stderr)
        return 2

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = Path(args.out) if args.out else Path("artifacts") / f"block-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)

    scenarios = build_scenarios(args.duration)
    if args.group:
        wanted = set(args.group)
        scenarios = [s for s in scenarios if s["group"] in wanted]

    results = []
    failures = []
    for index, scenario in enumerate(scenarios):
        port = args.base_port + index
        print(f"[{index + 1}/{len(scenarios)}] {scenario['name']} ...", flush=True)
        try:
            results.append(run_scenario(args, scenario, port, out_dir))
        except Exception as exc:  # noqa: BLE001 - report and continue the matrix
            failures.append({"scenario": scenario["name"], "error": str(exc)})
            print(f"  failed: {exc}", file=sys.stderr)

    (out_dir / "results.json").write_text(
        json.dumps({"results": results, "failures": failures}, indent=2),
        encoding="utf-8",
    )
    print()
    print_table(results)
    print(f"\nresults: {out_dir / 'results.json'}")
    if failures:
        print(f"failures: {len(failures)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
