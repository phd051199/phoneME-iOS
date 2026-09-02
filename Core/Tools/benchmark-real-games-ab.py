#!/usr/bin/env python3
"""Run a same-JAR baseline/candidate phoneME A/B without touching the worktree.

The baseline is built in a detached temporary git worktree. The candidate is
the current source tree, including intentional uncommitted optimization work.
Both sides use the same pinned JAR files, observation durations, autoplay and
display hints from performance-benchmarks.json.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any


SCRIPT_PATH = pathlib.Path(__file__).resolve()
CORE_ROOT = SCRIPT_PATH.parent.parent
REPO_ROOT = CORE_ROOT.parent
DEFAULT_MANIFEST = SCRIPT_PATH.parent / "performance-benchmarks.json"


def run(
    command: list[str],
    *,
    cwd: pathlib.Path,
    env: dict[str, str],
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, cwd=cwd, env=env, check=check, text=True)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    benchmarks = data.get("benchmarks")
    if not isinstance(benchmarks, list) or not benchmarks:
        raise SystemExit(f"benchmark manifest has no benchmarks: {path}")
    for benchmark in benchmarks:
        if not isinstance(benchmark, dict):
            raise SystemExit("benchmark manifest contains a non-object entry")
        relative = pathlib.Path(str(benchmark.get("jar", "")))
        jar = REPO_ROOT / relative
        if not jar.is_file():
            raise SystemExit(f"missing benchmark JAR: {relative}")
        actual = sha256(jar)
        expected = str(benchmark.get("sha256", "")).lower()
        if actual != expected:
            raise SystemExit(
                f"benchmark JAR hash mismatch for {relative}: {actual} != {expected}"
            )
    return data


PERF_LINE = re.compile(r"^\[phoneME-perf\]\s+(.*)$")
KEY_VALUE = re.compile(r"([A-Za-z0-9_]+)=(-?[0-9]+(?:\.[0-9]+)?)")


def parse_perf_log(path: pathlib.Path) -> dict[str, float]:
    metrics: dict[str, float] = {}
    if not path.is_file():
        return metrics
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = PERF_LINE.match(raw_line)
        if not match:
            continue
        body = match.group(1)
        first_token = body.split(maxsplit=1)[0] if body else "perf"
        category = first_token if "=" not in first_token else "execution"
        for key, value in KEY_VALUE.findall(body):
            parsed = float(value)
            # Keep both a qualified key and the first unqualified occurrence.
            metrics[f"{category}.{key}"] = parsed
            metrics.setdefault(key, parsed)
    return metrics


RUNNER_METRICS = (
    "startup_ms",
    "frames_produced",
    "frame_changes",
    "unique_frames",
    "process_cpu_ms",
    "wall_time_ms",
    "cpu_utilization_pct",
    "cpu_ms_per_generated_frame",
    "frame_interval_p50_ms",
    "frame_interval_p95_ms",
    "frame_interval_p99_ms",
    "frame_hitches_over_50ms",
    "frame_interval_samples",
    "longest_idle_ms",
)


def report_json_path(report_md: pathlib.Path) -> pathlib.Path:
    return report_md.with_suffix(".json")


def extract_report(report_md: pathlib.Path) -> dict[str, Any]:
    path = report_json_path(report_md)
    data = json.loads(path.read_text(encoding="utf-8"))
    extracted: dict[str, Any] = {}
    for item in data.get("items", []):
        jar_name = pathlib.Path(str(item.get("jar", ""))).name
        observed = item.get("observed") or {}
        metrics: dict[str, float] = {}
        for key in RUNNER_METRICS:
            value = observed.get(key)
            if isinstance(value, (int, float)):
                metrics[key] = float(value)
        stderr_path = pathlib.Path(str((item.get("artifacts") or {}).get("stderr", "")))
        # Older baselines may come from a test-jar-directory version that did
        # not promote CPU/frame-timing fields into `observed`. The harness has
        # always written those fields to runner-result.json, so read that raw
        # sidecar as a backwards-compatible source for A/B metrics.
        runner_result_path = stderr_path.parent / "runner-result.json"
        if runner_result_path.is_file():
            try:
                runner_result = json.loads(
                    runner_result_path.read_text(encoding="utf-8")
                )
            except (OSError, json.JSONDecodeError):
                runner_result = {}
            for key in RUNNER_METRICS:
                value = runner_result.get(key)
                if key not in metrics and isinstance(value, (int, float)):
                    metrics[key] = float(value)
        perf = parse_perf_log(stderr_path)
        for key, value in perf.items():
            metrics[f"perf.{key}"] = value
        extracted[jar_name] = {
            "status": item.get("status", ""),
            "failures": item.get("failures", []),
            "metrics": metrics,
        }
    return extracted


def percent_delta(baseline: float, candidate: float) -> float | None:
    if baseline == 0.0:
        return None
    return ((candidate - baseline) / baseline) * 100.0


COMPARE_KEYS = (
    "process_cpu_ms",
    "cpu_ms_per_generated_frame",
    "frame_interval_p50_ms",
    "frame_interval_p95_ms",
    "frame_interval_p99_ms",
    "frame_hitches_over_50ms",
    "perf.bytecodes",
    "perf.allocations.bytes",
    "perf.heap_locked",
    "perf.heap_fast",
    "perf.gc.count",
    "perf.gc.total_ms",
    "perf.gc.max_pause_ms",
    "perf.gc.roots_scanned",
    "perf.scheduler.yields",
    "perf.scheduler.sleeps",
    "perf.scheduler.wakeups",
    "perf.jit.compile_ms",
    "perf.jit.exec_ms",
    "perf.jit.deopt",
    "perf.jit.osr",
    "perf.canvas.publish_ms",
)


def find_metric(metrics: dict[str, float], requested: str) -> float | None:
    if requested in metrics:
        return metrics[requested]
    # The textual perf format evolved over time. Fall back to the final token
    # only when it resolves unambiguously enough for a baseline comparison.
    tail = requested.rsplit(".", 1)[-1]
    candidates = [value for key, value in metrics.items() if key.endswith("." + tail)]
    if len(candidates) == 1:
        return candidates[0]
    return None


def compare(
    manifest: dict[str, Any], baseline: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, Any]:
    items: list[dict[str, Any]] = []
    for benchmark in manifest["benchmarks"]:
        jar_name = pathlib.Path(benchmark["jar"]).name
        base = baseline.get(jar_name, {})
        cand = candidate.get(jar_name, {})
        base_metrics = base.get("metrics", {})
        cand_metrics = cand.get("metrics", {})
        deltas: dict[str, Any] = {}
        for key in COMPARE_KEYS:
            base_value = find_metric(base_metrics, key)
            cand_value = find_metric(cand_metrics, key)
            if base_value is None or cand_value is None:
                continue
            deltas[key] = {
                "baseline": base_value,
                "candidate": cand_value,
                "delta_percent": percent_delta(base_value, cand_value),
            }
        items.append(
            {
                "id": benchmark["id"],
                "jar": benchmark["jar"],
                "baseline_status": base.get("status", "missing"),
                "candidate_status": cand.get("status", "missing"),
                "metrics": deltas,
            }
        )
    return {"schema_version": 1, "items": items}


def write_markdown(path: pathlib.Path, comparison: dict[str, Any]) -> None:
    lines = [
        "# phoneME real-game A/B",
        "",
        "Negative deltas are improvements for CPU/frame-time/allocation/GC metrics.",
        "OS/package thermal state is intentionally not fabricated by this host harness.",
        "",
    ]
    for item in comparison["items"]:
        lines.extend(
            [
                f"## {item['id']}",
                "",
                f"Baseline: `{item['baseline_status']}`; candidate: `{item['candidate_status']}`",
                "",
                "| Metric | Baseline | Candidate | Delta |",
                "| --- | ---: | ---: | ---: |",
            ]
        )
        for key, metric in item["metrics"].items():
            delta = metric["delta_percent"]
            delta_text = "n/a" if delta is None else f"{delta:+.2f}%"
            lines.append(
                f"| `{key}` | {metric['baseline']:.3f} | "
                f"{metric['candidate']:.3f} | {delta_text} |"
            )
        lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_corpus(
    tree: pathlib.Path,
    jar_root: pathlib.Path,
    manifest: pathlib.Path,
    output: pathlib.Path,
    report: pathlib.Path,
    env: dict[str, str],
) -> None:
    corpus_manifest = load_manifest(manifest)
    filters = [
        pathlib.Path(str(item["jar"])).name
        for item in corpus_manifest["benchmarks"]
    ]
    command = [
        sys.executable,
        str(tree / "Core" / "Tools" / "test-jar-directory.py"),
        "--jar-dir",
        str(jar_root),
        "--output",
        str(output),
        "--report",
        str(report),
        "--mode",
        "smoke",
        "--jobs",
        "1",
        "--timeout-ms",
        "120000",
        "--observe-manifest",
        str(manifest),
        "--autoplay",
        "--width",
        "240",
        "--height",
        "320",
    ]
    for value in filters:
        command.extend(["--filter", value])
    # A single compatibility failure must not discard performance data from
    # every other pinned game. test-jar-directory always writes its JSON/report
    # before returning non-zero, so preserve that evidence and let the A/B
    # report expose the failed item's status explicitly.
    run(command, cwd=tree, env=env, check=False)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-ref", default="HEAD")
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-root", type=pathlib.Path)
    parser.add_argument("--jit", choices=("0", "1"), default="1")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.expanduser().resolve()
    manifest = load_manifest(manifest_path)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output_root = (
        args.output_root.expanduser().resolve()
        if args.output_root
        else CORE_ROOT / "build" / "real-game-ab" / stamp
    )
    output_root.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    release_flags = env.get("PHONEME_EXTRA_CXXFLAGS", "").strip()
    release_flags = f"{release_flags} -O3 -DNDEBUG".strip()
    env.update(
        {
            "PHONEME_ENABLE_VM_PROFILING": "1",
            "PHONEME_ENABLE_DECODED_EXECUTION": "1",
            "PHONEME_DUMP_PERF": "1",
            "PHONEME_HARNESS_JIT": args.jit,
            # Performance A/B must resemble the production Core build. The
            # generic compatibility harness intentionally defaults to an
            # unoptimized compile for diagnostics, which badly distorts CPU,
            # frame-time and interpreter-vs-JIT comparisons.
            "PHONEME_EXTRA_CXXFLAGS": release_flags,
        }
    )

    baseline_tree = pathlib.Path(tempfile.mkdtemp(prefix="phoneme-baseline-"))
    shutil.rmtree(baseline_tree)
    baseline_added = False
    try:
        run(
            ["git", "worktree", "add", "--detach", str(baseline_tree), args.baseline_ref],
            cwd=REPO_ROOT,
            env=env,
        )
        baseline_added = True
        baseline_output = output_root / "baseline"
        candidate_output = output_root / "candidate"
        baseline_report = output_root / "baseline.md"
        candidate_report = output_root / "candidate.md"

        run_corpus(
            baseline_tree,
            REPO_ROOT / "jar_test",
            manifest_path,
            baseline_output,
            baseline_report,
            env,
        )
        run_corpus(
            REPO_ROOT,
            REPO_ROOT / "jar_test",
            manifest_path,
            candidate_output,
            candidate_report,
            env,
        )

        baseline = extract_report(baseline_report)
        candidate = extract_report(candidate_report)
        result = compare(manifest, baseline, candidate)
        result.update(
            {
                "baseline_ref": args.baseline_ref,
                "candidate_tree": str(REPO_ROOT),
                "jit_enabled": args.jit == "1",
                "manifest": str(manifest_path),
            }
        )
        json_path = output_root / "comparison.json"
        md_path = output_root / "comparison.md"
        json_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        write_markdown(md_path, result)
        print(f"A/B comparison JSON: {json_path}")
        print(f"A/B comparison Markdown: {md_path}")
    finally:
        if baseline_added:
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(baseline_tree)],
                cwd=REPO_ROOT,
                env=env,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        elif baseline_tree.exists():
            shutil.rmtree(baseline_tree, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
