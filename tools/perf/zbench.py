#!/usr/bin/env python3
"""zbench - unified perf / stress / stability harness for zrpc.

One entry point that drives the C load engines (zrpc_bench_server +
zrpc_loadgen) and the CTest unit/integration suites, and produces
machine-readable + HTML reports so any change can be gated.

Commands
  unit      run CTest unit + integration suites (hard gate)
  perf      normal scenario matrix (payload x transport x concurrency x rate)
  stress    abnormal scenarios (loss, backpressure, oversize, notfound,
            connect-fail, mid-transfer abort, malformed datagrams)
  soak      long-running stability run with resource sampling + leak heuristics
  valgrind  run unit + integration under Valgrind Memcheck (Linux)
  sanitizer build an ASan/UBSan tree and run unit + integration + perf smoke
  analyze   read a result dir, render report.html + junit.xml, gate vs baseline
  all       unit + perf + stress (+ soak when --soak is given)

Examples
  python3 tools/perf/zbench.py unit
  python3 tools/perf/zbench.py perf --quick
  python3 tools/perf/zbench.py perf --transport udp --duration 10
  python3 tools/perf/zbench.py soak --duration 300 --clients 4
  python3 tools/perf/zbench.py analyze --run artifacts/20260930T120000Z

Exit code is 0 only when every selected scenario passes its expectation and,
for `analyze`, no regression against the stored baseline is detected.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import html
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_OUT_DIR = ROOT / "artifacts"
MAX_MSG = 64 * 1024 * 1024
PORT_BASE = 44000
PORT_BLOCK = 80

IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""


def log(msg: str) -> None:
    print(msg, flush=True)


def timestamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def human_size(n: int) -> str:
    for unit, div in (("m", 1024 * 1024), ("k", 1024)):
        if n >= div and n % div == 0:
            return "{}{}".format(n // div, unit)
    return "{}b".format(n)


# ---------------------------------------------------------------------------
# process helpers
# ---------------------------------------------------------------------------

def popen(cmd: List[str], log_path: Path, env: Optional[Dict[str, str]] = None) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(str(log_path), "wb")
    kwargs: Dict[str, Any] = {}
    if IS_WINDOWS:
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT, env=env, **kwargs)
    proc._zbench_log = handle  # type: ignore[attr-defined]
    return proc


def terminate(proc: subprocess.Popen, timeout: float = 10.0) -> None:
    if proc.poll() is not None:
        return
    try:
        if IS_WINDOWS:
            proc.terminate()
        else:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        try:
            proc.terminate()
        except Exception:
            pass
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            if IS_WINDOWS:
                proc.kill()
            else:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        try:
            proc.wait(timeout=5.0)
        except Exception:
            pass


def close_log(proc: subprocess.Popen) -> None:
    handle = getattr(proc, "_zbench_log", None)
    if handle:
        try:
            handle.close()
        except Exception:
            pass


def wait_ready(proc: subprocess.Popen, log_path: Path, timeout: int) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if log_path.exists() and "ready:" in log_path.read_text(errors="replace"):
                return True
        except OSError:
            pass
        time.sleep(0.05)
    return False


# ---------------------------------------------------------------------------
# resource sampling (Linux /proc; no-op elsewhere)
# ---------------------------------------------------------------------------

def sample_process(pid: int) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if IS_WINDOWS:
        return out
    try:
        status = Path("/proc/{}/status".format(pid)).read_text()
        for line in status.splitlines():
            if line.startswith("VmRSS:"):
                out["rss_kb"] = int(line.split()[1])
            elif line.startswith("Threads:"):
                out["threads"] = int(line.split()[1])
        out["fds"] = len(os.listdir("/proc/{}/fd".format(pid)))
    except OSError:
        pass
    return out


def resource_leak_suspected(resource: Dict[str, Any]) -> bool:
    """Apply conservative soak thresholds to Linux process samples."""
    if not resource:
        return False
    rss_growth = float(resource.get("rss_growth", 0.0))
    fd_start = int(resource.get("fd_start", 0))
    fd_end = int(resource.get("fd_end", 0))
    return rss_growth > 0.25 or (fd_end - fd_start) > 4


class ResourceSampler(threading.Thread):
    def __init__(self, pid: int, interval: float = 0.5):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self.samples: List[Dict[str, Any]] = []
        self._halt = threading.Event()

    def run(self) -> None:
        while not self._halt.is_set():
            s = sample_process(self.pid)
            if s:
                s["t"] = time.monotonic()
                self.samples.append(s)
            self._halt.wait(self.interval)

    def stop(self) -> Dict[str, Any]:
        self._halt.set()
        self.join(timeout=2.0)
        if not self.samples:
            return {}
        first = self.samples[0]
        last = self.samples[-1]
        rss0 = first.get("rss_kb", 0)
        rss1 = last.get("rss_kb", 0)
        growth = (rss1 - rss0) / rss0 if rss0 else 0.0
        return {
            "rss_start_kb": rss0,
            "rss_end_kb": rss1,
            "rss_growth": growth,
            "fd_start": first.get("fds", 0),
            "fd_end": last.get("fds", 0),
            "threads_end": last.get("threads", 0),
            "samples": len(self.samples),
        }


# ---------------------------------------------------------------------------
# result aggregation
# ---------------------------------------------------------------------------

def _percentile(hist: Dict[int, int], p: float) -> float:
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


def load_worker(path: Path) -> Dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return {"status": "missing", "attempted": 0, "succeeded": 0, "failed": 0,
                "duration_seconds": 0.0, "errors": {}, "latency_histogram_us": {}}


def aggregate(workers: List[Dict[str, Any]]) -> Dict[str, Any]:
    attempted = succeeded = failed = response_bytes = 0
    duration = 0.0
    histogram: collections.Counter = collections.Counter()
    errors: collections.Counter = collections.Counter()
    metrics: collections.Counter = collections.Counter()
    statuses: collections.Counter = collections.Counter()
    lat_min: Optional[float] = None
    lat_max: Optional[float] = None
    lat_weighted = 0.0
    lat_n = 0
    for w in workers:
        attempted += int(w.get("attempted", 0))
        succeeded += int(w.get("succeeded", 0))
        failed += int(w.get("failed", 0))
        response_bytes += int(w.get("response_bytes", 0))
        duration = max(duration, float(w.get("duration_seconds", 0.0)))
        statuses[str(w.get("status"))] += 1
        errors.update({str(k): int(v) for k, v in (w.get("errors") or {}).items()})
        metrics.update({str(k): int(v) for k, v in (w.get("metrics") or {}).items()})
        histogram.update({int(k): int(v) for k, v in (w.get("latency_histogram_us") or {}).items()})
        lat = w.get("latency_us") or {}
        ok = int(w.get("succeeded", 0))
        if ok > 0 and lat:
            cur_min = float(lat.get("min", 0.0))
            cur_max = float(lat.get("max", 0.0))
            lat_min = cur_min if lat_min is None else min(lat_min, cur_min)
            lat_max = cur_max if lat_max is None else max(lat_max, cur_max)
            lat_weighted += float(lat.get("mean", 0.0)) * ok
            lat_n += ok
    return {
        "attempted": attempted,
        "succeeded": succeeded,
        "failed": failed,
        "error_rate": failed / attempted if attempted else 0.0,
        "response_bytes": response_bytes,
        "duration_seconds": duration,
        "qps": succeeded / duration if duration > 0 else 0.0,
        "errors": dict(sorted(errors.items())),
        "metrics": dict(sorted(metrics.items())),
        "worker_status": dict(statuses),
        "latency_us": {
            "min": lat_min or 0.0,
            "mean": lat_weighted / lat_n if lat_n else 0.0,
            "p50": _percentile(dict(histogram), 0.50),
            "p95": _percentile(dict(histogram), 0.95),
            "p99": _percentile(dict(histogram), 0.99),
            "max": lat_max or 0.0,
        },
    }


# ---------------------------------------------------------------------------
# scenario model
# ---------------------------------------------------------------------------

def scn(name: str, suite: str, transport: str, **kw: Any) -> Dict[str, Any]:
    base: Dict[str, Any] = {
        "name": name,
        "suite": suite,
        "transport": transport,
        "service": "OrderService",
        "method": "get",
        "client_service": None,
        "payload": 64,
        "clients": 1,
        "duration": 3,
        "rate": 0.0,
        "max_requests": 0,
        "timeout_ms": 5000,
        "connect_timeout": 10,
        "echo": True,
        "response_bytes": 0,
        "frag_bytes": 0,
        "drop_percent": 0,
        "backpressure": 0,
        "delay_ms": 0,
        "server_max_msg": MAX_MSG,
        "client_max_msg": MAX_MSG,
        "verify": True,
        "kill_server_after": 0.0,
        "garbage": None,
        "expect": {"kind": "success", "max_error_rate": 0.0, "min_success": 1},
    }
    base.update(kw)
    return base


def perf_scenarios(args: argparse.Namespace) -> List[Dict[str, Any]]:
    transports = ["udp", "tcp"] if args.transport == "both" else [args.transport]
    d = args.duration or (2 if args.quick else 3)
    out: List[Dict[str, Any]] = []
    sizes = [64, 8192, 32768, 1048576, 33554432] if not args.quick else [64, 1048576]
    for t in transports:
        for s in sizes:
            mr = 1 if s >= 33554432 else (10 if s >= 1048576 else 0)
            out.append(scn("{}-{}".format(t, human_size(s)), "perf", t, payload=s,
                           clients=1, duration=d, max_requests=mr))
    for t in transports:
        for n in ([16] if args.quick else [4, 16, 32]):
            out.append(scn("{}-conc{}".format(t, n), "perf", t, payload=64, clients=n, duration=d))
    if not args.quick:
        for t in transports:
            out.append(scn("{}-1k-10kqps".format(t), "perf", t, payload=1024, clients=16,
                           duration=d, rate=10000.0))
    return out


def stress_scenarios(args: argparse.Namespace) -> List[Dict[str, Any]]:
    d = args.duration or (3 if not args.quick else 2)
    out: List[Dict[str, Any]] = []
    # Weak network: UDP packet loss, expect NACK/RTX recovery.
    out.append(scn("loss10-8k", "stress", "udp", payload=8192, clients=1, duration=d,
                   drop_percent=10, max_requests=0, expect={"kind": "recover"}))
    out.append(scn("loss20-1k", "stress", "udp", payload=1024, clients=1, duration=d,
                   drop_percent=20, max_requests=0, expect={"kind": "recover"}))
    # Backpressure: tiny high-water mark, large payload.
    for t in (["udp", "tcp"] if args.transport == "both" else [args.transport]):
        out.append(scn("bp-1m", "stress", t, payload=1048576, clients=1, duration=d,
                       backpressure=65536, max_requests=200, expect={"kind": "success"}))
    # Oversize: client sends above server max_msg -> client must fail, server must survive.
    for t in (["udp", "tcp"] if args.transport == "both" else [args.transport]):
        out.append(scn("oversize", "stress", t, payload=131072, clients=1, duration=d,
                       server_max_msg=65536, client_max_msg=MAX_MSG, max_requests=1,
                       timeout_ms=1500, expect={"kind": "client_error"}))
    # Unknown service -> NOTFOUND, server survives.
    out.append(scn("notfound", "stress", "udp", payload=64, clients=1, duration=d,
                   client_service="NoSuchService", max_requests=1, verify=False,
                   timeout_ms=1500, connect_timeout=3,
                   expect={"kind": "client_error"}))
    # Connect failure: dial a closed port.
    out.append(scn("connect-fail", "stress", "udp", payload=64, clients=1, duration=d,
                   target_port_offset=7, connect_timeout=3, timeout_ms=1500,
                   max_requests=1, verify=False,
                   expect={"kind": "client_error"}, no_server=True))
    # Mid-transfer abort: kill the server while a large TCP transfer is in flight.
    out.append(scn("server-abort", "stress", "tcp", payload=16777216, clients=1, duration=d,
                   max_requests=1, kill_server_after=0.15,
                   expect={"kind": "aborted"}))
    # Malformed frames: blast garbage at the data port, then a normal request must still work.
    for t in (["udp", "tcp"] if args.transport == "both" else [args.transport]):
        out.append(scn("malformed", "stress", t, payload=128, clients=1, duration=d,
                       max_requests=3, garbage=64, expect={"kind": "success"}))
    # Timeout: server stalls longer than the client timeout.
    out.append(scn("timeout", "stress", "udp", payload=64, clients=1, duration=d,
                   delay_ms=1500, timeout_ms=500, max_requests=1, verify=False,
                   expect={"kind": "client_error"}))
    return out


def soak_scenarios(args: argparse.Namespace) -> List[Dict[str, Any]]:
    d = args.duration or 120
    clients = args.clients or 4
    out: List[Dict[str, Any]] = []
    for t in (["udp", "tcp"] if args.transport == "both" else [args.transport]):
        out.append(scn("{}-soak-4k".format(t), "soak", t, payload=4096, clients=clients,
                       duration=d, rate=50.0 * clients, max_requests=0,
                       expect={"kind": "success"}))
    return out


# ---------------------------------------------------------------------------
# scenario execution
# ---------------------------------------------------------------------------

class PortAllocator:
    def __init__(self, base: int = PORT_BASE):
        self.next = base

    def take(self, count: int) -> int:
        port = self.next
        self.next += max(count, 1) + 2
        if self.next > 65000:
            self.next = PORT_BASE
        return port


def server_cmd(server: Path, s: Dict[str, Any], data_port: int, disc_port: int) -> List[str]:
    cmd = [str(server), "--transport", s["transport"], "--bind", "127.0.0.1",
           "--data-port", str(data_port), "--discovery-port", str(disc_port),
           "--service", s["service"], "--method", s["method"],
           "--max-msg-bytes", str(s["server_max_msg"])]
    if s["echo"]:
        cmd.append("--echo-request")
    if s["response_bytes"]:
        cmd += ["--response-bytes", str(s["response_bytes"])]
    if s["frag_bytes"]:
        cmd += ["--frag-bytes", str(s["frag_bytes"])]
    if s["backpressure"]:
        cmd += ["--backpressure-bytes", str(s["backpressure"])]
    if s["delay_ms"]:
        cmd += ["--delay-ms", str(s["delay_ms"])]
    return cmd


def worker_cmd(loadgen: Path, s: Dict[str, Any], index: int, target_port: int,
               disc_port: int, out_path: Path) -> List[str]:
    service = s["client_service"] or s["service"]
    cmd = [str(loadgen), "--transport", s["transport"], "--bind", "127.0.0.1",
           "--data-port", "0", "--discovery-port", str(disc_port),
           "--target", "127.0.0.1:{}".format(target_port),
           "--service", service, "--method", s["method"],
           "--duration-seconds", str(s["duration"]),
           "--payload-bytes", str(s["payload"]),
           "--expect-response-bytes", str(s["payload"]),
           "--max-msg-bytes", str(s["client_max_msg"]),
           "--timeout-ms", str(s["timeout_ms"]),
           "--connect-timeout-seconds", str(s["connect_timeout"]),
           "--worker-id", "w{}".format(index),
           "--output", str(out_path)]
    if s["verify"]:
        cmd.append("--verify-response")
    if s["rate"] > 0:
        cmd += ["--rate", "{:.6f}".format(s["rate"] / max(1, s["clients"]))]
    if s["max_requests"]:
        cmd += ["--max-requests", str(s["max_requests"])]
    if s["frag_bytes"]:
        cmd += ["--frag-bytes", str(s["frag_bytes"])]
    if s["backpressure"]:
        cmd += ["--backpressure-bytes", str(s["backpressure"])]
    return cmd


def send_garbage(transport: str, port: int, count: int) -> None:
    import random
    import socket
    rng = random.Random(0x5EED)
    if transport == "udp":
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(count):
            size = rng.randint(1, 1400)
            sock.sendto(bytes(rng.randrange(256) for _ in range(size)), ("127.0.0.1", port))
        sock.close()
    else:
        import struct
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            return
        for _ in range(count):
            size = rng.randint(1, 4000)
            # sometimes a plausible-looking length prefix followed by junk
            if rng.random() < 0.5:
                body = bytes(rng.randrange(256) for _ in range(size))
                sock.sendall(struct.pack("<I", size) + body)
            else:
                sock.sendall(bytes(rng.randrange(256) for _ in range(size)))
        sock.close()


def run_scenario(server: Path, loadgen: Path, s: Dict[str, Any], ports: PortAllocator,
                 out_dir: Path, env_base: Dict[str, str], sample: bool) -> Dict[str, Any]:
    run_dir = out_dir / s["name"]
    run_dir.mkdir(parents=True, exist_ok=True)
    clients = max(1, int(s["clients"]))
    base = ports.take(clients + 2)
    data_port = base
    disc_port = base + 1
    client_disc = base + 2

    env = dict(env_base)
    if s["drop_percent"]:
        env["ZRPC_DROP_PERCENT"] = str(s["drop_percent"])

    server_log = run_dir / "server.log"
    srv: Optional[subprocess.Popen] = None
    if not s.get("no_server"):
        srv = popen(server_cmd(server, s, data_port, disc_port), server_log, env)
        if not wait_ready(srv, server_log, 20):
            if srv.poll() is None:
                terminate(srv)
            close_log(srv)
            res = aggregate([])
            res.update({"name": s["name"], "suite": s["suite"], "transport": s["transport"],
                        "payload": s["payload"], "clients": clients, "rate": s["rate"],
                        "expect": s["expect"], "status": "server_start_failed", "checks": []})
            return finalize(res, s, server_alive=False)
        if s["garbage"]:
            send_garbage(s["transport"], data_port, int(s["garbage"]))

    target_port = data_port + s.get("target_port_offset", 0)
    sampler: Optional[ResourceSampler] = None
    if sample and srv is not None:
        sampler = ResourceSampler(srv.pid)
        sampler.start()

    procs: List[Tuple[subprocess.Popen, Path]] = []
    for i in range(clients):
        result_path = run_dir / "worker_{}.json".format(i)
        cmd = worker_cmd(loadgen, s, i, target_port, client_disc + i, result_path)
        wlog = run_dir / "worker_{}.log".format(i)
        procs.append((popen(cmd, wlog, env), result_path))

    if srv is not None and s["kill_server_after"]:
        time.sleep(float(s["kill_server_after"]))
        terminate(srv)

    wait_budget = s["duration"] + s["connect_timeout"] + 60
    for proc, _ in procs:
        try:
            proc.wait(timeout=wait_budget)
        except subprocess.TimeoutExpired:
            terminate(proc)
        close_log(proc)

    resource = sampler.stop() if sampler else {}
    server_alive = False
    if srv is not None:
        server_alive = srv.poll() is None
        terminate(srv)
        close_log(srv)

    workers = [load_worker(p) for _, p in procs]
    res = aggregate(workers)
    res.update({
        "name": s["name"], "suite": s["suite"], "transport": s["transport"],
        "payload": s["payload"], "clients": clients, "rate": s["rate"],
        "duration_seconds_requested": s["duration"],
        "expect": s["expect"], "status": "ok" if not res["failed"] else "error",
        "resource": resource,
    })
    if sample:
        res["leak_suspected"] = resource_leak_suspected(resource)
    return finalize(res, s, server_alive=server_alive)


def finalize(res: Dict[str, Any], s: Dict[str, Any], server_alive: bool) -> Dict[str, Any]:
    expect = s["expect"]
    kind = expect.get("kind", "success")
    checks: List[Tuple[str, bool]] = []
    err = res.get("error_rate", 1.0)
    ok = res.get("succeeded", 0)
    observed_error = res.get("failed", 0) > 0 or any(
        status != "ok" for status in res.get("worker_status", {}) if status)
    max_err = float(expect.get("max_error_rate", 0.0))
    min_ok = int(expect.get("min_success", 1))
    has_server = not s.get("no_server")

    if kind == "success":
        checks.append(("succeeded >= {}".format(min_ok), ok >= min_ok))
        checks.append(("error_rate <= {}".format(max_err), err <= max_err))
        if s["suite"] == "soak":
            checks.append(("no suspected resource leak", not res.get("leak_suspected", False)))
    elif kind == "recover":
        checks.append(("succeeded >= {}".format(min_ok), ok >= min_ok))
        checks.append(("no fatal errors", res.get("failed", 0) == 0))
    elif kind == "client_error":
        checks.append(("client observed error", observed_error))
    elif kind == "aborted":
        checks.append(("no hang", True))
    else:
        checks.append(("unknown expectation", False))

    if has_server and kind != "aborted":
        checks.append(("server survived", server_alive))

    res["checks"] = [{"name": n, "ok": okv} for n, okv in checks]
    res["pass"] = all(c["ok"] for c in res["checks"])
    return res


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------

def summarize_console(results: List[Dict[str, Any]]) -> None:
    header = "{:16s} {:>10s} {:>10s} {:>8s} {:>10s} {:>9s} {:>8s}".format(
        "scenario", "attempted", "ok", "fail", "qps", "p99us", "pass")
    log(header)
    log("-" * len(header))
    for r in results:
        lat = r.get("latency_us", {})
        log("{:16s} {:>10d} {:>10d} {:>8d} {:>10.1f} {:>9.0f} {:>8s}".format(
            r["name"], r.get("attempted", 0), r.get("succeeded", 0), r.get("failed", 0),
            r.get("qps", 0.0), float(lat.get("p99", 0.0)), "ok" if r.get("pass") else "FAIL"))


REPORT_CSS = """
:root{--ink:#0f172a;--muted:#475569;--line:#e2e8f0;--ok:#15803d;--bad:#b91c1c;--bg:#f1f5f9}
*{box-sizing:border-box}body{font-family:Inter,ui-sans-serif,system-ui,"Segoe UI",sans-serif;margin:0;background:var(--bg);color:var(--ink);line-height:1.5}
.wrap{max-width:1120px;margin:0 auto;padding:26px 20px 60px}
.hero{background:linear-gradient(135deg,#0f766e,#0f172a);color:#fff;border-radius:14px;padding:22px 26px;margin-bottom:18px}
.hero h1{margin:0 0 4px;font-size:23px}.hero p{margin:0;color:rgba(255,255,255,.82);font-size:14px}
table{border-collapse:collapse;width:100%;background:#fff;border-radius:10px;overflow:hidden;border:1px solid var(--line);margin:8px 0 16px}
th,td{padding:8px 11px;text-align:left;border-bottom:1px solid var(--line);font-size:13px}
th{background:#f8fafc;color:#334155;font-weight:600}
tr.ok td{background:#f0fdf4}tr.bad td{background:#fef2f2}
.pill{display:inline-block;padding:3px 10px;border-radius:999px;font-size:12px;font-weight:700}
.pill.ok{background:#dcfce7;color:#166534}.pill.bad{background:#fee2e2;color:#991b1b}
.muted{color:var(--muted);font-size:12px}
"""


def render_report(results: List[Dict[str, Any]], title: str) -> str:
    passed = all(r.get("pass") for r in results)
    rows = []
    for r in results:
        lat = r.get("latency_us", {})
        cls = "ok" if r.get("pass") else "bad"
        rows.append(
            "<tr class='{c}'><td>{n}</td><td>{s}</td><td>{t}</td><td>{p}</td><td>{cl}</td>"
            "<td>{ar:.2f}</td><td>{ok}</td><td>{f}</td><td>{q:.1f}</td>"
            "<td>{p50:.0f}</td><td>{p99:.0f}</td><td>{e}</td></tr>".format(
                c=cls, n=html.escape(r["name"]), s=html.escape(r.get("suite", "")),
                t=html.escape(r.get("transport", "")), p=r.get("payload", 0),
                cl=r.get("clients", 1), ar=float(r.get("error_rate", 0.0)) * 100,
                ok=r.get("succeeded", 0), f=r.get("failed", 0), q=float(r.get("qps", 0.0)),
                p50=float(lat.get("p50", 0.0)), p99=float(lat.get("p99", 0.0)),
                e=html.escape(str(r.get("errors", {})))))
    return ("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>{t}</title><style>{css}</style></head><body><div class='wrap'>"
            "<div class='hero'><h1>{t}</h1><p>generated {ts} · {n} scenarios · "
            "<span class='pill {cls}'>{verdict}</span></p></div>"
            "<table><tr><th>scenario</th><th>suite</th><th>transport</th><th>payload</th>"
            "<th>clients</th><th>err%</th><th>ok</th><th>fail</th><th>qps</th>"
            "<th>p50 us</th><th>p99 us</th><th>errors</th></tr>{rows}</table>"
            "<p class='muted'>zbench · zrpc perf/stress/stability harness</p>"
            "</div></body></html>").format(
                t=html.escape(title), css=REPORT_CSS, ts=timestamp(), n=len(results),
                cls="ok" if passed else "bad", verdict="ALL PASS" if passed else "FAILURES",
                rows="".join(rows))


def write_junit(path: Path, results: List[Dict[str, Any]]) -> None:
    failures = sum(0 if r.get("pass") else 1 for r in results)
    parts = ["<?xml version='1.0' encoding='utf-8'?>",
             "<testsuite name='zbench' tests='{}' failures='{}'>".format(len(results), failures)]
    for r in results:
        parts.append("<testcase classname='{}' name='{}'>".format(
            html.escape(r.get("suite", "perf")), html.escape(r["name"])))
        if not r.get("pass"):
            reasons = "; ".join(c["name"] for c in r.get("checks", []) if not c["ok"])
            parts.append("<failure message='{}'/>".format(html.escape(reasons or "failed")))
        parts.append("</testcase>")
    parts.append("</testsuite>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def write_reports(out_dir: Path, results: List[Dict[str, Any]], title: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "briefing.json").write_text(
        json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (out_dir / "report.html").write_text(render_report(results, title), encoding="utf-8")
    write_junit(out_dir / "junit.xml", results)


# ---------------------------------------------------------------------------
# baseline / analyze
# ---------------------------------------------------------------------------

def compare_baseline(results: List[Dict[str, Any]], baseline: Dict[str, Any],
                     qps_tol: float, p99_tol: float) -> List[Dict[str, Any]]:
    regressions: List[Dict[str, Any]] = []
    for r in results:
        base = baseline.get(r["name"])
        if not base:
            continue
        if base.get("qps") and r.get("qps", 0.0) < base["qps"] * (1.0 - qps_tol):
            regressions.append({"name": r["name"], "metric": "qps",
                                "baseline": base["qps"], "current": r.get("qps", 0.0)})
        if base.get("p99_us") and r.get("latency_us", {}).get("p99", 0.0) > base["p99_us"] * (1.0 + p99_tol):
            regressions.append({"name": r["name"], "metric": "p99_us",
                                "baseline": base["p99_us"],
                                "current": r.get("latency_us", {}).get("p99", 0.0)})
    return regressions


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def resolve_binaries(args: argparse.Namespace) -> Tuple[Path, Path]:
    if args.binary_dir:
        base = Path(args.binary_dir).resolve()
    else:
        base = Path(args.build_dir).resolve() / "zrpc" / "tools"
        if IS_WINDOWS:
            base = base / (args.build_type or "Debug")
    server = base / ("zrpc_bench_server" + EXE)
    loadgen = base / ("zrpc_loadgen" + EXE)
    if not server.exists() or not loadgen.exists():
        raise FileNotFoundError("bench binaries not found under {}".format(base))
    return server, loadgen


def build_tree(args: argparse.Namespace, build_dir: Path, sanitize: bool = False) -> None:
    cfg = ["cmake", "-S", str(ROOT), "-B", str(build_dir)]
    cfg += ["-DZRPC_BUILD_GTESTS=ON"]
    if sanitize:
        cfg += ["-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g",
                "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
    else:
        cfg += ["-DCMAKE_BUILD_TYPE=" + (args.build_type or "RelWithDebInfo")]
    log("$ " + " ".join(cfg))
    subprocess.run(cfg, check=True, cwd=str(ROOT))
    build = ["cmake", "--build", str(build_dir)]
    if IS_WINDOWS:
        build += ["--config", args.build_type or "Debug"]
    else:
        build += ["-j", str(os.cpu_count() or 4)]
    subprocess.run(build, check=True, cwd=str(ROOT))


def cmd_unit(args: argparse.Namespace) -> int:
    build_dir = Path(args.build_dir).resolve()
    if not args.no_build:
        build_tree(args, build_dir)
    labels = args.labels or "unit|integration"
    # CMake < 3.20 has no `ctest --test-dir`, so run from inside the build dir.
    cmd = ["ctest", "--output-on-failure", "-L", labels, "--timeout", "600"]
    if IS_WINDOWS:
        cmd += ["-C", args.build_type or "Debug"]
    log("$ " + " ".join(cmd))
    return subprocess.run(cmd, cwd=str(build_dir)).returncode


def cmd_sanitizer(args: argparse.Namespace) -> int:
    build_dir = ROOT / "build-asan"
    build_tree(args, build_dir, sanitize=True)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    ctest = ["ctest", "--output-on-failure", "-L", "unit|integration", "--timeout", "600"]
    if IS_WINDOWS:
        ctest += ["-C", args.build_type or "Debug"]
    log("$ ASAN_OPTIONS=... " + " ".join(ctest))
    rc = subprocess.run(ctest, cwd=str(build_dir), env=env).returncode
    return rc


def cmd_valgrind(args: argparse.Namespace) -> int:
    """Run the labeled tests through Memcheck on Linux."""
    if IS_WINDOWS:
        log("valgrind is supported only on Linux")
        return 2
    if shutil.which("valgrind") is None:
        log("valgrind is not installed")
        return 2

    build_dir = Path(args.build_dir).resolve()
    build_tree(args, build_dir)
    options = (
        "--tool=memcheck --leak-check=full "
        "--show-leak-kinds=definite,indirect --track-fds=yes "
        "--errors-for-leak-kinds=definite,indirect --error-exitcode=99"
    )
    valgrind = shutil.which("valgrind")
    config = build_dir / "DartConfiguration.tcl"
    lines = config.read_text(encoding="utf-8").splitlines() if config.exists() else []
    replacements = {
        "MemoryCheckCommand:": "MemoryCheckCommand: {}".format(valgrind),
        "MemoryCheckCommandOptions:": "MemoryCheckCommandOptions: {}".format(options),
    }
    updated = []
    seen = set()
    for line in lines:
        key = next((key for key in replacements if line.startswith(key)), None)
        if key:
            updated.append(replacements[key])
            seen.add(key)
        else:
            updated.append(line)
    for key, value in replacements.items():
        if key not in seen:
            updated.append(value)
    config.write_text("\n".join(updated) + "\n", encoding="utf-8")
    labels = args.labels or "unit|integration"
    cmd = ["ctest", "-T", "memcheck", "-L", labels, "--timeout", "600"]
    log("$ " + " ".join(cmd))
    return subprocess.run(cmd, cwd=str(build_dir)).returncode


def _run_suite(args: argparse.Namespace, scenarios: List[Dict[str, Any]], sample: bool,
               title: str) -> int:
    server, loadgen = resolve_binaries(args)
    run_dir = Path(args.out).resolve() / timestamp()
    run_dir.mkdir(parents=True, exist_ok=True)
    env_base = dict(os.environ)
    ports = PortAllocator()
    results: List[Dict[str, Any]] = []
    for s in scenarios:
        log("=== {} ({}) ===".format(s["name"], s["suite"]))
        r = run_scenario(server, loadgen, s, ports, run_dir, env_base, sample)
        results.append(r)
        log("    pass={} ok={} fail={} qps={:.1f} p99={:.0f}us".format(
            r.get("pass"), r.get("succeeded", 0), r.get("failed", 0), r.get("qps", 0.0),
            float(r.get("latency_us", {}).get("p99", 0.0))))
    write_reports(run_dir, results, title)
    log("\n" + "=" * 60)
    summarize_console(results)
    log("\nreport: {}".format(run_dir / "report.html"))
    log("result: {}".format(run_dir))

    regressions: List[Dict[str, Any]] = []
    if args.baseline and Path(args.baseline).exists():
        baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        regressions = compare_baseline(results, baseline, args.qps_tol, args.p99_tol)
        for reg in regressions:
            log("REGRESSION {}: {} {} -> {}".format(
                reg["name"], reg["metric"], reg["baseline"], reg["current"]))
    (run_dir / "baseline.json").write_text(json.dumps(
        {r["name"]: {"qps": r.get("qps", 0.0),
                     "p99_us": r.get("latency_us", {}).get("p99", 0.0),
                     "error_rate": r.get("error_rate", 0.0)} for r in results},
        indent=2, sort_keys=True) + "\n", encoding="utf-8")

    failed = any(not r.get("pass") for r in results) or bool(regressions)
    return 1 if failed else 0


def cmd_perf(args: argparse.Namespace) -> int:
    if not args.no_build:
        build_tree(args, Path(args.build_dir).resolve())
    return _run_suite(args, perf_scenarios(args), sample=False, title="zrpc perf matrix")


def cmd_stress(args: argparse.Namespace) -> int:
    if not args.no_build:
        build_tree(args, Path(args.build_dir).resolve())
    return _run_suite(args, stress_scenarios(args), sample=False, title="zrpc stress scenarios")


def cmd_soak(args: argparse.Namespace) -> int:
    if not args.no_build:
        build_tree(args, Path(args.build_dir).resolve())
    return _run_suite(args, soak_scenarios(args), sample=True, title="zrpc stability soak")


def cmd_all(args: argparse.Namespace) -> int:
    rc = cmd_unit(args)
    if rc != 0:
        return rc
    if not args.no_build:
        build_tree(args, Path(args.build_dir).resolve())
    rc = _run_suite(args, perf_scenarios(args), sample=False, title="zrpc perf matrix")
    if rc != 0:
        return rc
    rc = _run_suite(args, stress_scenarios(args), sample=False, title="zrpc stress scenarios")
    if rc != 0 or not args.soak:
        return rc
    return _run_suite(args, soak_scenarios(args), sample=True, title="zrpc stability soak")


def cmd_analyze(args: argparse.Namespace) -> int:
    run_dir = Path(args.run).resolve() if args.run else latest_run(Path(args.out).resolve())
    if run_dir is None or not run_dir.exists():
        log("no result directory found")
        return 1
    briefing = run_dir / "briefing.json"
    if not briefing.exists():
        log("{} has no briefing.json".format(run_dir))
        return 1
    results = json.loads(briefing.read_text(encoding="utf-8"))
    write_reports(run_dir, results, "zrpc report ({})".format(run_dir.name))
    summarize_console(results)
    regressions: List[Dict[str, Any]] = []
    if args.baseline and Path(args.baseline).exists():
        baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        regressions = compare_baseline(results, baseline, args.qps_tol, args.p99_tol)
        for reg in regressions:
            log("REGRESSION {}: {} {} -> {}".format(
                reg["name"], reg["metric"], reg["baseline"], reg["current"]))
    log("report: {}".format(run_dir / "report.html"))
    failed = any(not r.get("pass") for r in results) or bool(regressions)
    return 1 if failed else 0


def latest_run(out_dir: Path) -> Optional[Path]:
    if not out_dir.exists():
        return None
    runs = sorted([p for p in out_dir.iterdir() if p.is_dir()])
    return runs[-1] if runs else None


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="zbench", description="zrpc perf/stress/stability harness")
    p.add_argument("command", nargs="?",
                   choices=["unit", "perf", "stress", "soak", "sanitizer",
                            "valgrind", "analyze", "all"])
    p.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    p.add_argument("--binary-dir", default=None)
    p.add_argument("--out", default=str(DEFAULT_OUT_DIR))
    p.add_argument("--run", default=None, help="analyze: explicit run dir")
    p.add_argument("--baseline", default=None, help="baseline json for regression gating")
    p.add_argument("--qps-tol", type=float, default=0.10, help="allowed qps drop fraction")
    p.add_argument("--p99-tol", type=float, default=0.25, help="allowed p99 increase fraction")
    p.add_argument("--build-type", default="Debug" if IS_WINDOWS else "RelWithDebInfo")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--quick", action="store_true")
    p.add_argument("--duration", type=int, default=None)
    p.add_argument("--clients", type=int, default=None)
    p.add_argument("--transport", choices=["udp", "tcp", "both"], default="both")
    p.add_argument("--labels", default=None, help="unit: ctest -L regex")
    p.add_argument("--soak", action="store_true", help="all: also run soak")
    p.add_argument("--list", action="store_true", help="list scenarios and exit")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.list:
        for s in perf_scenarios(args) + stress_scenarios(args) + soak_scenarios(args):
            print("{:8s} {:4s} {:16s} payload={:<9d} clients={}".format(
                s["suite"], s["transport"], s["name"], s["payload"], s["clients"]))
        return 0
    if not args.command:
        parser.print_help()
        return 2
    try:
        if args.command == "unit":
            return cmd_unit(args)
        if args.command == "sanitizer":
            return cmd_sanitizer(args)
        if args.command == "valgrind":
            return cmd_valgrind(args)
        if args.command == "perf":
            return cmd_perf(args)
        if args.command == "stress":
            return cmd_stress(args)
        if args.command == "soak":
            return cmd_soak(args)
        if args.command == "all":
            return cmd_all(args)
        if args.command == "analyze":
            return cmd_analyze(args)
    except FileNotFoundError as exc:
        log("error: {}".format(exc))
        return 2
    except subprocess.CalledProcessError as exc:
        log("error: command failed: {}".format(exc))
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
