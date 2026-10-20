#!/usr/bin/env python3
"""Starts each of a range of the local hunter's actions over a running
game's debug port (MH3_DEBUG_PORT) and measures what it leaves: logic steps
per wall second over --ticks of game time (run with MH3_FRAME_LIMIT=off to
see the speed an action costs), the action and motion 10 ticks in and after.
Between actions the hunter goes back to its stance (FieldEndAction) and to
where it started (actions that lunge or roll carry it off, into the water or
the next area); idle windows before and after are the baseline.

  action-sweep.py [--port [HOST:]PORT] [--group 4] [--actions 0-56] [--ticks 90]
                  [--charge LEVEL] [--shots DIR] [--setup COMMAND]...

Group 4 holds the weapon's actions (their tables: action-dispatch.py and
runtime/src/weapon_actions.cpp). --shots dumps a frame of each action into
DIR (on the machine the game runs on).
"""
import argparse
import os
import socket
import time


class Port:
    def __init__(self, address):
        host, _, port = address.rpartition(':')
        self.socket = socket.create_connection((host or '127.0.0.1', int(port)), timeout=300)
        self.reader = self.socket.makefile('r')

    def __call__(self, command):
        self.socket.sendall((command + '\n').encode())
        return self.reader.readline().rstrip('\n')


def fields(answer):
    words = answer.split()
    return {words[i]: words[i + 1] for i in range(1, len(words) - 1)}


def measure(port, ticks):
    """Steps and game ticks per wall second over `ticks` of game time."""
    a = fields(port('state'))
    t0 = time.monotonic()
    port(f'wait {ticks}')
    b = fields(port('state'))
    t1 = time.monotonic()
    return (int(b['step']) - int(a['step'])) / (t1 - t0), (float(b['time']) - float(a['time'])) / (t1 - t0)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', default=os.environ.get('MH3_DEBUG_PORT', '7777'))
    parser.add_argument('--group', type=int, default=4)
    parser.add_argument('--actions', default='0-56')
    parser.add_argument('--ticks', type=int, default=90)
    parser.add_argument('--charge', type=int, default=-1, help='charge level set before each action')
    parser.add_argument('--shots')
    parser.add_argument('--tag', default='', help='prefix of the shots\' names')
    parser.add_argument('--setup', action='append', default=[], help='a command run once first')
    args = parser.parse_args()
    first, _, last = args.actions.partition('-')
    port = Port(args.port)

    while not port('hunter').startswith('ok hunter'):
        time.sleep(2)

    for command in args.setup:
        print('#', command, '->', port(command), flush=True)

    hunter = port('hunter')
    plw, stance = hunter.split()[2], fields(hunter)['stance']
    words = port('state').split()
    place = words[words.index('at') + 1:words.index('at') + 4]
    print('#', hunter, 'at', ' '.join(place), flush=True)
    port('wait 60')
    base = [measure(port, args.ticks) for _ in range(3)]
    idle = sorted(steps for steps, _ in base)[1]
    print(f'idle steps/s {" ".join(f"{steps:.1f}" for steps, _ in base)}', flush=True)
    begin = time.monotonic()

    for action in range(int(first), int(last or first) + 1):
        try:
            if args.charge >= 0:
                port(f'hunter charge {args.charge}')

            started = time.monotonic() - begin
            port(f'hunter act {args.group} {action}')
            port('wait 10')
            during = fields(port('hunter'))

            if args.shots:
                port(f'shot {args.shots}/{args.tag}{args.group}-{action}.bmp')

            steps, _ = measure(port, args.ticks)
            after = fields(port('hunter'))
            print(f'{args.group}.{action:<3} {steps:7.1f} steps/s {100 * steps / idle:6.1f}% of idle | at +10: action {during.get("action")} motion {during.get("motion")} '
                  f'stance {during.get("stance")} | after: action {after.get("action")} motion {after.get("motion")} | at {started:.1f} s', flush=True)
            port(f'call 8027621C {plw} {stance} 4 0')
            port(f'pokef {int(plw, 16) + 0x3C:08x} {" ".join(place)}')
            port('wait 20')
        except (OSError, KeyError, ValueError) as error:
            print(f'{args.group}.{action:<3} failed: {error!r}', flush=True)
            return

    print(f'idle after {" ".join(f"{steps:.1f}" for steps, _ in (measure(port, args.ticks) for _ in range(2)))}', flush=True)


if __name__ == '__main__':
    main()
