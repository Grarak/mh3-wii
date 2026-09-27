#!/usr/bin/env python3
"""Send live input to an isolated Dolphin run's Classic Controller pipe."""

import argparse
import os
from pathlib import Path
import re
import time


BUTTONS = {
    "A", "B", "X", "Y", "Z", "START", "L", "R",
    "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT",
}
ROOT = Path(__file__).resolve().parent.parent


def duration(value: str) -> float:
    seconds = float(value)
    if not 0.02 <= seconds <= 60:
        raise argparse.ArgumentTypeError("seconds must be between 0.02 and 60")
    return seconds


def axis(value: str) -> float:
    position = float(value)
    if not -1 <= position <= 1:
        raise argparse.ArgumentTypeError("axis must be between -1 and 1")
    return position


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--run", required=True, help="name passed to run-dolphin-reference.sh")
parser.add_argument("--debug", action="store_true", help="use Dolphin's debug pipe: A saves slot 1, B loads it")
commands = parser.add_subparsers(dest="command", required=True)
tap = commands.add_parser("tap", help="tap a controller button")
tap.add_argument("button", choices=sorted(BUTTONS))
tap.add_argument("--seconds", type=duration, default=0.25)
stick = commands.add_parser("stick", help="hold the left stick at X Y (-1 to 1)")
stick.add_argument("x", type=axis)
stick.add_argument("y", type=axis)
stick.add_argument("--seconds", type=duration, default=1.0)
args = parser.parse_args()
if not re.fullmatch(r"[A-Za-z0-9_-]+", args.run):
    parser.error("run name must contain only letters, digits, underscores or hyphens")

pipe = ROOT / "logs" / "dolphin-runs" / args.run / "user" / "Pipes" / ("mh3-debug" if args.debug else "mh3")
try:
    fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
except OSError as exc:
    parser.error(f"cannot open live controller pipe {pipe}: {exc}")

with os.fdopen(fd, "w", buffering=1) as out:
    if args.command == "tap":
        out.write(f"PRESS {args.button}\n")
        time.sleep(args.seconds)
        out.write(f"RELEASE {args.button}\n")
    else:
        out.write(f"SET MAIN {(args.x + 1) / 2} {(args.y + 1) / 2}\n")
        time.sleep(args.seconds)
        out.write("SET MAIN 0.5 0.5\n")
