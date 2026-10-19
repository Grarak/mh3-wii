#!/usr/bin/env python3
"""Sends commands to a running game's debug port and
prints each answer.

  debug-port.py [--port [HOST:]PORT] COMMAND...   each argument one command
  debug-port.py [--port [HOST:]PORT]              commands from stdin

The port defaults to $MH3_DEBUG_PORT, else 7777 on 127.0.0.1. Exits 1 when
an answer is an error.
"""
import argparse
import os
import socket
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', default=os.environ.get('MH3_DEBUG_PORT', '7777'))
    parser.add_argument('--timeout', type=float, default=120, help='seconds to wait for an answer')
    parser.add_argument('commands', nargs='*')
    args = parser.parse_args()
    host, _, port = args.port.rpartition(':')
    commands = args.commands or [line.rstrip('\n') for line in sys.stdin if line.strip()]
    failed = False

    with socket.create_connection((host or '127.0.0.1', int(port)), timeout=args.timeout) as sock:
        reader = sock.makefile('r')

        for command in commands:
            sock.sendall((command + '\n').encode())
            answer = reader.readline().rstrip('\n')
            print(answer, flush=True)
            failed |= not answer.startswith('ok')

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
