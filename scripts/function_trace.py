#!/usr/bin/env python3
"""Summarize an MH3_CALL_TRACE=PATH,movie trace (runtime/src/function_trace.cpp).

  function_trace.py PATH [--out DIR] [--depth N]

DIR (default PATH.report) gets, for each movie window:
  functions-W.txt  every function called, with calls, threads, seconds and
                   its busiest callers
  timeline-W.txt   per second, the functions called for the first time and
                   the ones that stopped being called
  sequence-W.txt   the game thread's calls in order around the last movie
                   frame as an indented tree (N levels below the outermost
                   frame, repeated sibling calls collapsed)
"""

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def guest_names():
    names = {}
    for line in (ROOT / "symbols.txt").read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2:
            try:
                names[int(parts[0], 16)] = parts[1]
            except ValueError:
                pass
    return names


class Symbols:
    def __init__(self, modules):
        self.modules = sorted(modules)  # (base, path)
        self.bases = [base for base, _ in self.modules]
        self.cache = {}
        self.guest = guest_names()

    def resolve(self, addresses):
        todo = defaultdict(list)
        for address in set(addresses) - self.cache.keys():
            index = bisect_right(self.bases, address) - 1
            if index < 0:
                self.cache[address] = f"0x{address:x}"
                continue
            base, path = self.modules[index]
            todo[path].append((address, address - base))
        for path, items in todo.items():
            query = "\n".join(f"0x{offset:x}" for _, offset in items) + "\n"
            out = subprocess.run(["llvm-symbolizer-21", f"--obj={path}", "--no-inlines", "-C", "-f"],
                                 input=query, capture_output=True, text=True).stdout
            blocks = [b for b in out.split("\n\n") if b.strip()]
            for (address, offset), block in zip(items, blocks):
                name = block.splitlines()[0]
                if name in ("??", ""):
                    name = f"{Path(path).name}+0x{offset:x}"
                self.cache[address] = self.pretty(name)
        return [self.cache[a] for a in addresses]

    def pretty(self, name):
        match = re.match(r"func_([0-9A-Fa-f]{8})", name)
        if match:
            guest = self.guest.get(int(match.group(1), 16))
            if guest:
                return f"{name} [{guest}]"
        return name

    def name(self, address):
        return self.resolve([address])[0]


def parse(path):
    modules, windows, threads, sequences = [], defaultdict(lambda: defaultdict(list)), {}, {}
    window = second = None
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "module":
            modules.append((int(parts[1], 16), parts[2]))
        elif parts[0] == "second":
            window, second = int(parts[1]), int(parts[2])
            windows[window][second]
        elif parts[0] == "thread":
            threads[int(parts[1])] = " ".join(parts[3:]) or parts[2]
        elif parts[0] == "sequence":
            sequences[int(parts[1])] = (parts[2], int(parts[4]), int(parts[6]))
        else:
            count, fn, site, mask = int(parts[0]), int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)
            windows[window][second].append((count, fn, site, mask))
    return modules, windows, threads, sequences


def thread_names(mask, threads):
    return ",".join(threads.get(i, str(i)) for i in range(64) if mask >> i & 1)


def report_functions(out, symbols, seconds, threads):
    calls, masks, seen, callers = Counter(), defaultdict(int), defaultdict(set), defaultdict(Counter)
    for second, rows in seconds.items():
        for count, fn, site, mask in rows:
            calls[fn] += count
            masks[fn] |= mask
            seen[fn].add(second)
            callers[fn][site] += count
    symbols.resolve(list(calls) + [s for c in callers.values() for s in c])
    with open(out, "w") as f:
        f.write(f"{len(calls)} functions, {sum(calls.values())} calls\n")
        f.write("calls\tthreads\tseconds\tfunction\tbusiest callers\n")
        for fn, count in calls.most_common():
            span = f"{min(seen[fn])}-{max(seen[fn])}"
            top = "; ".join(f"{symbols.name(site)} x{n}" for site, n in callers[fn].most_common(3))
            f.write(f"{count}\t{thread_names(masks[fn], threads)}\t{span}\t{symbols.name(fn)}\t{top}\n")


def report_timeline(out, symbols, seconds):
    per = {s: {fn for _, fn, _, _ in rows} for s, rows in seconds.items()}
    symbols.resolve([fn for fns in per.values() for fn in fns])
    before, last = set(), {}
    for s, fns in per.items():
        for fn in fns:
            last[fn] = s
    with open(out, "w") as f:
        for s in sorted(per):
            new = sorted(symbols.name(fn) for fn in per[s] - before)
            gone = sorted(symbols.name(fn) for fn, l in last.items() if l == s - 1)
            f.write(f"== second {s}: {len(per[s])} functions, {len(new)} new, {len(gone)} stopped\n")
            for name in new:
                f.write(f"  + {name}\n")
            for name in gone:
                f.write(f"  - {name}\n")
            before |= per[s]


def report_sequence(out, symbols, path, last_frame, max_depth):
    data = Path(path).read_bytes()
    count = len(data) // 16
    records = struct.iter_unpack("<QQ", data[:count * 16])
    events, depth, low, frame_time = [], 0, 0, None
    for index, (fn, site) in enumerate(records):
        if fn == 0:
            events.append(("time", index, site, depth))
            continue
        if fn & 1:
            depth -= 1
            low = min(low, depth)
            continue
        events.append(("call", index, fn, depth))
        depth += 1
    symbols.resolve(list({e[2] for e in events if e[0] == "call"}))
    for kind, index, value, _ in events:
        if kind == "time" and index <= last_frame:
            frame_time = value
    with open(out, "w") as f:
        f.write(f"{count} records; the last movie frame's decode is record {last_frame}; depth 0 is the shallowest frame seen\n")
        previous = None
        repeat = 0
        marked = False

        def flush():
            if previous and repeat > 1:
                f.write(f"{'  ' * previous[1]}  (x{repeat})\n")

        for kind, index, value, d in events:
            if not marked and index >= last_frame:
                flush()
                previous, repeat = None, 0
                f.write("======== last movie frame decode ========\n")
                marked = True
            if kind == "time":
                if frame_time is not None:
                    f.write(f"{'  ' * max(0, d - low)}[t {(value - frame_time) / 1e6:+.3f} ms]\n")
                continue
            level = d - low
            if level > max_depth:
                continue
            key = (value, level)
            if key == previous:
                repeat += 1
                continue
            flush()
            previous, repeat = key, 1
            f.write(f"{'  ' * level}{symbols.name(value)}\n")
        flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path")
    parser.add_argument("--out")
    parser.add_argument("--depth", type=int, default=12)
    args = parser.parse_args()
    modules, windows, threads, sequences = parse(args.path)
    symbols = Symbols(modules)
    out = Path(args.out or args.path + ".report")
    out.mkdir(parents=True, exist_ok=True)
    for window, seconds in windows.items():
        report_functions(out / f"functions-{window}.txt", symbols, seconds, threads)
        report_timeline(out / f"timeline-{window}.txt", symbols, seconds)
        if window in sequences:
            path, _, last_frame = sequences[window]
            report_sequence(out / f"sequence-{window}.txt", symbols, path, last_frame, args.depth)
        print(f"window {window}: {len(seconds)} s -> {out}")


if __name__ == "__main__":
    main()
