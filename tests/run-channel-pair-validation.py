#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture real Qt/converter pair routing on a private quad PulseAudio software sink."""

import argparse
import array
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def validate(args, root, runtime):
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=str(runtime), PULSE_SERVER=f"unix:{runtime}/native",
               PULSE_CLIENTCONFIG=str(runtime / "client.conf"), QT_MEDIA_BACKEND="pulseaudio")
    (runtime / "client.conf").write_text("autospawn = no\n")
    channel_map = "front-left,front-right,rear-left,rear-right"
    config = runtime / "server.pa"
    config.write_text(
        f"load-module module-native-protocol-unix socket={runtime}/native auth-anonymous=1\n"
        f"load-module module-null-sink sink_name=pairs rate=48000 channels=4 channel_map={channel_map}\n"
    )
    command = [args.pulseaudio, "-n", "--daemonize=no", "--use-pid-file=no", "--exit-idle-time=-1",
               "--log-target=stderr", f"--file={config}"]
    if args.pulse_module_dir:
        command.append(f"--dl-search-path={args.pulse_module_dir.resolve()}")
    server = capture = None
    report = {"passed": False, "scenarios": {}}
    try:
        with (root / "pulse.log").open("w") as log:
            server = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError("Private PulseAudio exited; see pulse.log")
                check = subprocess.run([args.pactl, "info"], env=env, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL, timeout=5)
                if check.returncode == 0:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Private audio server did not become ready")
            report["pulse_version"] = subprocess.check_output([args.pulseaudio, "--version"], env=env,
                                                              text=True).strip()
            for name, pair, concurrent in (("pair-1", 1, False), ("pair-3", 3, False), ("concurrent", 3, True)):
                path = root / f"{name}.raw"
                with path.open("wb") as output, (root / f"{name}.log").open("w") as probe_log:
                    capture = subprocess.Popen(
                        [args.parec, "--device=pairs.monitor", "--format=float32le", "--rate=48000",
                         "--channels=4", f"--channel-map={channel_map}", "--latency-msec=30", "--raw"], env=env,
                        stdout=output, stderr=probe_log,
                    )
                    command = [str(args.probe.resolve()), "cGFpcnM=", str(pair)]
                    if concurrent:
                        command.append("--concurrent")
                    subprocess.run(command, env=env, check=True, timeout=15, stdout=probe_log,
                                   stderr=subprocess.STDOUT)
                    time.sleep(0.15)
                    stop(capture)
                    capture = None
                samples = array.array("f", path.read_bytes())
                if sys.byteorder != "little":
                    samples.byteswap()
                channels = [samples[channel::4] for channel in range(4)]
                if not channels[0]:
                    raise RuntimeError(f"No monitor capture for {name}")
                peaks = [max(map(abs, data)) for data in channels]
                expected = [220, 330, 440, 660]
                # Frequency energy detects unintended mixing even when all four lanes are active.
                energies = []
                for channel, data in enumerate(channels):
                    # Short windows avoid phase cancellation across pause/reset,
                    # where queued audio is intentionally discarded.
                    windows = [data[start:start + 4800] for start in range(0, len(data) - 4799, 4800)]
                    spectra = [[
                        2 * abs(sum(value * complex(math.cos(2 * math.pi * frequency * i / 48000),
                                                   math.sin(2 * math.pi * frequency * i / 48000))
                                    for i, value in enumerate(window))) / len(window)
                        for frequency in expected] for window in windows]
                    # Use the strongest complete tone window rather than a reset
                    # boundary, whose abrupt edge naturally has broadband energy.
                    energies.append(max(spectra, key=lambda spectrum: spectrum[channel], default=[0] * 4))
                report["scenarios"][name] = {"frames": len(channels[0]), "peaks": peaks, "energies": energies}
                for channel, data in enumerate(channels):
                    active = concurrent or channel // 2 == (pair - 1) // 2
                    if active:
                        if peaks[channel] < 0.09 or energies[channel][channel] < 0.02:
                            raise RuntimeError(f"Missing expected signal in {name}, channel {channel + 1}")
                        if any(value > 0.002 for frequency, value in enumerate(energies[channel])
                               if frequency != channel):
                            raise RuntimeError(f"Cross-pair/channel mixing in {name}, channel {channel + 1}")
                    elif any(value != 0.0 for value in data):
                        raise RuntimeError(f"Non-silent unselected channel in {name}, channel {channel + 1}")
            report["passed"] = True
    finally:
        stop(capture)
        stop(server)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=Path("build-test-agent/tests/channelpairprobe"))
    parser.add_argument("--pulseaudio", default=shutil.which("pulseaudio"))
    parser.add_argument("--pactl", default=shutil.which("pactl"))
    parser.add_argument("--parec", default=shutil.which("parec"))
    parser.add_argument("--pulse-module-dir", type=Path)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if not all((args.pulseaudio, args.pactl, args.parec)) or not args.probe.is_file():
        parser.error("Supply a built channelpairprobe and pulseaudio, pactl, parec executables")
    if args.artifacts:
        root = args.artifacts.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        root = Path(tempfile.mkdtemp(prefix="headphones-channel-pairs-"))
    print(f"Validation artifacts: {root}", flush=True)
    with tempfile.TemporaryDirectory(prefix="headphones-pair-pulse-") as runtime:
        validate(args, root, Path(runtime))


if __name__ == "__main__":
    main()
