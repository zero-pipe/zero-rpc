#!/usr/bin/env python3
"""Unified quality gate for the network bridge framework.

Gate taxonomy (also used as CTest labels):

  unit       pure module / codec / config logic
  e2e        in-process loopback plus process-level server/client scenarios
  perf       process-level throughput and latency smoke (optional baseline)
  stability  process-level soak with changing payload sizes and resource checks
  fuzz       libFuzzer smoke targets (only in BUILD_FUZZERS builds)

Examples:

  python3 tools/gate.py unit
  python3 tools/gate.py e2e --transports tcp,udp,dds
  python3 tools/gate.py perf --duration 10
  python3 tools/gate.py stability --stability-duration 60
  python3 tools/gate.py all --no-build

Artifacts are written under artifacts/gate/<UTC timestamp>/ and a top-level
summary.json is always produced.
"""

import argparse
import json
import math
import os
import platform
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT / "build"
ARTIFACT_ROOT = ROOT / "artifacts" / "gate"
IS_WINDOWS = os.name == "nt"
ALL_TRANSPORTS = ("tcp", "udp", "dds", "shm")
MIB = 1024 * 1024


class GateError(RuntimeError):
    pass


class Outcome:
    def __init__(self, name: str, status: str, detail: str,
                 artifacts: Optional[str] = None,
                 metrics: Optional[Dict[str, Any]] = None) -> None:
        self.name = name
        self.status = status
        self.detail = detail
        self.artifacts = artifacts
        self.metrics = metrics or {}

    def to_dict(self) -> Dict[str, Any]:
        value = {
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
            "metrics": self.metrics,
        }
        if self.artifacts:
            value["artifacts"] = self.artifacts
        return value


class Context:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.build_dir = Path(args.build_dir).resolve()
        self.cache = read_cache(self.build_dir)
        self.multiconfig = bool(self.cache.get("CMAKE_CONFIGURATION_TYPES"))
        self.config = resolve_config(self.cache, args.config, self.multiconfig)
        self.cmake = resolve_tool(args.cmake, "cmake")
        self.ctest = resolve_tool(args.ctest, "ctest")
        self.dds_enabled = cache_bool(self.cache, "NB_ENABLE_DDS", True)
        self.tools_enabled = cache_bool(self.cache, "BUILD_TOOLS", True)
        self.tests_enabled = cache_bool(self.cache, "BUILD_TESTS", True)
        self.transports = select_transports(args, self.dds_enabled)
        self.out = Path(args.output_dir).resolve() if args.output_dir else ARTIFACT_ROOT / timestamp()
        self.out.mkdir(parents=True, exist_ok=True)
        self.env = child_env()
        self.connect_timeout = int(args.connect_timeout)
        self.dds_domain = args.dds_domain if args.dds_domain is not None else random.randint(0, 232)
        self.shm_channel = "zrpc-shm-{}-{}".format(os.getpid(), random.randint(0, 1 << 30))
        if args.command == "stability" and not args.transports:
            # Soak is intentionally short by default; ask for more transports
            # explicitly with --transports tcp,udp,dds.
            self.transports = [item for item in self.transports if item == "tcp"] or self.transports[:1]
        self.did_build = False

    def binary(self, name: str) -> Path:
        return find_binary(self, name)


def timestamp() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    tmp.replace(path)


