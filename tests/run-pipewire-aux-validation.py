#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce surround-to-AUX remapping and validate numbered pairs on private PipeWire."""

import argparse
import array
import base64
import json
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
    config = root / "config"
    shutil.copytree(args.pipewire_config, config / "pipewire")
    shutil.copytree(args.wireplumber_config, config / "wireplumber")
    extra = config / "pipewire/pipewire.conf.d"
    extra.mkdir(exist_ok=True)
    (extra / "20-aux.conf").write_text("""
context.objects = [
  { factory = adapter args = {
      factory.name = support.null-audio-sink
      node.name = pairs
      node.description = "Six-channel AUX test"
      media.class = Audio/Sink
      audio.rate = 96000
      audio.channels = 6
      audio.position = [ AUX0 AUX1 AUX2 AUX3 AUX4 AUX5 ]
      adapter.auto-port-config = { mode = dsp monitor = true position = preserve }
  } }
]
""")
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(config), XDG_RUNTIME_DIR=str(runtime),
               XDG_DATA_HOME=str(root / "data"), XDG_CACHE_HOME=str(root / "cache"),
               XDG_STATE_HOME=str(root / "state"), DBUS_SESSION_BUS_ADDRESS=f"unix:path={runtime}/no-session-bus",
               PIPEWIRE_CONFIG_DIR=str(config / "pipewire"),
               WIREPLUMBER_CONFIG_DIR=str(config / "wireplumber"), QT_AUDIO_BACKEND="pipewire",
               PULSE_SERVER=f"unix:{runtime}/pulse/native", PULSE_CLIENTCONFIG=str(runtime / "client.conf"))
    (runtime / "client.conf").write_text("autospawn = no\n")
    children = []
    report = {"passed": False, "qt_output": args.qt_output, "scenarios": {}}

    def launch(name, command, **kwargs):
        child = subprocess.Popen(command, env=env, stderr=(root / f"{name}.log").open("w"), **kwargs)
        children.append(child)
        return child

    def capture(name, players):
        path = root / f"{name}.raw"
        with path.open("wb") as output:
            recorder = launch(name + "-capture", [args.parec, "--device=pairs.monitor", "--format=float32le",
                "--rate=48000", "--channels=6", "--channel-map=aux0,aux1,aux2,aux3,aux4,aux5",
                "--latency-msec=30", "--raw"], stdout=output)
            running = []
            for index, (command, stdin) in enumerate(players):
                running.append(launch(f"{name}-player-{index}", command, stdin=stdin,
                                      stdout=subprocess.DEVNULL))
            for child in running:
                if child.wait(timeout=15) != 0:
                    raise RuntimeError(f"Playback failed in {name}; see logs")
            time.sleep(0.2)
            stop(recorder)
        samples = array.array("f", path.read_bytes())
        if sys.byteorder != "little":
            samples.byteswap()
        peaks = [max(map(abs, samples[channel::6]), default=0) for channel in range(6)]
        report["scenarios"][name] = peaks
        return peaks

    def player(pair, legacy=False):
        if args.qt_output and not legacy:
            return ([str(args.probe.resolve()), base64.b64encode(b"pairs").decode(), str(pair)], None)
        mapping = "FL,FR,FC,LFE,RL,RR" if legacy else "AUX0,AUX1,AUX2,AUX3,AUX4,AUX5"
        return ([args.pw_cat, "--playback", "--raw", "--target=pairs", "--rate=48000", "--format=f32",
                 "--channels=6", f"--channel-map={mapping}", "-"], (root / f"pair-{pair}.pcm").open("rb"))

    try:
        launch("pipewire", [args.pipewire], stdout=subprocess.DEVNULL)
        for _ in range(100):
            if (runtime / "pipewire-0").exists():
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Private PipeWire did not start")
        launch("wireplumber", [args.wireplumber, "--profile=policy"], stdout=subprocess.DEVNULL)
        launch("pulse", [args.pipewire_pulse], stdout=subprocess.DEVNULL)
        for _ in range(100):
            ready = subprocess.run([args.pactl, "list", "short", "sinks"], env=env, text=True,
                                   capture_output=True, timeout=5)
            if ready.returncode == 0 and "pairs" in ready.stdout:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Private AUX sink did not become available")
        # Exercise native metadata discovery and the plugin's actual converter.
        # The resulting unknown Qt channel config is translated to AUX0..N by
        # native Qt PipeWire. pw-cat also permits this check on older Qt builds.
        for pair in (1, 3, 5):
            with (root / f"pair-{pair}.pcm").open("wb") as output, (root / f"pair-{pair}.log").open("w") as log:
                subprocess.run([str(args.probe.resolve()), "--dump-pipewire-pcm", "pairs", str(pair)],
                               env=env, stdout=output, stderr=log, check=True, timeout=10)
        old = capture("legacy-surround-3", [player(3, legacy=True)])
        report["legacy_misrouting_reproduced"] = old[0] > 0.05 and old[1] > 0.05 and max(old[2:]) == 0
        for pairs in ((1,), (3,), (5,), (1, 3), (1, 5)):
            name = "aux-" + "-".join(map(str, pairs))
            peaks = capture(name, [player(pair) for pair in pairs])
            for channel, peak in enumerate(peaks):
                active = channel // 2 * 2 + 1 in pairs
                if (active and peak < 0.09) or (not active and peak != 0):
                    raise RuntimeError(f"Incorrect routing in {name}: {peaks}")
        report["passed"] = True
    finally:
        for child in reversed(children):
            stop(child)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=Path("build-test-agent/tests/channelpairprobe"))
    parser.add_argument("--qt-output", action="store_true", help="Use real native QAudioSink playback (Qt 6.10+)")
    for name in ("pipewire", "pipewire-pulse", "wireplumber", "pw-cat", "pactl", "parec"):
        parser.add_argument("--" + name, default=shutil.which(name))
    parser.add_argument("--pipewire-config", type=Path, default=Path("/usr/share/pipewire"))
    parser.add_argument("--wireplumber-config", type=Path, default=Path("/usr/share/wireplumber"))
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if not all((args.pipewire, args.pipewire_pulse, args.wireplumber, args.pw_cat, args.pactl, args.parec)):
        parser.error("PipeWire, WirePlumber, pw-cat and PulseAudio client utilities are required")
    if not args.probe.is_file() or not args.pipewire_config.is_dir() or not args.wireplumber_config.is_dir():
        parser.error("Supply a built channelpairprobe and installed configuration directories")
    root = args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix="headphones-aux-"))
    if args.artifacts:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Validation artifacts: {root}", flush=True)
    with tempfile.TemporaryDirectory(prefix="headphones-aux-pulse-") as runtime:
        validate(args, root, Path(runtime))


if __name__ == "__main__":
    main()
