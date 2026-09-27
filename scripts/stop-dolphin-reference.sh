#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if (( $# != 1 )) || [[ ! $1 =~ ^[A-Za-z0-9_-]+$ ]]; then
  echo 'usage: stop-dolphin-reference.sh RUN_NAME' >&2
  exit 2
fi
run="$PWD/logs/dolphin-runs/$1"
pid_file="$run/dolphin.pid"
if [[ ! -f $pid_file ]]; then
  echo "No Dolphin PID file: $pid_file" >&2
  exit 1
fi
read -r pid < "$pid_file"
if [[ ! $pid =~ ^[0-9]+$ ]]; then
  echo "Invalid Dolphin PID: $pid" >&2
  exit 1
fi
is_this_run() {
  local command
  command=$(ps -p "$pid" -o command= 2>/dev/null || true)
  [[ $command == *'/Dolphin '* && $command == *"-u $run/user "* ]]
}
if ! is_this_run; then
  echo "Dolphin run $1 is not running"
  exit 0
fi
kill -TERM "$pid" 2>/dev/null || true
for _ in {1..20}; do
  if ! is_this_run; then
    echo "Stopped Dolphin run $1"
    exit 0
  fi
  sleep 0.25
done
kill -KILL "$pid" 2>/dev/null || true
for _ in {1..20}; do
  if ! is_this_run; then
    echo "Stopped Dolphin run $1"
    exit 0
  fi
  sleep 0.25
done
echo "Dolphin run $1 is still running as PID $pid" >&2
exit 1
