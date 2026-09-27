#!/usr/bin/env python3
"""Boot an isolated village save and advance the opening dialogue unattended.

The framebuffer is used only to recognize the dialogue box; game state is
advanced through the same Classic Controller input path as a real player.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPTURE = re.compile(r"frame-(\d+)\.bmp$")


def controller(path, buttons=0, x=0, y=0):
    next_path = path.with_suffix(".next")
    next_path.write_text(f"{buttons:x} {x} {y} 0 0\n")
    next_path.replace(path)


def latest_frame(directory):
    frames = ((int(match.group(1)), path) for path in directory.glob("frame-*.bmp")
              if (match := CAPTURE.search(path.name)))
    return max(frames, default=(0, None))


def overlay_visible(path):
    # Aurora's 640x528, 32-bit bottom-up BMP has a black dialogue-box corner
    # at (15, 500) and a blue-gray dialogue panel at (300, 500). Checking both
    # avoids mistaking the black loading screen for the first dialogue page.
    data = path.read_bytes()
    if len(data) < 54:
        raise ValueError(f"Incomplete capture: {path}")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    if (width, height, bits) != (640, 528, 32):
        raise ValueError(f"Unexpected capture dimensions: {width}x{height}x{bits}")
    def color(x, y):
        pixel = offset + (height - 1 - y) * width * 4 + x * 4
        return data[pixel:pixel + 3]
    blue, green, red = color(300, 500)
    dialogue = color(15, 500) == b"\0\0\0" and blue > 30 and green > 40 and red > 60
    reward = color(400, 100) == b"\0\0\0" and max(color(300, 100)) < 40
    return dialogue or reward


def wait_for_frame(directory, previous, process, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None:
            raise RuntimeError(f"MH3 exited with status {process.returncode}")
        number, path = latest_frame(directory)
        if number > previous and path and path.stat().st_size >= 640 * 528 * 4 + 54:
            return number, path
        time.sleep(0.1)
    raise TimeoutError(f"No frame after {previous} within {timeout}s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="village-replay", help="run name under logs/replays")
    parser.add_argument("--dock-edge", action="store_true", help="continue right from free movement toward the dock edge")
    parser.add_argument("--moga-woods", action="store_true", help="follow the dock route and select Exit to Moga Woods")
    parser.add_argument("--fresh", action="store_true", help="start from the original save and replay the Chief dialogue")
    parser.add_argument("--profile", action="store_true", help="log process CPU and Metal completion time per display copy")
    parser.add_argument("--capture-interval", type=int, default=5, help="capture every N display copies (default: 5)")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/metal/MH3", help="native executable to benchmark")
    parser.add_argument("--max-pulses", type=int, default=120)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.name):
        parser.error("--name must contain only letters, digits, underscores or hyphens")
    if args.capture_interval < 1:
        parser.error("--capture-interval must be positive")
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"Executable not found: {binary}")
    run = ROOT / "logs/replays" / args.name
    if run.exists():
        parser.error(f"Run already exists: {run}")
    checkpoint = ROOT / "fixtures/post-chief-data00.bin"
    source = ROOT / "saves/nand"
    if not source.is_dir():
        parser.error(f"Save not found: {source}")
    run.mkdir(parents=True)
    shutil.copytree(source, run / "user/nand")
    using_checkpoint = checkpoint.is_file() and not args.fresh
    if using_checkpoint:
        target = run / "user/nand/title/00010000/524d4850/data/nocopy/data00"
        if not target.is_file() or target.stat().st_size != checkpoint.stat().st_size:
            raise RuntimeError("Post-Chief checkpoint does not match the original save slot")
        # Write into the copied inode so its Wii NAND metadata xattr survives.
        target.write_bytes(checkpoint.read_bytes())
    input_path = run / "input.txt"
    controller(input_path)
    env = os.environ.copy()
    env.update(MH3_USER_DIR=str(run / "user"), MH3_INPUT_FILE=str(input_path),
               MH3_CAPTURE_PATH=str(run / "frame.bmp"), MH3_CAPTURE_INTERVAL=str(args.capture_interval))
    if args.profile:
        env["MH3_PROFILE_FRAMES"] = "50"
    with (run / "mh3.log").open("w") as output:
        process = subprocess.Popen([str(binary), "--village"], cwd=ROOT,
                                   env=env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
    (run / "pid").write_text(f"{process.pid}\n")
    print(f"MH3 PID {process.pid}; captures and log: {run}", flush=True)
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"MH3 exited before village load; see {run / 'mh3.log'}")
        if "village module verified" in (run / "mh3.log").read_text(errors="replace"):
            break
        time.sleep(0.25)
    else:
        raise TimeoutError(f"Village module did not load; see {run / 'mh3.log'}")
    frame = 0
    if using_checkpoint:
        # A checkpoint created through the game's Quit Game/Save flow has
        # already recorded the Chief conversation. Give the village load time
        # to settle before injecting movement; there is no dialogue to pulse.
        quiet_until = time.monotonic() + 12
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            frame, path = wait_for_frame(run, frame, process, 30)
            if overlay_visible(path):
                quiet_until = time.monotonic() + 12
            elif time.monotonic() >= quiet_until:
                break
        else:
            raise TimeoutError("Post-Chief checkpoint did not reach unobstructed village view")
        print(f"Post-Chief checkpoint loaded at frame {frame}: {path}", flush=True)
    else:
        frame = replay_opening_dialogue(run, frame, process, args.max_pulses, input_path)
    if args.dock_edge or args.moga_woods:
        if gate := os.environ.get("MH3_CALL_TRACE_GATE"):
            Path(gate).touch()
        controller(input_path, x=511)
        time.sleep(16)
        controller(input_path)
        frame, path = wait_for_frame(run, frame, process, 30)
        print(f"Dock-route capture at frame {frame}: {path}", flush=True)
    if args.moga_woods:
        controller(input_path, buttons=0x10)
        time.sleep(0.25)
        controller(input_path)
        print("Selected Exit to Moga Woods", flush=True)
    print(f"Game remains running as PID {process.pid}; input: {input_path}")


def replay_opening_dialogue(run, frame, process, max_pulses, input_path):
    opening_deadline = time.monotonic() + 180
    while time.monotonic() < opening_deadline:
        frame, path = wait_for_frame(run, frame, process, 30)
        if overlay_visible(path):
            print(f"Opening conversation visible at frame {frame}", flush=True)
            break
    else:
        raise TimeoutError("Opening dialogue did not appear")
    for pulse in range(max_pulses):
        controller(input_path, buttons=0x10)
        time.sleep(0.3)
        controller(input_path)
        time.sleep(0.2)
        frame, path = wait_for_frame(run, frame, process, 30)
        if not overlay_visible(path):
            # The Chief's scripted conversation briefly removes its overlay
            # while control is still locked, then opens another page. Require
            # a sustained quiet interval before calling this free movement.
            stable = True
            quiet_until = time.monotonic() + 12
            while time.monotonic() < quiet_until:
                frame, path = wait_for_frame(run, frame, process, 30)
                if overlay_visible(path):
                    stable = False
                    break
            if stable:
                print(f"Free movement after {pulse + 1} A pulses, frame {frame}: {path}", flush=True)
                return frame
    else:
        raise RuntimeError(f"Dialogue did not end within {max_pulses} pulses")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        sys.exit(f"Replay failed: {error}")
