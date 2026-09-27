#!/usr/bin/env python3
"""Minimal interactive client for Dolphin's built-in PowerPC GDB stub.

Launch a reference run with --gdb-port PORT. The guest starts paused; use
"continue" here, then drive its Classic Controller with dolphin-input.py.
"""

import argparse
import select
import socket
import sys
import time


class Remote:
    def __init__(self, port: int):
        deadline = time.monotonic() + 20
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)
        self.socket.settimeout(10)

    def byte(self) -> bytes:
        data = self.socket.recv(1)
        if not data:
            raise EOFError("Dolphin closed the GDB socket")
        return data

    def send(self, command: str) -> None:
        payload = command.encode("ascii")
        checksum = sum(payload) & 255
        self.socket.sendall(b"$" + payload + f"#{checksum:02x}".encode("ascii"))
        acknowledgement = self.byte()
        if acknowledgement != b"+":
            raise RuntimeError(f"GDB stub rejected {command!r}: {acknowledgement!r}")

    def receive(self) -> str:
        while self.byte() != b"$":
            pass
        payload = bytearray()
        while (byte := self.byte()) != b"#":
            payload.extend(byte)
        checksum = int(self.byte() + self.byte(), 16)
        if (sum(payload) & 255) != checksum:
            self.socket.sendall(b"-")
            raise ValueError("Bad GDB reply checksum")
        self.socket.sendall(b"+")
        return payload.decode("ascii")

    def query(self, command: str) -> str:
        self.send(command)
        return self.receive()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    remote = Remote(args.port)
    running = False
    print("Connected to Dolphin PPC GDB stub. Commands: continue, interrupt, pc,"
          " regs, read ADDR SIZE, break ADDR, clear ADDR, step, quit", flush=True)
    while True:
        ready, _, _ = select.select([sys.stdin, remote.socket], [], [])
        if remote.socket in ready:
            reply = remote.receive()
            running = False
            print(f"Guest stopped: {reply}", flush=True)
        if sys.stdin not in ready:
            continue
        line = sys.stdin.readline()
        if not line:
            break
        parts = line.strip().split()
        if not parts:
            continue
        command = parts[0].lower()
        try:
            if command in ("quit", "exit"):
                if not running:
                    remote.send("c")
                break
            if command == "interrupt":
                if running:
                    remote.socket.sendall(b"\x03")
                continue
            if running:
                print("Interrupt the running guest before issuing this command", flush=True)
                continue
            if command == "continue":
                remote.send("c")
                running = True
            elif command == "step":
                remote.send("s")
                running = True
            elif command == "pc":
                print(f"PC = {remote.query('p40')}", flush=True)
            elif command == "regs":
                values = remote.query("g")
                for i in range(0, 32, 4):
                    print(" ".join(f"r{n:02}={values[n*8:n*8+8]}"
                                   for n in range(i, i + 4)), flush=True)
            elif command == "read" and len(parts) == 3:
                address, size = int(parts[1], 0), int(parts[2], 0)
                if not 1 <= size <= 4096:
                    raise ValueError("SIZE must be 1..4096")
                print(remote.query(f"m{address:x},{size:x}"), flush=True)
            elif command in ("break", "clear") and len(parts) == 2:
                address = int(parts[1], 0)
                operation = "Z" if command == "break" else "z"
                print(remote.query(f"{operation}0,{address:x},4"), flush=True)
            else:
                print("Unknown command", flush=True)
        except (OSError, ValueError, RuntimeError, EOFError) as error:
            print(f"GDB command failed: {error}", flush=True)
    remote.socket.close()


if __name__ == "__main__":
    main()
