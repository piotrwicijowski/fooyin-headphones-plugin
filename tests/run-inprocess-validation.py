#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in Linux validation using a private software-only PulseAudio server."""

import argparse
import array
import base64
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave


def fixture(path, rate, channels, seconds, frequency, floating=False):
    """Generate distinct, channel-identifiable signals without external fixtures."""
    samples = array.array("f" if floating else "h")
    for frame in range(rate):
        for channel in range(channels):
            value = 0.15 * math.sin(2 * math.pi * (frequency + channel * 173) * frame / rate)
            samples.append(value if floating else round(value * 32767))
    if sys.byteorder != "little":
        samples.byteswap()
    block = samples.tobytes()
    if floating:
        size = len(block) * seconds
        with path.open("wb") as output:
            output.write(struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", size + 36, b"WAVE", b"fmt ",
                                     16, 3, channels, rate, rate * channels * 4, channels * 4, 32, b"data", size))
            for _ in range(seconds):
                output.write(block)
    else:
        with wave.open(str(path), "wb") as output:
            output.setparams((channels, 2, rate, 0, "NONE", "not compressed"))
            for _ in range(seconds):
                output.writeframesraw(block)


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fooyin", required=True, type=Path, help="Matching built fooyin executable")
    parser.add_argument("--plugin-dir", type=Path, default=Path("build-test-agent"))
    parser.add_argument("--vim-plugin-dir", type=Path)
    parser.add_argument("--seconds", type=int, default=60, help="Stress duration, 0..3600 (default: 60)")
    parser.add_argument("--change-default", action="store_true",
                        help="Also test server default-device changes (fails if the server reroutes streams)")
    parser.add_argument("--artifacts", type=Path, help="New directory for fixtures, logs and report")
    parser.add_argument("--pulseaudio", default=shutil.which("pulseaudio"))
    parser.add_argument("--pactl", default=shutil.which("pactl"))
    parser.add_argument("--pulse-module-dir", type=Path, help="For a staged PulseAudio installation")
    args = parser.parse_args()
    if not 0 <= args.seconds <= 3600:
        parser.error("--seconds must be 0..3600")
    if not args.pulseaudio or not args.pactl:
        parser.error("pulseaudio and pactl are required (or supply their explicit paths)")
    args.fooyin = args.fooyin.resolve()
    plugin = args.plugin_dir.resolve()
    for path in (args.fooyin, plugin / "fyplugin_headphones.so",
                 plugin / "tests/inprocess/fytest_headphones_inprocess.so"):
        if not path.is_file():
            parser.error(f"Missing build artifact: {path}")
    plugins = [str(plugin), str(plugin / "tests/inprocess")]
    if args.vim_plugin_dir:
        vim = args.vim_plugin_dir.resolve()
        if not (vim / "fyplugin_vimmotions.so").is_file():
            parser.error(f"Missing Vim Motions module in {vim}")
        plugins.append(str(vim))
    if args.artifacts:
        root = args.artifacts.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        root = Path(tempfile.mkdtemp(prefix="headphones-validation-"))
    print(f"Validation artifacts: {root}", flush=True)
    # Keep the UNIX socket short even when the artifacts directory has a long path.
    with tempfile.TemporaryDirectory(prefix="headphones-pulse-") as runtime:
        return validate(args, root, Path(runtime), plugins)


