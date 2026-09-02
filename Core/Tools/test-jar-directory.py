#!/usr/bin/env python3
"""Batch static/smoke test J2ME JARs and produce one fix-oriented report.

The tool deliberately runs every MIDlet in a separate CompatibilityHarness process
and runtime home so a crash, timeout, or corrupt suite cannot poison later tests.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import datetime as dt
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import textwrap
import threading
import time
import zipfile
from typing import Any, Iterable, Mapping, Sequence

SCRIPT_PATH = pathlib.Path(__file__).resolve()
CORE_ROOT = SCRIPT_PATH.parent.parent
PROJECT_ROOT = CORE_ROOT.parent
DEFAULT_JAR_DIR = PROJECT_ROOT / "jar_test"
DEFAULT_TIMEOUT_MS = 2_500
DEFAULT_WIDTH = 240
DEFAULT_HEIGHT = 320
MAX_ERROR_TEXT = 700

FAILURE_KIND_ALIASES = {
    "native_crash": "native crash",
    "missing_class": "missing class",
    "missing_method": "missing method",
    "missing_native": "missing native",
    "verifier": "verifier/ClassFormat/StackMap failure",
    "install": "install/manifest/suite failure",
    "thread": "thread/scheduler/monitor failure",
    "lcdui": "LCDUI callback/bridge failure",
    "graphics": "graphics/framebuffer/render failure",
    "rms": "RMS/persistence failure",
    "performance": "performance/memory failure",
    "uncaught_exception": "uncaught Java exception",
    "network": "network failure",
    "media": "media failure",
    "timeout": "timeout",
}

PRIORITY_ORDER = {
    "native crash": 0,
    "asan/ubsan/native crash": 0,
    "timeout": 1,
    "verifier/ClassFormat/StackMap failure": 2,
    "bytecode runtime semantics": 3,
    "missing class": 4,
    "missing method": 5,
    "missing native": 6,
    "missing field": 7,
    "uncaught Java exception": 8,
    "thread/scheduler/monitor failure": 9,
    "LCDUI callback/bridge failure": 10,
    "graphics/framebuffer/render failure": 11,
    "RMS/persistence failure": 12,
    "network failure": 13,
    "media failure": 14,
    "performance/memory failure": 15,
    "install/manifest/suite failure": 16,
    "metadata": 17,
    "static scan": 18,
    "runtime failure": 19,
}

API_PREFIXES = (
    "java/",
    "javax/",
    "com/nokia/",
    "com/siemens/",
    "com/samsung/",
    "com/motorola/",
    "com/sony",
    "com/mascotcapsule/",
    "com/vodafone/",
    "com/sprintpcs/",
    "com/sun/midp/",
    "org/microemu/",
    "org/xml/sax/",
)

_PROGRESS_LOCK = threading.Lock()
_PROGRESS_DONE = 0
_PROGRESS_TOTAL = 0
_PROGRESS_STARTED = 0.0


@dataclasses.dataclass(frozen=True)
class MidletEntry:
    name: str
    class_name: str
    source: str


@dataclasses.dataclass(frozen=True)
class JarTarget:
    item_id: str
    jar_path: pathlib.Path
    relative_path: str
    midlet_name: str
    main_class: str
    main_source: str
    display_width: int | None = None
    display_height: int | None = None
    orientation: str = ""


@dataclasses.dataclass(frozen=True)
class DiscoveryIssue:
    jar_path: pathlib.Path
    relative_path: str
    kind: str
    detail: str


@dataclasses.dataclass
class TargetResult:
    target: JarTarget
    status: str
    duration_ms: int = 0
    static_error: str = ""
    launch_error: str = ""
    failures: list[dict[str, str]] = dataclasses.field(default_factory=list)
    observed: dict[str, Any] = dataclasses.field(default_factory=dict)
    referenced_classes: dict[str, int] = dataclasses.field(default_factory=dict)
    referenced_methods: dict[str, int] = dataclasses.field(default_factory=dict)
    referenced_fields: dict[str, int] = dataclasses.field(default_factory=dict)
    run_dir: str = ""
    stdout_log: str = ""
    stderr_log: str = ""
    result_json: str = ""


def load_compat_module() -> Any:
    analyzer_path = CORE_ROOT / "Compatibility" / "analyze-failures.py"
    spec = importlib.util.spec_from_file_location("phoneme_compat_analyzer", analyzer_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load compatibility analyzer: {analyzer_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def utc_timestamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def json_write(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def markdown_escape(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ").replace("\r", " ")


def compact_text(value: str, limit: int = MAX_ERROR_TEXT) -> str:
    compact = re.sub(r"\s+", " ", value).strip()
    if len(compact) <= limit:
        return compact
    return compact[: max(0, limit - 3)] + "..."


def stable_id(relative_path: str, main_class: str) -> str:
    stem = pathlib.Path(relative_path).stem.casefold()
    slug = re.sub(r"[^a-z0-9]+", "-", stem).strip("-") or "jar"
    digest = hashlib.sha256(f"{relative_path}\0{main_class}".encode("utf-8")).hexdigest()[:10]
    return f"{slug[:52]}-{digest}"


def decode_manifest(raw: bytes) -> str:
    for encoding in ("utf-8-sig", "cp1252", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def parse_manifest_text(text: str) -> dict[str, str]:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    logical_lines: list[str] = []
    for line in normalized.split("\n"):
        if line.startswith(" ") and logical_lines:
            logical_lines[-1] += line[1:]
        else:
            logical_lines.append(line)

    attributes: dict[str, str] = {}
    for line in logical_lines:
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        if not key:
            continue
        attributes[key.casefold()] = value.lstrip()
    return attributes


def midlets_from_attributes(attributes: Mapping[str, str], source: str) -> list[MidletEntry]:
    indexed: list[tuple[int, MidletEntry]] = []
    for key, raw_value in attributes.items():
        match = re.fullmatch(r"midlet-(\d+)", key.casefold())
        if not match:
            continue
        parts = [part.strip() for part in raw_value.split(",")]
        if not parts:
            continue
        class_name = parts[-1].replace("/", ".").strip()
        if not class_name:
            continue
        display_name = parts[0] if len(parts) >= 2 and parts[0] else class_name
        indexed.append((int(match.group(1)), MidletEntry(display_name, class_name, source)))
    indexed.sort(key=lambda item: item[0])
    if indexed:
        return [entry for _, entry in indexed]

    for key in ("midlet-class", "main-class"):
        class_name = attributes.get(key, "").replace("/", ".").strip()
        if class_name:
            return [MidletEntry(class_name.rsplit(".", 1)[-1], class_name, source)]
    return []


def read_zip_manifest(archive: zipfile.ZipFile) -> dict[str, str]:
    manifest_name = next(
        (name for name in archive.namelist() if name.casefold() == "meta-inf/manifest.mf"),
        "",
    )
    if not manifest_name:
        return {}
    return parse_manifest_text(decode_manifest(archive.read(manifest_name)))


def read_jad_attributes(jar_path: pathlib.Path) -> dict[str, str]:
    candidates = [jar_path.with_suffix(".jad"), jar_path.with_suffix(".JAD")]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            return parse_manifest_text(decode_manifest(candidate.read_bytes()))
        except OSError:
            continue
    return {}


def read_jad_midlets(jar_path: pathlib.Path) -> list[MidletEntry]:
    attributes = read_jad_attributes(jar_path)
    if not attributes:
        return []
    return midlets_from_attributes(
        attributes,
        f"JAD:{jar_path.with_suffix('.jad').name}",
    )


def inferred_display(
    attributes: Mapping[str, str],
) -> tuple[int | None, int | None, str]:
    orientation = ""
    for key in (
        "nokia-midlet-app-orientation",
        "screenmode",
        "midlet-screen-mode",
    ):
        value = attributes.get(key, "").strip().casefold()
        if "portrait" in value:
            orientation = "portrait"
            break
        if "landscape" in value:
            orientation = "landscape"
            break

    width: int | None = None
    height: int | None = None
    for key in (
        "target-display-size",
        "nokia-midlet-target-display-size",
        "nokia-midlet-original-display-size",
        "midlet-original-display-size",
    ):
        value = attributes.get(key, "")
        match = re.search(r"(\d+)\s*[,xX*]\s*(\d+)", value)
        if match:
            width = int(match.group(1))
            height = int(match.group(2))
            break
    if width is None or height is None:
        width_text = attributes.get("lge-midlet-targetlcd-width", "")
        height_text = attributes.get("lge-midlet-targetlcd-height", "")
        if width_text.isdigit() and height_text.isdigit():
            width = int(width_text)
            height = int(height_text)

    if width is not None and height is not None:
        if orientation == "portrait" and width > height:
            width, height = height, width
        elif orientation == "landscape" and width < height:
            width, height = height, width
    return width, height, orientation


def read_u1(data: memoryview, offset: int) -> tuple[int, int]:
    if offset + 1 > len(data):
        raise ValueError("truncated class file")
    return int(data[offset]), offset + 1


def read_u2(data: memoryview, offset: int) -> tuple[int, int]:
    if offset + 2 > len(data):
        raise ValueError("truncated class file")
    return struct.unpack_from(">H", data, offset)[0], offset + 2


def read_u4(data: memoryview, offset: int) -> tuple[int, int]:
    if offset + 4 > len(data):
        raise ValueError("truncated class file")
    return struct.unpack_from(">I", data, offset)[0], offset + 4


def class_and_super(raw: bytes) -> tuple[str, str]:
    data = memoryview(raw)
    if len(data) < 10 or data[:4].tobytes() != b"\xca\xfe\xba\xbe":
        raise ValueError("invalid class-file magic")
    offset = 8
    cp_count, offset = read_u2(data, offset)
    cp: list[Any] = [None] * cp_count
    index = 1
    while index < cp_count:
        tag, offset = read_u1(data, offset)
        if tag == 1:
            length, offset = read_u2(data, offset)
            if offset + length > len(data):
                raise ValueError("truncated UTF-8 constant")
            cp[index] = (tag, data[offset : offset + length].tobytes().decode("utf-8", "replace"))
            offset += length
        elif tag in (3, 4):
            _, offset = read_u4(data, offset)
        elif tag in (5, 6):
            _, offset = read_u4(data, offset)
            _, offset = read_u4(data, offset)
            index += 1
        elif tag in (7, 8, 16, 19, 20):
            value, offset = read_u2(data, offset)
            cp[index] = (tag, value)
        elif tag in (9, 10, 11, 12, 17, 18):
            first, offset = read_u2(data, offset)
            second, offset = read_u2(data, offset)
            cp[index] = (tag, first, second)
        elif tag == 15:
            kind, offset = read_u1(data, offset)
            reference, offset = read_u2(data, offset)
            cp[index] = (tag, kind, reference)
        else:
            raise ValueError(f"unsupported constant-pool tag {tag}")
        index += 1

    _, offset = read_u2(data, offset)  # access flags
    this_class, offset = read_u2(data, offset)
    super_class, offset = read_u2(data, offset)

    def utf8(cp_index: int) -> str:
        if cp_index <= 0 or cp_index >= len(cp):
            raise ValueError("invalid UTF-8 index")
        entry = cp[cp_index]
        if not entry or entry[0] != 1:
            raise ValueError("invalid UTF-8 entry")
        return str(entry[1])

    def class_name(cp_index: int) -> str:
        if cp_index == 0:
            return ""
        if cp_index <= 0 or cp_index >= len(cp):
            raise ValueError("invalid class index")
        entry = cp[cp_index]
        if not entry or entry[0] != 7:
            raise ValueError("invalid class entry")
        return utf8(int(entry[1]))

    return class_name(this_class), class_name(super_class)


def detect_midlet_classes(archive: zipfile.ZipFile) -> list[MidletEntry]:
    hierarchy: dict[str, str] = {}
    for name in archive.namelist():
        if not name.endswith(".class") or name.startswith("META-INF/versions/"):
            continue
        try:
            defined, parent = class_and_super(archive.read(name))
        except (KeyError, ValueError):
            continue
        hierarchy[defined] = parent

    base = "javax/microedition/midlet/MIDlet"

    def derives_from_midlet(class_name: str) -> bool:
        seen: set[str] = set()
        current = class_name
        while current and current not in seen:
            seen.add(current)
            parent = hierarchy.get(current, "")
            if parent == base:
                return True
            current = parent
        return False

    candidates = sorted(name for name in hierarchy if derives_from_midlet(name))
    return [
        MidletEntry(name.rsplit("/", 1)[-1], name.replace("/", "."), "class-hierarchy-fallback")
        for name in candidates
    ]


def discover_jar(
    jar_path: pathlib.Path,
    jar_root: pathlib.Path,
    all_midlets: bool,
) -> tuple[list[JarTarget], list[DiscoveryIssue]]:
    try:
        relative_path = str(jar_path.relative_to(jar_root))
    except ValueError:
        relative_path = str(jar_path)

    entries: list[MidletEntry] = []
    display_width: int | None = None
    display_height: int | None = None
    orientation = ""
    try:
        with zipfile.ZipFile(jar_path) as archive:
            manifest_attributes = read_zip_manifest(archive)
            jad_attributes = read_jad_attributes(jar_path)
            combined_attributes = dict(manifest_attributes)
            combined_attributes.update(jad_attributes)
            display_width, display_height, orientation = inferred_display(
                combined_attributes
            )
            entries = midlets_from_attributes(
                manifest_attributes, "JAR manifest"
            )
            if not entries and jad_attributes:
                entries = midlets_from_attributes(
                    jad_attributes,
                    f"JAD:{jar_path.with_suffix('.jad').name}",
                )
            if not entries:
                entries = detect_midlet_classes(archive)
    except (OSError, zipfile.BadZipFile, RuntimeError, ValueError) as exc:
        return [], [DiscoveryIssue(jar_path, relative_path, "metadata", compact_text(str(exc)))]

    if not entries:
        return [], [
            DiscoveryIssue(
                jar_path,
                relative_path,
                "metadata",
                "No MIDlet-N/Main-Class entry and no class deriving from MIDlet was found",
            )
        ]

    selected = entries if all_midlets else entries[:1]
    targets = [
        JarTarget(
            item_id=stable_id(relative_path, entry.class_name),
            jar_path=jar_path,
            relative_path=relative_path,
            midlet_name=entry.name,
            main_class=entry.class_name,
            main_source=entry.source,
            display_width=display_width,
            display_height=display_height,
            orientation=orientation,
        )
        for entry in selected
    ]
    return targets, []


def matches_filters(path: pathlib.Path, filters: Sequence[str]) -> bool:
    if not filters:
        return True
    haystack = str(path).casefold()
    return any(token.casefold() in haystack for token in filters)


def discover_targets(
    jar_root: pathlib.Path,
    filters: Sequence[str],
    limit: int,
    all_midlets: bool,
) -> tuple[list[JarTarget], list[DiscoveryIssue], int]:
    jars = sorted(
        (
            path
            for path in jar_root.rglob("*")
            if path.is_file() and path.suffix.casefold() == ".jar" and matches_filters(path, filters)
        ),
        key=lambda path: str(path).casefold(),
    )
    discovered_count = len(jars)
    if limit > 0:
        jars = jars[:limit]

    targets: list[JarTarget] = []
    issues: list[DiscoveryIssue] = []
    for jar_path in jars:
        jar_targets, jar_issues = discover_jar(jar_path, jar_root, all_midlets)
        targets.extend(jar_targets)
        issues.extend(jar_issues)
    return targets, issues, discovered_count


def compiler_command() -> tuple[list[str], list[str]]:
    configured = os.environ.get("CXX", "").strip()
    if configured:
        return shlex.split(configured), []
    xcrun = shutil.which("xcrun")
    if xcrun:
        compiler = subprocess.check_output(
            [xcrun, "--sdk", "macosx", "--find", "clang++"], text=True
        ).strip()
        sdk = subprocess.check_output(
            [xcrun, "--sdk", "macosx", "--show-sdk-path"], text=True
        ).strip()
        return [compiler], ["-isysroot", sdk]
    compiler = shutil.which("clang++") or shutil.which("c++")
    if not compiler:
        raise RuntimeError("a C++23 compiler is required")
    return [compiler], []


def objective_c_compiler_command() -> tuple[list[str], list[str]]:
    configured = os.environ.get("CC", "").strip()
    if configured:
        return shlex.split(configured), []
    xcrun = shutil.which("xcrun")
    if xcrun:
        compiler = subprocess.check_output(
            [xcrun, "--sdk", "macosx", "--find", "clang"], text=True
        ).strip()
        sdk = subprocess.check_output(
            [xcrun, "--sdk", "macosx", "--show-sdk-path"], text=True
        ).strip()
        return [compiler], ["-isysroot", sdk]
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("an Objective-C compiler is required on macOS")
    return [compiler], []


def compile_time_feature(name: str) -> str:
    value = os.environ.get(name, "0").strip().casefold()
    return "1" if value in {"1", "true", "yes", "on"} else "0"


def extra_build_flags(name: str) -> list[str]:
    value = os.environ.get(name, "").strip()
    return shlex.split(value) if value else []


def build_harness(output_root: pathlib.Path, sanitize: bool) -> tuple[pathlib.Path | None, str]:
    build_root = output_root / "harness-build"
    build_root.mkdir(parents=True, exist_ok=True)
    binary = build_root / "CompatibilityHarness"
    stdout_path = build_root / "build.stdout.log"
    stderr_path = build_root / "build.stderr.log"

    try:
        compiler, sdk_flags = compiler_command()
        objective_c_compiler: list[str] = []
        objective_c_sdk_flags: list[str] = []
        if sys.platform == "darwin":
            objective_c_compiler, objective_c_sdk_flags = (
                objective_c_compiler_command()
            )
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        return None, str(exc)

    bridge_object: pathlib.Path | None = None
    if sys.platform == "darwin":
        bridge_source = PROJECT_ROOT / "phoneME" / "Core" / "PhoneMEHTTPSBridge.m"
        bridge_object = build_root / "PhoneMEHTTPSBridge.o"
        bridge_command = [
            *objective_c_compiler,
            *objective_c_sdk_flags,
            "-fobjc-arc",
            "-fmodules",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Wconversion",
            "-Wsign-conversion",
            "-Wshadow",
            "-Werror=return-type",
            "-Werror=unguarded-availability-new",
        ]
        if sanitize:
            bridge_command.extend(
                ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            )
        bridge_command.extend(
            ["-c", str(bridge_source), "-o", str(bridge_object)]
        )
        try:
            with stdout_path.open("wb") as stdout_stream, stderr_path.open(
                "wb"
            ) as stderr_stream:
                completed = subprocess.run(
                    bridge_command,
                    stdout=stdout_stream,
                    stderr=stderr_stream,
                    check=False,
                    timeout=120,
                )
        except subprocess.TimeoutExpired:
            return None, "phoneME HTTP bridge build timed out after 120 seconds"
        if completed.returncode != 0 or not bridge_object.is_file():
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            return None, (
                "phoneME HTTP bridge build failed: "
                + compact_text(stderr, 2_000)
            )

    prebuilt_core_value = os.environ.get("PHONEME_PREBUILT_CORE_LIB", "").strip()
    prebuilt_core = pathlib.Path(prebuilt_core_value) if prebuilt_core_value else None
    if prebuilt_core is not None and not prebuilt_core.is_file():
        return None, f"prebuilt phoneME core library does not exist: {prebuilt_core}"
    sources = [] if prebuilt_core is not None else sorted(
        path
        for path in (CORE_ROOT / "src").rglob("*.cpp")
        if path != CORE_ROOT / "src" / "api" / "CAPI.cpp"
    )
    command = [
        *compiler,
        "-std=c++23",
        *sdk_flags,
        *extra_build_flags("PHONEME_EXTRA_CXXFLAGS"),
        f"-DPHONEME_ENABLE_VM_PROFILING={compile_time_feature('PHONEME_ENABLE_VM_PROFILING')}",
        f"-DPHONEME_ENABLE_DECODED_EXECUTION={compile_time_feature('PHONEME_ENABLE_DECODED_EXECUTION')}",
        f"-I{CORE_ROOT / 'include'}",
        "-fno-exceptions",
        "-fno-rtti",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wconversion",
        "-Wsign-conversion",
        "-Wshadow",
        "-Werror=return-type",
    ]
    if sanitize:
        command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
    command.extend(
        [
            str(CORE_ROOT / "Tests" / "Compatibility" / "CompatibilityHarness.cpp"),
            *(str(path) for path in sources),
            *([str(prebuilt_core)] if prebuilt_core is not None else []),
            *([str(bridge_object)] if bridge_object is not None else []),
            "-lz",
        ]
    )
    command.extend(extra_build_flags("PHONEME_EXTRA_LDFLAGS"))
    if sys.platform == "darwin":
        command.extend(
            [
                "-framework",
                "CoreText",
                "-framework",
                "CoreGraphics",
                "-framework",
                "CoreFoundation",
                "-framework",
                "ImageIO",
                "-framework",
                "Foundation",
                "-framework",
                "Security",
            ]
        )
    command.extend(["-o", str(binary)])

    command_path = build_root / "build-command.txt"
    command_lines = []
    if sys.platform == "darwin":
        command_lines.append(shlex.join(bridge_command))
    command_lines.append(shlex.join(command))
    command_path.write_text("\n".join(command_lines) + "\n", encoding="utf-8")
    started = time.monotonic()
    try:
        with stdout_path.open("wb") as stdout_stream, stderr_path.open("wb") as stderr_stream:
            completed = subprocess.run(
                command,
                stdout=stdout_stream,
                stderr=stderr_stream,
                check=False,
                timeout=300,
            )
    except subprocess.TimeoutExpired:
        return None, "CompatibilityHarness build timed out after 300 seconds"
    duration = int((time.monotonic() - started) * 1000)
    if completed.returncode != 0 or not binary.is_file():
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        return None, f"CompatibilityHarness build failed ({duration} ms): {compact_text(stderr, 2_000)}"
    return binary, ""


def add_failure(failures: list[dict[str, str]], kind: str, detail: str) -> None:
    kind = FAILURE_KIND_ALIASES.get(kind, kind)
    normalized = compact_text(detail)
    if not normalized:
        return
    key = (kind, normalized)
    if any((item.get("kind"), item.get("detail")) == key for item in failures):
        return
    failures.append({"kind": kind, "detail": normalized})


def classify_status(
    execution: Any,
    observed: Mapping[str, Any],
    failures: Sequence[Mapping[str, str]],
    require_visual: bool,
) -> str:
    if execution is None:
        return "STATIC_ONLY"
    if execution.timed_out:
        return "TIMEOUT"
    if execution.launch_error:
        return "LAUNCH_ERROR"
    if execution.return_code is not None and execution.return_code < 0:
        return "NATIVE_CRASH"
    if execution.return_code not in (0, None):
        return "FAILED"
    if observed.get("install") != "success":
        return "FAILED"
    if observed.get("app_state") not in ("active", "paused", "destroyed"):
        return "FAILED"
    severe = {
        "native crash",
        "asan/ubsan/native crash",
        "verifier/ClassFormat/StackMap failure",
        "bytecode runtime semantics",
        "missing class",
        "missing method",
        "missing native",
        "missing field",
        "uncaught Java exception",
        "runtime failure",
    }
    if any(str(item.get("kind")) in severe for item in failures):
        return "FAILED"
    if observed.get("stall_suspected") is True:
        return "STALLED"
    milestones = set(str(value) for value in observed.get("milestones", []))
    visual = bool(observed.get("visual_output_observed")) or (
        int(observed.get("nonzero_frame_bytes", 0) or 0) > 0
        or "lcdui-screen-shown" in milestones
    )
    if require_visual and not visual:
        return "NO_VISUAL"
    if int(observed.get("frames_produced", 0) or 0) > 0:
        return "STARTED_FRAME"
    if any(value.startswith(("canvas-", "lcdui-", "ui-")) for value in milestones):
        return "STARTED_UI"
    return "STARTED"


def inspect_target(
    compat: Any,
    target: JarTarget,
    output_root: pathlib.Path,
    runner: pathlib.Path | None,
    mode: str,
    timeout_ms: int,
    observe_ms: int,
    autoplay: bool,
    input_start_delay_ms: int,
    input_interval_ms: int,
    stall_ms: int,
    heartbeat_ms: int,
    require_visual: bool,
    skip_teardown: bool,
    width: int,
    height: int,
) -> TargetResult:
    started = time.monotonic()
    static_error = ""
    referenced_classes: dict[str, int] = {}
    referenced_methods: dict[str, int] = {}
    referenced_fields: dict[str, int] = {}

    if mode in ("static", "both"):
        try:
            references = compat.scan_jar(target.jar_path, target.main_class)
            referenced_classes = dict(references.class_references)
            referenced_methods = dict(references.method_references)
            referenced_fields = dict(references.field_references)
        except Exception as exc:  # analyzer exposes CorpusError, but keep batch alive for all failures
            static_error = compact_text(str(exc))

    execution = None
    failures: list[dict[str, str]] = []
    run_dir = output_root / "items" / target.item_id
    run_width = target.display_width or width
    run_height = target.display_height or height
    if target.display_width is None or target.display_height is None:
        if target.orientation == "portrait" and run_width > run_height:
            run_width, run_height = run_height, run_width
        elif target.orientation == "landscape" and run_width < run_height:
            run_width, run_height = run_height, run_width
    if static_error:
        add_failure(failures, "static scan", static_error)

    if mode in ("smoke", "both") and runner is not None:
        item = {
            "id": target.item_id,
            "main_class": target.main_class,
            "input_sequence": [
                {"action": "launch"},
                {
                    "action": "autoplay" if autoplay else "observe",
                    "observe_ms": observe_ms,
                    "input_start_delay_ms": input_start_delay_ms,
                    "input_interval_ms": input_interval_ms,
                    "stall_ms": stall_ms,
                },
            ],
            "expected": {
                "install": "success",
                "exit": "normal",
                "app_state": "active",
                "min_frames": 0,
                "milestones": [],
                "network_actions": [],
                "media_actions": [],
                "frame_hashes": [],
                "max_startup_ms": timeout_ms,
            },
        }
        runner_tokens = [str(runner)]
        if observe_ms > 0:
            runner_tokens.extend(["--observe-ms", str(observe_ms)])
        runner_tokens.extend(
            [
                "--autoplay",
                "1" if autoplay else "0",
                "--input-start-delay-ms",
                str(input_start_delay_ms),
                "--input-interval-ms",
                str(input_interval_ms),
                "--stall-ms",
                str(stall_ms),
                "--heartbeat-ms",
                str(heartbeat_ms),
                "--skip-teardown",
                "1" if skip_teardown else "0",
            ]
        )
        # The external process must cover both the startup budget and the
        # requested observation window. Previously the process timeout equaled
        # only max_startup_ms, so a MIDlet that started near the limit and then
        # rendered successfully during observation was mislabeled TIMEOUT.
        process_timeout_ms = timeout_ms + max(0, observe_ms) + 5_000
        execution = compat.execute_runner(
            runner_tokens,
            item,
            target.jar_path,
            run_dir,
            process_timeout_ms,
            {"width": run_width, "height": run_height},
        )
        logs = compat.combined_logs(execution)
        classified = compat.classify_failures(logs)
        if execution.timed_out:
            classified = [failure for failure in classified if failure.get("kind") != "timeout"]
        for failure in classified:
            add_failure(failures, str(failure.get("kind", "runtime failure")), str(failure.get("detail", "")))
        if execution.timed_out:
            add_failure(failures, "timeout", f"runner exceeded {timeout_ms} ms")
        if execution.launch_error:
            add_failure(failures, "runtime failure", execution.launch_error)
        if execution.return_code is not None and execution.return_code < 0:
            signal_number = -execution.return_code
            try:
                signal_name = signal.Signals(signal_number).name
            except ValueError:
                signal_name = str(signal_number)
            add_failure(failures, "native crash", f"process terminated by {signal_name}")
        java_exception = execution.result.get("java_exception_class") if execution.result else ""
        java_message = execution.result.get("error_message") if execution.result else ""
        message_text = str(java_message) if isinstance(java_message, str) else ""
        lowered_message = message_text.casefold()
        if (
            "abstractmethoderror" in lowered_message
            or "nosuchmethoderror" in lowered_message
            or "method was not found in the class hierarchy" in lowered_message
        ):
            add_failure(failures, "missing method", message_text)
        if "field was not found" in lowered_message or "nosuchfielderror" in lowered_message:
            add_failure(failures, "missing field", message_text)
        if "class is neither built into core nor present in the application jar" in lowered_message:
            missing_class = message_text.rsplit(":", 1)[-1].strip()
            if missing_class.startswith("["):
                add_failure(failures, "bytecode runtime semantics", message_text)
            else:
                add_failure(failures, "missing class", missing_class or message_text)
        if "native method is not ported" in lowered_message or "native method was not found" in lowered_message:
            missing_native = message_text.split(":", 1)[-1].strip()
            add_failure(failures, "missing native", missing_native or message_text)
        if "jar entry contains an unsafe path" in lowered_message:
            add_failure(failures, "install/manifest/suite failure", message_text)
        if (
            "array load opcode does not match element type" in lowered_message
            or "array store opcode does not match element type" in lowered_message
            or "operand stack" in lowered_message
            or "local slot" in lowered_message
        ):
            add_failure(failures, "bytecode runtime semantics", message_text)

        embedded_exception = re.search(
            r"((?:java|javax|com)/[A-Za-z0-9_$/]+(?:Exception|Error))",
            message_text,
        )
        if isinstance(java_exception, str) and java_exception:
            failures[:] = [
                failure
                for failure in failures
                if not (
                    failure.get("kind") == "uncaught Java exception"
                    and failure.get("detail") == "uncaught Java exception"
                )
            ]
            add_failure(
                failures,
                "uncaught Java exception",
                f"{java_exception}: {message_text}" if message_text else java_exception,
            )
        elif embedded_exception is not None:
            exception_name = embedded_exception.group(1)
            already_explained = (
                exception_name.endswith(("AbstractMethodError", "NoSuchMethodError", "NoSuchFieldError"))
                or "native method is not ported" in lowered_message
                or "class is neither built into core" in lowered_message
            )
            failures[:] = [
                failure
                for failure in failures
                if not (
                    failure.get("kind") == "uncaught Java exception"
                    and failure.get("detail") == "uncaught Java exception"
                )
            ]
            if not already_explained:
                add_failure(
                    failures,
                    "uncaught Java exception",
                    f"{exception_name}: {message_text}",
                )
        elif execution.return_code not in (0, None) and not failures:
            add_failure(
                failures,
                "runtime failure",
                f"runner returned {execution.return_code}; {message_text or 'no structured error'}",
            )

    observed = compat.observed_data(execution)
    observed["test_width"] = run_width
    observed["test_height"] = run_height
    if target.orientation:
        observed["declared_orientation"] = target.orientation
    if execution is not None and execution.result:
        for key in (
            "frame_changes",
            "unique_frames",
            "nonzero_frame_bytes",
            "process_cpu_ms",
            "wall_time_ms",
            "cpu_utilization_pct",
            "cpu_ms_per_generated_frame",
            "frame_interval_p50_ms",
            "frame_interval_p95_ms",
            "frame_interval_p99_ms",
            "frame_hitches_over_50ms",
            "frame_interval_samples",
            "ui_event_count",
            "canvas_event_count",
            "lcdui_event_count",
            "screens_shown",
            "key_presses",
            "key_events_sent",
            "lcdui_actions_sent",
            "heartbeat_count",
            "last_progress_ms",
            "longest_idle_ms",
            "visual_output_observed",
            "stall_suspected",
            "final_frame_hash",
            "last_input_action",
            "input_actions",
            "frame_hashes",
        ):
            if key in execution.result:
                observed[key] = execution.result[key]

    milestones = set(str(value) for value in observed.get("milestones", []))
    if observed.get("stall_suspected") is True:
        add_failure(
            failures,
            "performance/memory failure",
            "no UI or framebuffer progress for "
            f"{int(observed.get('longest_idle_ms', 0) or 0)} ms after "
            f"{int(observed.get('key_presses', 0) or 0)} key presses and "
            f"{int(observed.get('lcdui_actions_sent', 0) or 0)} LCDUI actions",
        )
    if require_visual:
        visual = bool(observed.get("visual_output_observed")) or (
            int(observed.get("nonzero_frame_bytes", 0) or 0) > 0
            or "lcdui-screen-shown" in milestones
        )
        if not visual:
            frames = int(observed.get("frames_produced", 0) or 0)
            if frames > 0:
                detail = (
                    f"only blank framebuffer data after {observe_ms} ms; "
                    f"frames={frames}, unique={int(observed.get('unique_frames', 0) or 0)}"
                )
            else:
                detail = f"no Canvas frame or native LCDUI screen after {observe_ms} ms"
            add_failure(failures, "graphics/framebuffer/render failure", detail)

    # A first-run openRecordStore(name, false) is required by MIDP to throw
    # RecordStoreNotFoundException. Many games catch it, print it for
    # diagnostics, create the store, and continue normally. Do not turn that
    # handled console output into a corpus failure when the structured runner
    # reports no Java exception and the MIDlet remains observable.
    if execution is not None and execution.result:
        structured_exception = execution.result.get("java_exception_class")
        observable = observed.get("app_state") in ("active", "paused") and (
            int(observed.get("frames_produced", 0) or 0) > 0
            or any(
                str(value).startswith(("canvas-", "lcdui-", "ui-"))
                for value in observed.get("milestones", [])
            )
        )
        if not structured_exception and observable:
            failures[:] = [
                failure
                for failure in failures
                if not (
                    failure.get("kind") == "RMS/persistence failure"
                    and "RecordStoreNotFoundException" in
                        str(failure.get("detail", ""))
                )
            ]
    status = classify_status(execution, observed, failures, require_visual)
    duration_ms = int((time.monotonic() - started) * 1000)

    result = TargetResult(
        target=target,
        status=status,
        duration_ms=duration_ms,
        static_error=static_error,
        launch_error=execution.launch_error if execution is not None else "",
        failures=failures,
        observed=observed,
        referenced_classes=referenced_classes,
        referenced_methods=referenced_methods,
        referenced_fields=referenced_fields,
        run_dir=str(run_dir),
    )
    if execution is not None:
        result.stdout_log = str(execution.stdout_path)
        result.stderr_log = str(execution.stderr_path)
        result.result_json = str(execution.result_path)
    update_progress(result)
    return result


def update_progress(result: TargetResult) -> None:
    global _PROGRESS_DONE
    with _PROGRESS_LOCK:
        _PROGRESS_DONE += 1
        done = _PROGRESS_DONE
        total = _PROGRESS_TOTAL
        elapsed = max(0.001, time.monotonic() - _PROGRESS_STARTED)
        rate = done / elapsed
        if done == total or done <= 10 or done % 25 == 0:
            print(
                f"[{done:4d}/{total:4d}] {result.status:13s} "
                f"{result.target.relative_path} :: {result.target.main_class} "
                f"({rate:.1f}/s)",
                flush=True,
            )


def target_result_to_json(result: TargetResult) -> dict[str, Any]:
    return {
        "id": result.target.item_id,
        "jar": str(result.target.jar_path),
        "relative_path": result.target.relative_path,
        "midlet_name": result.target.midlet_name,
        "main_class": result.target.main_class,
        "main_source": result.target.main_source,
        "display_hint": {
            "width": result.target.display_width,
            "height": result.target.display_height,
            "orientation": result.target.orientation,
        },
        "status": result.status,
        "duration_ms": result.duration_ms,
        "static_error": result.static_error,
        "launch_error": result.launch_error,
        "failures": result.failures,
        "observed": result.observed,
        "reference_counts": {
            "classes": sum(result.referenced_classes.values()),
            "methods": sum(result.referenced_methods.values()),
            "fields": sum(result.referenced_fields.values()),
            "unique_classes": len(result.referenced_classes),
            "unique_methods": len(result.referenced_methods),
            "unique_fields": len(result.referenced_fields),
        },
        "artifacts": {
            "run_dir": result.run_dir,
            "stdout": result.stdout_log,
            "stderr": result.stderr_log,
            "runner_result": result.result_json,
        },
    }


def failure_signature(failure: Mapping[str, str]) -> tuple[str, str]:
    kind = str(failure.get("kind", "runtime failure"))
    detail = compact_text(str(failure.get("detail", "")), 300)
    return kind, detail


def aggregate_report(
    jar_root: pathlib.Path,
    output_root: pathlib.Path,
    mode: str,
    timeout_ms: int,
    observe_ms: int,
    observe_manifest: pathlib.Path | None,
    observe_overrides: Mapping[str, int],
    autoplay: bool,
    input_start_delay_ms: int,
    input_interval_ms: int,
    stall_ms: int,
    heartbeat_ms: int,
    require_visual: bool,
    skip_teardown: bool,
    jobs: int,
    smoke_limit: int,
    smoke_filters: Sequence[str],
    runner: pathlib.Path | None,
    discovered_count: int,
    targets: Sequence[JarTarget],
    discovery_issues: Sequence[DiscoveryIssue],
    results: Sequence[TargetResult],
    build_error: str,
    command: Sequence[str],
) -> dict[str, Any]:
    statuses = collections.Counter(result.status for result in results)
    smoke_tested = sum(1 for result in results if result.status != "STATIC_ONLY")
    static_scanned = sum(1 for result in results if not result.static_error)
    failure_groups: dict[tuple[str, str], list[TargetResult]] = collections.defaultdict(list)
    for result in results:
        for failure in result.failures:
            failure_groups[failure_signature(failure)].append(result)
    for issue in discovery_issues:
        pseudo = TargetResult(
            target=JarTarget(
                item_id=stable_id(issue.relative_path, "metadata"),
                jar_path=issue.jar_path,
                relative_path=issue.relative_path,
                midlet_name="",
                main_class="",
                main_source="",
            ),
            status="METADATA_ERROR",
            failures=[{"kind": issue.kind, "detail": issue.detail}],
        )
        failure_groups[(issue.kind, issue.detail)].append(pseudo)

    class_counts: collections.Counter[str] = collections.Counter()
    method_counts: collections.Counter[str] = collections.Counter()
    field_counts: collections.Counter[str] = collections.Counter()
    for result in results:
        class_counts.update(result.referenced_classes)
        method_counts.update(result.referenced_methods)
        field_counts.update(result.referenced_fields)

    api_class_counts = {
        name: count
        for name, count in class_counts.most_common()
        if name.startswith(API_PREFIXES)
    }
    api_method_counts = {
        name: count
        for name, count in method_counts.most_common()
        if name.startswith(API_PREFIXES)
    }

    sorted_groups = sorted(
        failure_groups.items(),
        key=lambda item: (
            PRIORITY_ORDER.get(item[0][0], 99),
            -len({result.target.relative_path for result in item[1]}),
            item[0][1].casefold(),
        ),
    )

    return {
        "schema_version": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "command": list(command),
        "configuration": {
            "jar_root": str(jar_root),
            "output_root": str(output_root),
            "mode": mode,
            "timeout_ms": timeout_ms,
            "observe_ms": observe_ms,
            "observe_manifest": (
                str(observe_manifest) if observe_manifest is not None else ""
            ),
            "observe_overrides": dict(sorted(observe_overrides.items())),
            "autoplay": autoplay,
            "input_start_delay_ms": input_start_delay_ms,
            "input_interval_ms": input_interval_ms,
            "stall_ms": stall_ms,
            "heartbeat_ms": heartbeat_ms,
            "require_visual": require_visual,
            "skip_teardown": skip_teardown,
            "jobs": jobs,
            "smoke_limit": smoke_limit,
            "smoke_filters": list(smoke_filters),
            "runner": str(runner) if runner is not None else "",
            "runner_sha256": sha256_file(runner) if runner is not None and runner.is_file() else "",
        },
        "summary": {
            "jar_files_matching_filter": discovered_count,
            "targets_selected": len(targets),
            "metadata_errors": len(discovery_issues),
            "tested_targets": len(results),
            "static_scanned_without_parser_error": static_scanned,
            "smoke_tested": smoke_tested,
            "statuses": dict(sorted(statuses.items())),
            "unique_failure_signatures": len(failure_groups),
            "build_error": build_error,
        },
        "failure_groups": [
            {
                "kind": kind,
                "detail": detail,
                "affected_count": len({result.target.relative_path for result in affected}),
                "affected": sorted({result.target.relative_path for result in affected}),
            }
            for (kind, detail), affected in sorted_groups
        ],
        "top_referenced_api_classes": list(api_class_counts.items())[:200],
        "top_referenced_api_methods": list(api_method_counts.items())[:200],
        "top_referenced_fields": list(field_counts.most_common(100)),
        "discovery_issues": [dataclasses.asdict(issue) | {"jar_path": str(issue.jar_path)} for issue in discovery_issues],
        "items": [target_result_to_json(result) for result in results],
    }


def write_markdown_report(report: Mapping[str, Any], path: pathlib.Path) -> None:
    summary = report["summary"]
    config = report["configuration"]
    lines: list[str] = [
        "# phoneME C++ — JAR Test Fix Report",
        "",
        f"Generated: `{report['generated_at']}`  ",
        f"JAR root: `{config['jar_root']}`  ",
        f"Mode: `{config['mode']}`, timeout: `{config['timeout_ms']} ms`, observe: `{config['observe_ms']} ms`, jobs: `{config['jobs']}`, "
        f"autoplay: `{'on' if config['autoplay'] else 'off'}`, require visual: `{'yes' if config['require_visual'] else 'no'}`, "
        f"stall threshold: `{config['stall_ms']} ms`, smoke limit: `{config['smoke_limit'] or 'all'}`  ",
        f"Runner: `{config['runner'] or 'none'}`  ",
        f"Runner SHA-256: `{config['runner_sha256'] or 'n/a'}`",
        "",
        "## Summary",
        "",
        "| Metric | Count |",
        "| --- | ---: |",
        f"| Matching JAR files | {summary['jar_files_matching_filter']} |",
        f"| MIDlet targets selected | {summary['targets_selected']} |",
        f"| Metadata/discovery errors | {summary['metadata_errors']} |",
        f"| Targets included in report | {summary['tested_targets']} |",
        f"| Static scans without parser error | {summary['static_scanned_without_parser_error']} |",
        f"| Smoke-launched targets | {summary['smoke_tested']} |",
        f"| Unique failure signatures | {summary['unique_failure_signatures']} |",
    ]
    for status, count in summary["statuses"].items():
        lines.append(f"| `{markdown_escape(status)}` | {count} |")

    if summary.get("build_error"):
        lines.extend(
            [
                "",
                "## Harness build blocker",
                "",
                f"> {markdown_escape(str(summary['build_error']))}",
            ]
        )

    lines.extend(
        [
            "",
            "## Priority fix queue",
            "",
            "The same root cause is grouped once even when it breaks many games.",
            "",
            "| Priority | Kind | Affected JARs | Error signature | Samples |",
            "| ---: | --- | ---: | --- | --- |",
        ]
    )
    for index, group in enumerate(report["failure_groups"], start=1):
        samples = ", ".join(group["affected"][:5])
        lines.append(
            "| {priority} | `{kind}` | {count} | {detail} | {samples} |".format(
                priority=index,
                kind=markdown_escape(str(group["kind"])),
                count=group["affected_count"],
                detail=markdown_escape(str(group["detail"])),
                samples=markdown_escape(samples),
            )
        )

    lines.extend(
        [
            "",
            "## Most referenced J2ME/vendor API classes",
            "",
            "This is static demand from reachable classes, not a claim that every API is missing.",
            "",
            "| Class | References |",
            "| --- | ---: |",
        ]
    )
    for name, count in report["top_referenced_api_classes"][:100]:
        lines.append(f"| `{markdown_escape(str(name))}` | {count} |")

    lines.extend(
        [
            "",
            "## Most referenced J2ME/vendor methods",
            "",
            "| Method | References |",
            "| --- | ---: |",
        ]
    )
    for name, count in report["top_referenced_api_methods"][:100]:
        lines.append(f"| `{markdown_escape(str(name))}` | {count} |")

    discovery = report["discovery_issues"]
    if discovery:
        lines.extend(
            [
                "",
                "## Metadata/discovery errors",
                "",
                "| JAR | Error |",
                "| --- | --- |",
            ]
        )
        for issue in discovery:
            lines.append(
                f"| `{markdown_escape(str(issue['relative_path']))}` | "
                f"{markdown_escape(str(issue['detail']))} |"
            )

    lines.extend(
        [
            "",
            "## Per-game result",
            "",
            "| JAR | Main MIDlet | Status | Startup | Frames/unique | Inputs | Max idle | Primary error | Artifacts |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |",
        ]
    )
    for item in report["items"]:
        observed = item["observed"]
        failures = item["failures"]
        primary = ""
        if failures:
            primary = f"{failures[0]['kind']}: {failures[0]['detail']}"
        artifacts = item["artifacts"]
        artifact_text = artifacts.get("run_dir", "")
        lines.append(
            "| `{jar}` | `{main}` | `{status}` | {startup} | {frames}/{unique} | {inputs} | {idle} | {error} | `{artifacts}` |".format(
                jar=markdown_escape(str(item["relative_path"])),
                main=markdown_escape(str(item["main_class"])),
                status=markdown_escape(str(item["status"])),
                startup=observed.get("startup_ms", ""),
                frames=observed.get("frames_produced", 0),
                unique=observed.get("unique_frames", 0),
                inputs=(
                    int(observed.get("key_presses", 0) or 0)
                    + int(observed.get("lcdui_actions_sent", 0) or 0)
                ),
                idle=observed.get("longest_idle_ms", 0),
                error=markdown_escape(primary),
                artifacts=markdown_escape(str(artifact_text)),
            )
        )

    lines.extend(
        [
            "",
            "## Re-run commands",
            "",
            "```sh",
            "# Full headless integration run with real input and visual/stall checks",
            "bash Core/Tools/test-jar-integration.sh",
            "",
            "# Basic static scan + launch smoke test",
            "bash Core/Tools/test-jar-directory.sh",
            "",
            "# Test only matching names",
            "bash Core/Tools/test-jar-directory.sh --filter MIDPlay --filter Nicknsonet",
            "",
            "# Faster static inventory without launching games",
            "bash Core/Tools/test-jar-directory.sh --mode static",
            "",
            "# Longer startup window for a smaller subset",
            "bash Core/Tools/test-jar-directory.sh --filter Gameloft --limit 50 --timeout-ms 10000",
            "```",
            "",
            f"Machine-readable report: `{path.with_suffix('.json')}`",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch static/smoke test J2ME JARs and aggregate failures into one report."
    )
    parser.add_argument("--jar-dir", type=pathlib.Path, default=DEFAULT_JAR_DIR)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--mode", choices=("static", "smoke", "both"), default="both")
    parser.add_argument("--filter", action="append", default=[], help="case-insensitive path substring")
    parser.add_argument("--limit", type=int, default=0, help="maximum matching JAR files; 0 means all")
    parser.add_argument("--all-midlets", action="store_true", help="test every MIDlet-N entry, not only MIDlet-1")
    parser.add_argument(
        "--smoke-limit",
        type=int,
        default=0,
        help="launch only an evenly distributed sample; static scan still covers all selected JARs",
    )
    parser.add_argument(
        "--smoke-filter",
        action="append",
        default=[],
        help="always smoke-launch matching path/main class in addition to the sampled targets",
    )
    parser.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS)
    parser.add_argument(
        "--observe-ms",
        type=int,
        default=0,
        help="default duration to keep each MIDlet alive while pumping events",
    )
    parser.add_argument(
        "--observe-manifest",
        type=pathlib.Path,
        help="benchmark manifest whose per-JAR observe_ms values override the default",
    )
    parser.add_argument(
        "--autoplay",
        action="store_true",
        help="send real key press/release events and activate native LCDUI controls",
    )
    parser.add_argument(
        "--input-start-delay-ms",
        type=int,
        default=400,
        help="delay before the first automated input",
    )
    parser.add_argument(
        "--input-interval-ms",
        type=int,
        default=220,
        help="interval between automated inputs",
    )
    parser.add_argument(
        "--stall-ms",
        type=int,
        default=0,
        help="fail when visual/UI progress stops for this long after input; 0 disables",
    )
    parser.add_argument(
        "--heartbeat-ms",
        type=int,
        default=1_000,
        help="checkpoint runner state at this interval so hard hangs retain evidence",
    )
    parser.add_argument(
        "--require-visual",
        action="store_true",
        help="require a nonblank Canvas frame or a shown native LCDUI screen",
    )
    parser.add_argument(
        "--skip-teardown",
        action="store_true",
        help="exit the isolated runner after observation without invoking MIDlet teardown",
    )
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--runner", type=pathlib.Path, help="reuse an existing CompatibilityHarness binary")
    parser.add_argument("--sanitize", action="store_true", help="build harness with ASan/UBSan")
    args = parser.parse_args(argv)
    if args.limit < 0:
        parser.error("--limit must be >= 0")
    if args.smoke_limit < 0:
        parser.error("--smoke-limit must be >= 0")
    if args.jobs <= 0:
        parser.error("--jobs must be > 0")
    if args.timeout_ms <= 0:
        parser.error("--timeout-ms must be > 0")
    if not (0 <= args.observe_ms <= 120_000):
        parser.error("--observe-ms must be in 0..120000")
    if not (0 <= args.input_start_delay_ms <= 60_000):
        parser.error("--input-start-delay-ms must be in 0..60000")
    if not (40 <= args.input_interval_ms <= 60_000):
        parser.error("--input-interval-ms must be in 40..60000")
    if not (0 <= args.stall_ms <= 120_000):
        parser.error("--stall-ms must be in 0..120000")
    if not (100 <= args.heartbeat_ms <= 60_000):
        parser.error("--heartbeat-ms must be in 100..60000")
    if args.autoplay and args.observe_ms == 0 and args.observe_manifest is None:
        parser.error("--autoplay requires --observe-ms > 0 or --observe-manifest")
    if args.stall_ms > 0 and not args.autoplay:
        parser.error("--stall-ms requires --autoplay")
    if not (1 <= args.width <= 8192 and 1 <= args.height <= 8192):
        parser.error("--width/--height must be in 1..8192")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
    jar_root = args.jar_dir.expanduser().resolve()
    if not jar_root.is_dir():
        print(f"JAR directory does not exist: {jar_root}", file=sys.stderr)
        return 2

    observe_manifest: pathlib.Path | None = None
    observe_overrides: dict[str, int] = {}
    if args.observe_manifest is not None:
        observe_manifest = args.observe_manifest.expanduser().resolve()
        try:
            observe_data = json.loads(observe_manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"Cannot read observe manifest: {error}", file=sys.stderr)
            return 2
        benchmarks = observe_data.get("benchmarks", [])
        if not isinstance(benchmarks, list):
            print("Observe manifest has no benchmark list", file=sys.stderr)
            return 2
        for benchmark in benchmarks:
            if not isinstance(benchmark, dict):
                continue
            jar_value = benchmark.get("jar")
            duration_value = benchmark.get("observe_ms", 0)
            if not isinstance(jar_value, str) or not jar_value:
                continue
            if (not isinstance(duration_value, int) or
                    not 0 <= duration_value <= 120_000):
                print(
                    f"Invalid observe_ms for {jar_value}: {duration_value}",
                    file=sys.stderr,
                )
                return 2
            jar_name = pathlib.PurePosixPath(jar_value).name
            observe_overrides[jar_name] = duration_value

    maximum_observe_ms = max([args.observe_ms, *observe_overrides.values()])
    if (
        args.mode in ("smoke", "both")
        and maximum_observe_ms > 0
        and args.timeout_ms <= maximum_observe_ms + 1_000
    ):
        print(
            "--timeout-ms must be at least 1000 ms longer than the longest observe duration",
            file=sys.stderr,
        )
        return 2

    output_root = (
        args.output.expanduser().resolve()
        if args.output
        else CORE_ROOT / "build" / "jar-directory-tests" / utc_timestamp()
    )
    output_root.mkdir(parents=True, exist_ok=True)
    report_path = (
        args.report.expanduser().resolve()
        if args.report
        else output_root / "JAR_TEST_FIX_REPORT.md"
    )

    print(f"Discovering JARs under {jar_root}...", flush=True)
    targets, discovery_issues, discovered_count = discover_targets(
        jar_root,
        args.filter,
        args.limit,
        args.all_midlets,
    )
    print(
        f"Found {discovered_count} matching JARs; selected {len(targets)} MIDlet targets; "
        f"metadata errors: {len(discovery_issues)}",
        flush=True,
    )

    compat = load_compat_module()
    runner: pathlib.Path | None = None
    build_error = ""
    if args.mode in ("smoke", "both"):
        if args.runner:
            runner = args.runner.expanduser().resolve()
            if not runner.is_file():
                build_error = f"runner does not exist: {runner}"
                runner = None
        else:
            print("Building isolated CompatibilityHarness...", flush=True)
            runner, build_error = build_harness(output_root, args.sanitize)
        if build_error:
            print(build_error, file=sys.stderr, flush=True)

    smoke_ids: set[str] = set()
    if args.mode in ("smoke", "both") and runner is not None:
        if args.smoke_limit <= 0 or args.smoke_limit >= len(targets):
            smoke_ids = {target.item_id for target in targets}
        elif args.smoke_limit == 1:
            smoke_ids = {targets[0].item_id}
        else:
            last = len(targets) - 1
            indices = {
                round(index * last / (args.smoke_limit - 1))
                for index in range(args.smoke_limit)
            }
            smoke_ids = {targets[index].item_id for index in sorted(indices)}
        for target in targets:
            smoke_haystack = f"{target.relative_path}\n{target.main_class}".casefold()
            if any(token.casefold() in smoke_haystack for token in args.smoke_filter):
                smoke_ids.add(target.item_id)
        print(
            f"Smoke-launching {len(smoke_ids)} of {len(targets)} targets; "
            "all selected targets still receive static scanning.",
            flush=True,
        )

    results: list[TargetResult] = []
    if targets and (args.mode == "static" or runner is not None):
        global _PROGRESS_DONE, _PROGRESS_TOTAL, _PROGRESS_STARTED
        _PROGRESS_DONE = 0
        _PROGRESS_TOTAL = len(targets)
        _PROGRESS_STARTED = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
            futures = [
                executor.submit(
                    inspect_target,
                    compat,
                    target,
                    output_root,
                    runner if target.item_id in smoke_ids else None,
                    args.mode,
                    args.timeout_ms,
                    observe_overrides.get(
                        target.relative_path,
                        observe_overrides.get(
                            pathlib.PurePosixPath(target.relative_path).name,
                            args.observe_ms,
                        ),
                    ),
                    args.autoplay,
                    args.input_start_delay_ms,
                    args.input_interval_ms,
                    args.stall_ms,
                    args.heartbeat_ms,
                    args.require_visual,
                    args.skip_teardown,
                    args.width,
                    args.height,
                )
                for target in targets
            ]
            for future in concurrent.futures.as_completed(futures):
                try:
                    results.append(future.result())
                except Exception as exc:
                    # A tooling bug must be visible but must not discard all completed game evidence.
                    print(f"worker failed: {exc}", file=sys.stderr, flush=True)
        results.sort(key=lambda result: (result.target.relative_path.casefold(), result.target.main_class))

    report = aggregate_report(
        jar_root=jar_root,
        output_root=output_root,
        mode=args.mode,
        timeout_ms=args.timeout_ms,
        observe_ms=args.observe_ms,
        observe_manifest=observe_manifest,
        observe_overrides=observe_overrides,
        autoplay=args.autoplay,
        input_start_delay_ms=args.input_start_delay_ms,
        input_interval_ms=args.input_interval_ms,
        stall_ms=args.stall_ms,
        heartbeat_ms=args.heartbeat_ms,
        require_visual=args.require_visual,
        skip_teardown=args.skip_teardown,
        jobs=args.jobs,
        smoke_limit=args.smoke_limit,
        smoke_filters=args.smoke_filter,
        runner=runner,
        discovered_count=discovered_count,
        targets=targets,
        discovery_issues=discovery_issues,
        results=results,
        build_error=build_error,
        command=[str(SCRIPT_PATH), *(sys.argv[1:] if argv is None else argv)],
    )
    json_path = report_path.with_suffix(".json")
    json_write(json_path, report)
    write_markdown_report(report, report_path)

    print(f"Report: {report_path}", flush=True)
    print(f"JSON:   {json_path}", flush=True)

    if build_error:
        return 2
    failed_statuses = {
        "FAILED",
        "TIMEOUT",
        "NATIVE_CRASH",
        "LAUNCH_ERROR",
        "STALLED",
        "NO_VISUAL",
    }
    failed = sum(1 for result in results if result.status in failed_statuses)
    return 1 if failed or discovery_issues else 0


if __name__ == "__main__":
    raise SystemExit(main())