def load_json(path: Path) -> Optional[Dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as source:
            value = json.load(source)
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    return value if isinstance(value, dict) else None


def read_cache(build_dir: Path) -> Dict[str, str]:
    cache: Dict[str, str] = {}
    path = build_dir / "CMakeCache.txt"
    if not path.is_file():
        return cache
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if ":" in key:
                key = key.split(":", 1)[0]
            cache[key] = value
    except OSError:
        pass
    return cache


def cache_bool(cache: Dict[str, str], key: str, default: bool) -> bool:
    value = cache.get(key)
    if value is None:
        return default
    return value.strip().upper() in ("ON", "TRUE", "1", "YES")


def resolve_config(cache: Dict[str, str], requested: Optional[str],
                   multiconfig: bool) -> Optional[str]:
    if requested:
        return requested
    build_type = cache.get("CMAKE_BUILD_TYPE", "").strip()
    if build_type:
        return build_type
    if multiconfig:
        return "Debug" if IS_WINDOWS else "Release"
    return None


def resolve_tool(override: Optional[str], name: str) -> Optional[str]:
    if override:
        path = Path(override)
        if path.is_file():
            return str(path)
        raise GateError("{} not found: {}".format(name, override))
    return shutil.which(name)


def child_env() -> Dict[str, str]:
    env = os.environ.copy()
    machine = platform.machine().lower()
    if IS_WINDOWS:
        root = ROOT / "3rdparty" / "cyclonedds" / "windows_amd64" / "bin"
        if root.is_dir():
            env["PATH"] = str(root) + os.pathsep + env.get("PATH", "")
        return env

    if "aarch64" in machine or "arm64" in machine:
        platform_dir = "linux_arm64"
    elif "loong" in machine:
        platform_dir = "linux_loongarch64"
    else:
        platform_dir = "linux_amd64"
    lib_dir = ROOT / "3rdparty" / "cyclonedds" / platform_dir / "lib"
    if lib_dir.is_dir():
        env["LD_LIBRARY_PATH"] = str(lib_dir) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def select_transports(args: argparse.Namespace, dds_enabled: bool) -> List[str]:
    if getattr(args, "no_dds", False):
        requested = [item for item in (args.transports or "tcp,udp,dds").split(",") if item]
        requested = [item for item in requested if item != "dds"]
    elif args.transports:
        requested = [item.strip() for item in args.transports.split(",") if item.strip()]
    else:
        requested = list(ALL_TRANSPORTS)
    result: List[str] = []
    for item in requested:
        if item not in ALL_TRANSPORTS:
            raise GateError("unknown transport '{}'; expected tcp, udp or dds".format(item))
        if item == "dds" and not dds_enabled:
            print("gate: skipping DDS (NB_ENABLE_DDS=OFF)")
            continue
        if item == "shm" and not sys.platform.startswith("linux"):
            print("gate: skipping SHM (Linux only)")
            continue
        if item not in result:
            result.append(item)
    return result


def find_binary(ctx: Context, name: str) -> Path:
    if not ctx.build_dir.is_dir():
        raise GateError("build directory not found: {}".format(ctx.build_dir))
    exe = name + (".exe" if IS_WINDOWS else "")
    candidates: List[Path] = []
    if ctx.config:
        candidates.extend([
            ctx.build_dir / "tools" / ctx.config / exe,
            ctx.build_dir / ctx.config / exe,
            ctx.build_dir / "bin" / ctx.config / exe,
        ])
    candidates.extend([
        ctx.build_dir / "tools" / exe,
        ctx.build_dir / "bin" / exe,
        ctx.build_dir / exe,
    ])
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    for candidate in ctx.build_dir.rglob(exe):
        if candidate.is_file():
            return candidate
    raise GateError("{} not found under {} (build with BUILD_TOOLS=ON)".format(name, ctx.build_dir))


def run_command(command: List[str], cwd: Optional[Path] = None, env: Optional[Dict[str, str]] = None,
                timeout: Optional[float] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
    )


def ensure_build(ctx: Context, require_tools: bool = False, require_tests: bool = False) -> None:
    if require_tools and not ctx.tools_enabled:
        raise GateError("BUILD_TOOLS=OFF in {}; reconfigure with -DBUILD_TOOLS=ON".format(ctx.build_dir))
    if require_tests and not ctx.tests_enabled:
        raise GateError("BUILD_TESTS=OFF in {}; reconfigure with -DBUILD_TESTS=ON".format(ctx.build_dir))
    if not ctx.args.no_build:
        if ctx.did_build:
            return
        if not (ctx.build_dir / "CMakeCache.txt").is_file():
            raise GateError(
                "build directory is not configured: {}\n"
                "run: cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_TOOLS=ON".format(ctx.build_dir)
            )
        if not ctx.cmake:
            raise GateError("cmake not found; pass --cmake or add it to PATH")
        command = [ctx.cmake, "--build", str(ctx.build_dir)]
        if ctx.multiconfig and ctx.config:
            command.extend(["--config", ctx.config])
        command.append("--parallel")
        print("gate: building {} ...".format(ctx.build_dir))
        result = run_command(command)
        if result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.returncode != 0:
            raise GateError("build failed with exit code {}".format(result.returncode))
        ctx.did_build = True


def parse_ctest_count(output: str) -> int:
    match = re.search(r"Total Tests:\s*(\d+)", output)
    return int(match.group(1)) if match else 0


def run_ctest(ctx: Context, label: str, out_dir: Path) -> Outcome:
    out_dir.mkdir(parents=True, exist_ok=True)
    if not ctx.ctest:
        return Outcome(label, "skip", "ctest not found")
    list_command = [ctx.ctest, "-N"]
    if ctx.multiconfig and ctx.config:
        list_command.extend(["-C", ctx.config])
    list_command.extend(["-L", label])
    list_result = run_command(list_command, cwd=ctx.build_dir, env=ctx.env)
    count = parse_ctest_count(list_result.stdout)
    write_text(out_dir / "list.log", list_result.stdout)
    if count == 0:
        return Outcome(label, "skip", "no tests with label '{}'".format(label))

    command = [ctx.ctest]
    if ctx.multiconfig and ctx.config:
        command.extend(["-C", ctx.config])
    command.extend(["-L", label, "--output-on-failure"])
    print("gate: ctest -L {} ({} tests) ...".format(label, count))
    result = run_command(command, cwd=ctx.build_dir, env=ctx.env, timeout=max(300, count * 120))
    write_text(out_dir / "ctest.log", result.stdout)
    status = "pass" if result.returncode == 0 else "fail"
    detail = "{} tests".format(count)
    if status == "fail":
        detail += " (exit {})".format(result.returncode)
    return Outcome(label, status, detail, artifacts=str(out_dir), metrics={"tests": count})


class ServerProcess:
    def __init__(self, command: List[str], log_path: Path, env: Dict[str, str]) -> None:
        self.command = command
        self.log_path = log_path
        self.env = env
        self.proc: Optional[subprocess.Popen] = None
        self.log = None

    def start(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log = self.log_path.open("w", encoding="utf-8", errors="replace")
        kwargs: Dict[str, Any] = {}
        if IS_WINDOWS:
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["start_new_session"] = True
        self.proc = subprocess.Popen(
            self.command,
            cwd=str(ROOT),
            env=self.env,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            text=True,
            **kwargs
        )

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def wait_ready(self, timeout: float = 20.0) -> None:
        deadline = time.time() + timeout
        marker = "ready:"
        while time.time() < deadline:
            if self.proc is None:
                raise GateError("server was not started")
            if self.proc.poll() is not None:
                tail = read_tail(self.log_path)
                raise GateError("server exited before ready (code {}); log tail:\n{}".format(
                    self.proc.returncode, tail))
            try:
                if marker in self.log_path.read_text(encoding="utf-8", errors="replace"):
                    return
            except OSError:
                pass
            time.sleep(0.1)
        raise GateError("server did not become ready within {:.0f}s; log tail:\n{}".format(
            timeout, read_tail(self.log_path)))

    def stop(self, grace: float = 5.0) -> Optional[int]:
        """Stop the server.

        Returns None when the gate intentionally stopped a still-running
        process, or the process exit code when it had already exited. This
        distinguishes a clean teardown (Windows terminate, POSIX SIGINT)
        from a crash during the scenario.
        """
        if self.proc is None:
            return -1
        if self.proc.poll() is not None:
            code = self.proc.returncode
        else:
            if IS_WINDOWS:
                self.proc.terminate()
            else:
                self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=grace)
            code = None
        if self.log is not None:
            self.log.close()
            self.log = None
        return code


def read_tail(path: Path, limit: int = 2000) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return text[-limit:]


def free_port(kind: str) -> int:
    socket_type = socket.SOCK_STREAM if kind == "tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, socket_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def percentile_from_histogram(histogram: Dict[int, int], percentile: float) -> float:
    total = sum(histogram.values())
    if total <= 0:
        return 0.0
    target = max(1, int(math.ceil(total * percentile)))
    seen = 0
    for key in sorted(histogram):
        seen += histogram[key]
        if seen >= target:
            return float(key)
    return float(max(histogram))


def metrics_from_result(data: Dict[str, Any]) -> Dict[str, Any]:
    succeeded = int(data.get("succeeded", 0) or 0)
    duration = float(data.get("duration_seconds", 0.0) or 0.0)
    histogram: Dict[int, int] = {}
    for key, value in (data.get("latency_histogram_us") or {}).items():
        try:
            histogram[int(key)] = int(value)
        except (TypeError, ValueError):
            continue
    latency = data.get("latency_us") or {}
    node_metrics = data.get("metrics") or {}
    return {
        "attempted": int(data.get("attempted", 0) or 0),
        "succeeded": succeeded,
        "failed": int(data.get("failed", 0) or 0),
        "qps": (float(succeeded) / duration) if duration > 0 else 0.0,
        "duration_seconds": duration,
        "p50_us": percentile_from_histogram(histogram, 0.50),
        "p90_us": percentile_from_histogram(histogram, 0.90),
        "p99_us": percentile_from_histogram(histogram, 0.99),
        "min_us": float(latency.get("min", 0.0) or 0.0),
        "mean_us": float(latency.get("mean", 0.0) or 0.0),
        "max_us": float(latency.get("max", 0.0) or 0.0),
        "timeouts": int(node_metrics.get("timeouts", 0) or 0),
        "retries": int(node_metrics.get("retries", 0) or 0),
        "udp_retransmissions": int(node_metrics.get("udp_retransmissions", 0) or 0),
    }


def evaluate_result(data: Optional[Dict[str, Any]], min_succeeded: int,
                    expect_bytes: int, label: str) -> Tuple[bool, List[str], Dict[str, Any]]:
    if data is None:
        return False, ["{} did not produce result JSON".format(label)], {}
    messages: List[str] = []
    if str(data.get("status", "")) != "ok":
        messages.append("status={}".format(data.get("status")))
    succeeded = int(data.get("succeeded", 0) or 0)
    failed = int(data.get("failed", 0) or 0)
    if failed:
        messages.append("failed={}".format(failed))
    if succeeded < min_succeeded:
        messages.append("succeeded={} < {}".format(succeeded, min_succeeded))
    if expect_bytes:
        response_bytes = int(data.get("response_bytes", 0) or 0)
        expected = succeeded * expect_bytes
        if response_bytes != expected:
            messages.append("response_bytes={} != {}".format(response_bytes, expected))
    errors = data.get("errors") or {}
    if errors:
        messages.append("errors={}".format(errors))
    return not messages, messages, metrics_from_result(data)


def server_command(binary: Path, transport: str, port: int, dds_domain: int,
                   response_bytes: Optional[int], echo: bool,
                   io_threads: int, handler_threads: int,
                   max_msg_bytes: int = MIB, shm_channel: str = "") -> List[str]:
    command = [str(binary)]
    if transport == "dds":
        command.extend(["--dds", "--dds-domain", str(dds_domain)])
    elif transport == "shm":
        command.extend(["--shm", "--shm-channel", shm_channel])
    else:
        command.extend([
            "--transport", transport,
            "--bind", "127.0.0.1",
            "--data-port", str(port),
        ])
    command.extend([
        "--mode", "static",
        "--io-threads", str(io_threads),
        "--handler-threads", str(handler_threads),
        "--worker",
        "--max-msg-bytes", str(max_msg_bytes),
    ])
    if echo:
        command.append("--echo-request")
    elif response_bytes:
        command.extend(["--response-bytes", str(response_bytes)])
    return command


def loadgen_command(binary: Path, transport: str, port: int, dds_domain: int,
                    duration: int, payload_bytes: int, expect_bytes: int,
                    output: Path, max_requests: int = 0, rate: float = 0.0,
                    verify: bool = False, timeout_ms: int = 5000,
                    connect_timeout: int = 20, max_msg_bytes: int = MIB,
                    shm_channel: str = "") -> List[str]:
    command = [str(binary)]
    if transport == "dds":
        command.extend(["--dds", "--dds-domain", str(dds_domain)])
    elif transport == "shm":
        command.extend(["--shm", "--shm-channel", shm_channel])
    else:
        command.extend([
            "--transport", transport,
            "--target", "127.0.0.1:{}".format(port),
        ])
    command.extend([
        "--mode", "static",
        "--service", "OrderService",
        "--method", "get",
        "--duration-seconds", str(duration),
        "--payload-bytes", str(payload_bytes),
        "--expect-response-bytes", str(expect_bytes),
        "--window", "1",
        "--timeout-ms", str(timeout_ms),
        "--connect-timeout-seconds", str(connect_timeout),
        "--io-threads", "1",
        "--max-msg-bytes", str(max_msg_bytes),
        "--output", str(output),
    ])
    if max_requests:
        command.extend(["--max-requests", str(max_requests)])
    if rate > 0:
        command.extend(["--rate", "{:.3f}".format(rate)])
    if verify:
        command.append("--verify-response")
    return command


def run_client(ctx: Context, transport: str, port: int, case_dir: Path,
               duration: int, payload_bytes: int, expect_bytes: int,
               max_requests: int = 0, rate: float = 0.0, verify: bool = False,
               timeout_ms: int = 5000) -> Optional[Dict[str, Any]]:
    case_dir.mkdir(parents=True, exist_ok=True)
    result_path = case_dir / "result.json"
    command = loadgen_command(
        ctx.binary("loadgen"), transport, port, ctx.dds_domain,
        duration, payload_bytes, expect_bytes, result_path,
        max_requests=max_requests, rate=rate, verify=verify,
        timeout_ms=timeout_ms, connect_timeout=ctx.connect_timeout,
        shm_channel=ctx.shm_channel,
    )
    try:
        result = run_command(command, cwd=ROOT, env=ctx.env,
                             timeout=duration + ctx.connect_timeout + 60)
        write_text(case_dir / "client.log",
                   (result.stdout or "") + "\n[exit={}]\n".format(result.returncode))
    except subprocess.TimeoutExpired:
        write_text(case_dir / "client.log", "[timeout]\n")
    return load_json(result_path)


def run_scenario(ctx: Context, transport: str, case_dir: Path, duration: int,
                 payload_bytes: int, expect_bytes: int, *,
                 max_requests: int = 0, rate: float = 0.0, verify: bool = False,
                 echo_server: bool = False, io_threads: int = 6,
                 handler_threads: int = 2) -> Tuple[Optional[Dict[str, Any]], Optional[int]]:
    case_dir.mkdir(parents=True, exist_ok=True)
    port = free_port(transport) if transport in ("tcp", "udp") else 0
    command = server_command(
        ctx.binary("bench_server"), transport, port, ctx.dds_domain,
        None if echo_server else expect_bytes, echo_server,
        io_threads, handler_threads, shm_channel=ctx.shm_channel,
    )
    server = ServerProcess(command, case_dir / "server.log", ctx.env)
    server.start()
    try:
        server.wait_ready(20.0)
        data = run_client(
            ctx, transport, port, case_dir, duration, payload_bytes,
            expect_bytes, max_requests=max_requests, rate=rate, verify=verify,
        )
    finally:
        server_code = server.stop()
    write_text(case_dir / "server.exit", "{}\n".format(server_code))
    return data, server_code


def gate_unit(ctx: Context) -> Outcome:
    ensure_build(ctx, require_tests=True)
    return run_ctest(ctx, "unit", ctx.out / "unit")


def gate_fuzz(ctx: Context) -> Outcome:
    ensure_build(ctx)
    return run_ctest(ctx, "fuzz", ctx.out / "fuzz")


def gate_e2e(ctx: Context) -> Outcome:
    ensure_build(ctx, require_tools=True, require_tests=True)
    out_dir = ctx.out / "e2e"
    cases: List[Outcome] = []
    ctest = run_ctest(ctx, "e2e", out_dir / "ctest")
    cases.append(ctest)
    for transport in ctx.transports:
        case_dir = out_dir / transport
        try:
            data, server_code = run_scenario(
                ctx, transport, case_dir,
                duration=20, payload_bytes=1360, expect_bytes=1360,
                max_requests=ctx.args.e2e_requests,
                io_threads=2, handler_threads=1,
            )
            ok, messages, metrics = evaluate_result(
                data, ctx.args.e2e_requests, 1360, "e2e.{}".format(transport))
            if server_code not in (None, 0):
                ok = False
                messages.append("server exit={}".format(server_code))
            cases.append(Outcome(
                "e2e.{}".format(transport),
                "pass" if ok else "fail",
                "; ".join(messages) if messages else "ok",
                artifacts=str(case_dir), metrics=metrics,
            ))
        except Exception as exc:
            cases.append(Outcome("e2e.{}".format(transport), "fail", str(exc),
                                 artifacts=str(case_dir)))
    return combine_outcomes("e2e", cases, out_dir)


def gate_perf(ctx: Context) -> Outcome:
    ensure_build(ctx, require_tools=True)
    out_dir = ctx.out / "perf"
    cases: List[Outcome] = []
    metrics_by_transport: Dict[str, Dict[str, Any]] = {}
    for transport in ctx.transports:
        case_dir = out_dir / transport
        try:
            data, server_code = run_scenario(
                ctx, transport, case_dir,
                duration=ctx.args.perf_duration, payload_bytes=1360, expect_bytes=1360,
                io_threads=6, handler_threads=2,
            )
            ok, messages, metrics = evaluate_result(
                data, 1, 1360, "perf.{}".format(transport))
            if server_code not in (None, 0):
                ok = False
                messages.append("server exit={}".format(server_code))
            metrics_by_transport[transport] = metrics
            cases.append(Outcome(
                "perf.{}".format(transport),
                "pass" if ok else "fail",
                "; ".join(messages) if messages else "ok",
                artifacts=str(case_dir), metrics=metrics,
            ))
        except Exception as exc:
            cases.append(Outcome("perf.{}".format(transport), "fail", str(exc),
                                 artifacts=str(case_dir)))
    write_json(out_dir / "metrics.json", metrics_by_transport)
    baseline_outcome = apply_baseline(ctx, metrics_by_transport, out_dir)
    if baseline_outcome is not None:
        cases.append(baseline_outcome)
    return combine_outcomes("perf", cases, out_dir)


def apply_baseline(ctx: Context, metrics: Dict[str, Dict[str, Any]],
                   out_dir: Path) -> Optional[Outcome]:
    baseline_path = Path(ctx.args.baseline).resolve() if ctx.args.baseline else None
    if ctx.args.update_baseline:
        target = baseline_path or (ROOT / "tests" / "perf" / "baseline.json")
        write_json(target, metrics)
        return Outcome("perf.baseline", "pass", "wrote baseline {}".format(target),
                       artifacts=str(target))
    if baseline_path is None:
        return None
    baseline = load_json(baseline_path)
    if baseline is None:
        return Outcome("perf.baseline", "fail",
                       "baseline not found or invalid: {}".format(baseline_path))
    tolerance = float(ctx.args.tolerance)
    messages: List[str] = []
    for transport, current in metrics.items():
        previous = baseline.get(transport)
        if not isinstance(previous, dict):
            continue
        old_qps = float(previous.get("qps", 0.0) or 0.0)
        old_p99 = float(previous.get("p99_us", 0.0) or 0.0)
        if old_qps > 0 and current["qps"] < old_qps * (1.0 - tolerance):
            messages.append("{} qps {:.1f} < {:.1f} (baseline {:.1f} -{:.0f}%)".format(
                transport, current["qps"], old_qps * (1.0 - tolerance), old_qps,
                tolerance * 100.0))
        if old_p99 > 0 and current["p99_us"] > old_p99 * (1.0 + tolerance):
            messages.append("{} p99 {:.0f}us > {:.0f}us (baseline {:.0f}us +{:.0f}%)".format(
                transport, current["p99_us"], old_p99 * (1.0 + tolerance), old_p99,
                tolerance * 100.0))
    write_json(out_dir / "baseline.json", {
        "path": str(baseline_path),
        "tolerance": tolerance,
        "violations": messages,
    })
    return Outcome(
        "perf.baseline",
        "fail" if messages else "pass",
        "; ".join(messages) if messages else "within {:.0f}% tolerance".format(tolerance * 100.0),
    )


def read_proc_sample(pid: int) -> Optional[Dict[str, float]]:
    if IS_WINDOWS:
        return None
    proc = Path("/proc") / str(pid)
    try:
        status = (proc / "status").read_text(encoding="utf-8", errors="replace")
        fd_count = len(list((proc / "fd").iterdir()))
    except (FileNotFoundError, PermissionError, OSError):
        return None
    rss_kb = 0.0
    threads = 0.0
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            rss_kb = float(line.split()[1])
        elif line.startswith("Threads:"):
            threads = float(line.split()[1])
    return {"rss_kb": rss_kb, "threads": threads, "fd_count": float(fd_count)}


class ResourceSampler(threading.Thread):
    def __init__(self, pid: int, interval: float = 0.5) -> None:
        super().__init__(name="gate-resource", daemon=True)
        self.pid = pid
        self.interval = interval
        self.samples: List[Dict[str, float]] = []
        self._stop_event = threading.Event()

    def run(self) -> None:
        while not self._stop_event.wait(self.interval):
            sample = read_proc_sample(self.pid)
            if sample is None:
                return
            self.samples.append(sample)

    def stop(self) -> None:
        self._stop_event.set()
        self.join(timeout=max(2.0, self.interval * 4.0))

    def summary(self) -> Dict[str, Any]:
        if not self.samples:
            return {}
        first = self.samples[0]
        last = self.samples[-1]
        return {
            "samples": len(self.samples),
            "rss_kb": {
                "initial": first["rss_kb"],
                "final": last["rss_kb"],
                "peak": max(item["rss_kb"] for item in self.samples),
                "growth": last["rss_kb"] - first["rss_kb"],
                "peak_growth": max(item["rss_kb"] for item in self.samples) - first["rss_kb"],
            },
            "threads": {
                "initial": first["threads"],
                "final": last["threads"],
                "peak": max(item["threads"] for item in self.samples),
                "growth": last["threads"] - first["threads"],
            },
            "fd_count": {
                "initial": first["fd_count"],
                "final": last["fd_count"],
                "peak": max(item["fd_count"] for item in self.samples),
                "growth": last["fd_count"] - first["fd_count"],
            },
        }


def stability_waves(total_seconds: int, transport: str) -> List[Tuple[int, float, int]]:
    payloads = (64, 1360, 8192, 65536)
    rates = (400.0, 200.0, 80.0, 20.0) if transport != "dds" else (250.0, 150.0, 60.0, 15.0)
    wave_seconds = max(2, total_seconds // len(payloads))
    return [(payloads[i], rates[i], wave_seconds) for i in range(len(payloads))]


def gate_stability(ctx: Context) -> Outcome:
    ensure_build(ctx, require_tools=True)
    out_dir = ctx.out / "stability"
    cases: List[Outcome] = []
    for transport in ctx.transports:
        cases.append(stability_case(ctx, transport, out_dir / transport,
                                    ctx.args.stability_duration))
    return combine_outcomes("stability", cases, out_dir)


def stability_case(ctx: Context, transport: str, case_dir: Path,
                   total_seconds: int) -> Outcome:
    case_dir.mkdir(parents=True, exist_ok=True)
    port = free_port(transport) if transport in ("tcp", "udp") else 0
    command = server_command(
        ctx.binary("bench_server"), transport, port, ctx.dds_domain,
        None, True, 2, 1, shm_channel=ctx.shm_channel,
    )
    server = ServerProcess(command, case_dir / "server.log", ctx.env)
    server.start()
    sampler: Optional[ResourceSampler] = None
    waves: List[Dict[str, Any]] = []
    total_failed = 0
    try:
        server.wait_ready(20.0)
        sampler = ResourceSampler(server.proc.pid if server.proc else -1)
        sampler.start()
        for index, (payload, rate, seconds) in enumerate(stability_waves(total_seconds, transport)):
            wave_dir = case_dir / "wave_{}_{}b".format(index, payload)
            data = run_client(
                ctx, transport, port, wave_dir, duration=seconds,
                payload_bytes=payload, expect_bytes=payload,
                rate=rate, verify=True, timeout_ms=max(8000, seconds * 1000),
            )
            ok, messages, metrics = evaluate_result(
                data, 1, payload, "stability.{}.wave{}".format(transport, index))
            total_failed += metrics.get("failed", 0)
            waves.append({
                "index": index,
                "payload_bytes": payload,
                "rate": rate,
                "duration_seconds": seconds,
                "status": "pass" if ok else "fail",
                "messages": messages,
                "metrics": metrics,
            })
            if not server.alive():
                waves[-1]["status"] = "fail"
                waves[-1]["messages"].append("server exited during wave")
                break
            time.sleep(0.5)
    except Exception as exc:
        waves.append({"status": "fail", "messages": [str(exc)], "metrics": {}})
    finally:
        if sampler is not None:
            sampler.stop()
        server_code = server.stop()
    resource = sampler.summary() if sampler is not None else {}
    messages: List[str] = []
    failed_waves = [wave for wave in waves if wave.get("status") != "pass"]
    if failed_waves:
        messages.append("{} wave(s) failed".format(len(failed_waves)))
    if total_failed:
        messages.append("{} request(s) failed".format(total_failed))
    if server_code not in (None, 0):
        messages.append("server exit={}".format(server_code))
    if resource:
        rss_growth_mb = float(resource["rss_kb"]["peak_growth"]) / 1024.0
        thread_growth = float(resource["threads"]["growth"])
        fd_growth = float(resource["fd_count"]["peak"]) - float(resource["fd_count"]["initial"])
        if rss_growth_mb > float(ctx.args.max_rss_growth_mb):
            messages.append("rss peak growth {:.1f}MB > {:.1f}MB".format(
                rss_growth_mb, float(ctx.args.max_rss_growth_mb)))
        if thread_growth > float(ctx.args.max_thread_growth):
            messages.append("thread growth {:.0f} > {:.0f}".format(
                thread_growth, float(ctx.args.max_thread_growth)))
        if fd_growth > float(ctx.args.max_fd_growth):
            messages.append("fd peak growth {:.0f} > {:.0f}".format(
                fd_growth, float(ctx.args.max_fd_growth)))
    detail = "{} waves, {} failed".format(len(waves), total_failed)
    summary = {
        "transport": transport,
        "status": "pass" if not messages else "fail",
        "waves": waves,
        "resource": resource,
        "server_exit": server_code,
        "messages": messages,
    }
    write_json(case_dir / "summary.json", summary)
    return Outcome(
        "stability.{}".format(transport),
        "pass" if not messages else "fail",
        "; ".join(messages) if messages else detail,
        artifacts=str(case_dir),
        metrics={"waves": len(waves), "failed_requests": total_failed,
                 "resource": resource},
    )


def combine_outcomes(name: str, cases: List[Outcome], out_dir: Path) -> Outcome:
    if not cases:
        return Outcome(name, "skip", "no cases selected", artifacts=str(out_dir))
    failed = [case for case in cases if case.status == "fail"]
    passed = [case for case in cases if case.status == "pass"]
    status = "fail" if failed else ("pass" if passed else "skip")
    if failed:
        detail = "{} of {} case(s) failed".format(len(failed), len(cases))
    else:
        detail = "{} case(s) passed".format(len(passed))
    write_json(out_dir / "summary.json", {
        "name": name,
        "status": status,
        "detail": detail,
        "cases": [case.to_dict() for case in cases],
    })
    return Outcome(name, status, detail, artifacts=str(out_dir),
                   metrics={"cases": [case.to_dict() for case in cases]})


def write_summary(ctx: Context, outcomes: List[Outcome]) -> Dict[str, Any]:
    failed = any(outcome.status == "fail" for outcome in outcomes)
    summary = {
        "schema_version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "build_dir": str(ctx.build_dir),
        "config": ctx.config,
        "transports": ctx.transports,
        "dds_domain": ctx.dds_domain,
        "status": "fail" if failed else "pass",
        "gates": [outcome.to_dict() for outcome in outcomes],
    }
    write_json(ctx.out / "summary.json", summary)
    return summary


def print_report(ctx: Context, outcomes: List[Outcome]) -> None:
    print("")
    print("gate artifacts: {}".format(ctx.out))
    print("{:<12} {:<6} {}".format("GATE", "STATUS", "DETAIL"))
    print("{:<12} {:<6} {}".format("-" * 12, "-" * 6, "-" * 40))
    for outcome in outcomes:
        print("{:<12} {:<6} {}".format(outcome.name, outcome.status.upper(), outcome.detail))
    failed = any(outcome.status == "fail" for outcome in outcomes)
    print("")
    print("result: {}".format("FAIL" if failed else "PASS"))


def make_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR),
                        help="CMake build directory (default: build)")
    common.add_argument("--config", default=None,
                        help="build configuration for multi-config generators")
    common.add_argument("--no-build", action="store_true",
                        help="do not run cmake --build before the gate")
    common.add_argument("--cmake", default=None, help="path to cmake")
    common.add_argument("--ctest", default=None, help="path to ctest")
    common.add_argument("--output-dir", default=None,
                        help="artifact directory (default: artifacts/gate/<timestamp>)")
    common.add_argument("--transports", default=None,
                        help="comma-separated transports: tcp,udp,dds")
    common.add_argument("--no-dds", action="store_true", help="skip DDS cases")
    common.add_argument("--dds-domain", type=int, default=None,
                        help="DDS domain (default: random 0..232)")
    common.add_argument("--connect-timeout", type=int, default=20,
                        help="loadgen warm-up deadline in seconds")

    parser = argparse.ArgumentParser(
        description="network bridge quality gate",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    unit = subparsers.add_parser("unit", parents=[common],
                                 help="run CTest unit tests")
    unit.set_defaults(handler=gate_unit)

    e2e = subparsers.add_parser("e2e", parents=[common],
                                help="run CTest e2e tests and process-level echo cases")
    e2e.add_argument("--e2e-requests", type=int, default=200,
                     help="requests per transport (default: 200)")
    e2e.set_defaults(handler=gate_e2e)

    perf = subparsers.add_parser("perf", parents=[common],
                                 help="run process-level throughput/latency smoke")
    perf.add_argument("--perf-duration", type=int, default=5,
                      help="seconds per transport (default: 5)")
    perf.add_argument("--baseline", default=None,
                      help="baseline JSON for QPS/p99 regression checks")
    perf.add_argument("--update-baseline", action="store_true",
                      help="write current metrics to the baseline path")
    perf.add_argument("--tolerance", type=float, default=0.20,
                      help="baseline tolerance ratio (default: 0.20)")
    perf.set_defaults(handler=gate_perf)

    stability = subparsers.add_parser("stability", parents=[common],
                                      help="run process-level soak waves")
    stability.add_argument("--stability-duration", type=int, default=30,
                           help="total soak seconds per transport (default: 30)")
    stability.add_argument("--max-rss-growth-mb", type=float, default=64.0)
    stability.add_argument("--max-thread-growth", type=float, default=8.0)
    stability.add_argument("--max-fd-growth", type=float, default=32.0)
    stability.set_defaults(handler=gate_stability)

    fuzz = subparsers.add_parser("fuzz", parents=[common],
                                 help="run CTest fuzz smoke tests")
    fuzz.set_defaults(handler=gate_fuzz)

    all_gate = subparsers.add_parser("all", parents=[common],
                                     help="run unit, e2e, perf and stability")
    all_gate.add_argument("--e2e-requests", type=int, default=200)
    all_gate.add_argument("--perf-duration", type=int, default=5)
    all_gate.add_argument("--baseline", default=None)
    all_gate.add_argument("--update-baseline", action="store_true")
    all_gate.add_argument("--tolerance", type=float, default=0.20)
    all_gate.add_argument("--stability-duration", type=int, default=30)
    all_gate.add_argument("--max-rss-growth-mb", type=float, default=64.0)
    all_gate.add_argument("--max-thread-growth", type=float, default=8.0)
    all_gate.add_argument("--max-fd-growth", type=float, default=32.0)
    all_gate.set_defaults(handler=None)

    subparsers.add_parser("list", parents=[common],
                          help="list gate categories")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.command == "list":
        print("gate categories:")
        for name, detail in (
            ("unit", "pure module / codec / config logic"),
            ("e2e", "in-process loopback + process-level echo cases"),
            ("perf", "throughput/latency smoke with optional baseline"),
            ("stability", "soak waves with resource-growth checks"),
            ("fuzz", "libFuzzer smoke targets"),
        ):
            print("  {:<10} {}".format(name, detail))
        return 0

    try:
        ctx = Context(args)
    except GateError as exc:
        print("gate: {}".format(exc), file=sys.stderr)
        return 2

    outcomes: List[Outcome] = []
    try:
        if args.command == "all":
            # Stability defaults to TCP only; pass --transports to widen it.
            outcomes.append(gate_unit(ctx))
            outcomes.append(gate_e2e(ctx))
            outcomes.append(gate_perf(ctx))
            all_transports = ctx.transports
            ctx.transports = [item for item in all_transports if item == "tcp"] or all_transports[:1]
            outcomes.append(gate_stability(ctx))
            ctx.transports = all_transports
        else:
            outcomes.append(args.handler(ctx))
    except GateError as exc:
        print("gate: {}".format(exc), file=sys.stderr)
        failed = Outcome(args.command, "fail", str(exc))
        outcomes.append(failed)
    except Exception:
        traceback.print_exc()
        outcomes.append(Outcome(args.command, "fail", "unexpected error"))

    summary = write_summary(ctx, outcomes)
    print_report(ctx, outcomes)
    return 0 if summary["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