def validate(args, root, runtime, plugins):
    env = os.environ.copy()
    env["HOME"] = str(root)
    for name in ("CONFIG", "DATA", "CACHE", "STATE"):
        directory = root / name.lower()
        directory.mkdir()
        env[f"XDG_{name}_HOME"] = str(directory)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    env["PULSE_SERVER"] = f"unix:{runtime}/native"
    env["PULSE_CLIENTCONFIG"] = str(runtime / "client.conf")
    (runtime / "client.conf").write_text("autospawn = no\n")
    # Do not connect this isolated application to the desktop's MPRIS/session bus.
    env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={runtime}/no-session-bus"
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_MEDIA_BACKEND"] = "pulseaudio"
    env["QT_LOGGING_RULES"] = "fy.headphones.debug=true"
    env["FOOYIN_PLUGIN_PATH"] = os.pathsep.join(plugins)
    env["HEADPHONES_INPROCESS_TEST"] = "1"
    env["HEADPHONES_TEST_STRESS_SECONDS"] = str(args.seconds)
    env["HEADPHONES_TEST_EXPECT_VIM"] = "1" if args.vim_plugin_dir else "0"
    for name in ("main", "headphones"):
        key = "HEADPHONES_TEST_MAIN_ID" if name == "main" else "HEADPHONES_TEST_ID"
        env[key] = base64.b64encode(name.encode()).decode()
    seconds = args.seconds + 30
    for name, rate, channels, frequency, floating in (
        ("main", 48000, 2, 220, True),
        ("preview", 44100, 1, 440, False),
        ("other", 96000, 2, 880, False),
    ):
        path = root / f"{name}.wav"
        fixture(path, rate, channels, seconds if name == "main" else 20, frequency, floating)
        env[f"HEADPHONES_TEST_{name.upper()}_FILE"] = str(path)
    config = runtime / "server.pa"
    config.write_text(
        f"load-module module-native-protocol-unix socket={runtime}/native auth-anonymous=1\n"
        "load-module module-null-sink sink_name=main rate=48000 channels=2\n"
        "load-module module-null-sink sink_name=headphones rate=48000 channels=2\n"
        "set-default-sink main\n"
    )
    command = [args.pulseaudio, "-n", "--daemonize=no", "--use-pid-file=no", "--exit-idle-time=-1",
               "--log-target=stderr", f"--file={config}"]
    if args.pulse_module_dir:
        command.append(f"--dl-search-path={args.pulse_module_dir.resolve()}")

    def pulse(*arguments):
        return subprocess.check_output([args.pactl, *arguments], env=env, text=True,
                                       stderr=subprocess.PIPE, timeout=5)

    server = host = None
    report = {"stress_seconds": args.seconds, "vim": bool(args.vim_plugin_dir),
              "routing_samples": 0, "default_changed": False, "rss_kib": [],
              "observed_formats": {}, "passed": False}
    try:
        with (root / "pulse.log").open("w") as pulse_log, (root / "fooyin.log").open("w") as host_log:
            server = subprocess.Popen(command, env=env, stdout=pulse_log, stderr=subprocess.STDOUT)
            ready = False
            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError("Private audio server exited; see pulse.log")
                try:
                    pulse("info")
                    ready = True
                    break
                except subprocess.CalledProcessError:
                    time.sleep(0.05)
            if not ready:
                raise RuntimeError("Private audio server did not become ready")
            report["pulse_version"] = subprocess.check_output([args.pulseaudio, "--version"], env=env, text=True).strip()
            sinks = {sink["name"]: sink["index"] for sink in json.loads(pulse("-f", "json", "list", "sinks"))}
            if set(sinks) != {"main", "headphones"}:
                raise RuntimeError(f"Unexpected private server sinks: {sinks}")
            host = subprocess.Popen([str(args.fooyin), "--skip-single"], env=env, cwd=root,
                                    stdout=host_log, stderr=subprocess.STDOUT)
            started = time.monotonic()
            last_cpu = 0
            while host.poll() is None:
                elapsed = time.monotonic() - started
                if elapsed > args.seconds + 60:
                    raise RuntimeError("Host validation timed out")
                try:
                    status = Path(f"/proc/{host.pid}/status").read_text().splitlines()
                    rss = next(int(line.split()[1]) for line in status if line.startswith("VmRSS:"))
                    report["rss_kib"].append([round(elapsed, 2), rss])
                    stat = Path(f"/proc/{host.pid}/stat").read_text().rsplit(")", 1)[1].split()
                    last_cpu = (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")
                except (FileNotFoundError, StopIteration, ProcessLookupError):
                    pass
                streams = json.loads(pulse("-f", "json", "list", "sink-inputs"))
                host_streams = [s for s in streams if s["properties"].get("application.process.id") == str(host.pid)]
                for stream in host_streams:
                    key = f"sink-{stream['sink']}: {stream['sample_specification']}"
                    report["observed_formats"].setdefault(key, stream["buffer_latency_usec"])
                if len(host_streams) == 2:
                    if {s["sink"] for s in host_streams} != set(sinks.values()):
                        report["unexpected_streams"] = host_streams
                        raise RuntimeError("Concurrent main/preview streams are not on separate explicit sinks")
                    report["routing_samples"] += 1
                    if args.change_default and not report["default_changed"]:
                        pulse("set-default-sink", "headphones")
                        report["default_changed"] = True
                elif len(host_streams) > 2:
                    raise RuntimeError("Duplicate main/preview streams detected")
                time.sleep(0.1)
            report["exit_code"] = host.returncode
            report["elapsed_seconds"] = round(time.monotonic() - started, 2)
            report["cpu_seconds"] = last_cpu
            warmed = [rss for elapsed, rss in report["rss_kib"] if elapsed >= 10]
            if warmed:
                report["warmed_rss_min_max_kib"] = [min(warmed), max(warmed)]
            log = (root / "fooyin.log").read_text()
            for line in log.splitlines():
                if "HEADPHONES " in line or "fy.headphones:" in line:
                    print(line)
            if host.returncode != 0 or "HEADPHONES SAME-PROCESS PASS" not in log:
                raise RuntimeError("Host workflow failed; see fooyin.log")
            if log.count("Shutdown complete") != 1 or not report["routing_samples"]:
                raise RuntimeError("Missing shutdown or concurrent explicit routing evidence")
            report["passed"] = True
            print(f"PASS: {report['routing_samples']} separate-routing observations; "
                  f"{last_cpu:.2f}s CPU / {report['elapsed_seconds']:.2f}s elapsed")
            if warmed:
                print(f"Warmed whole-process RSS: {min(warmed)}..{max(warmed)} KiB")
            return 0
    finally:
        stop(host)
        stop(server)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
