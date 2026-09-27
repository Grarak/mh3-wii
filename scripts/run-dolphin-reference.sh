#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
dump_frames=False
post_chief=False
pipe_controller=False
dolphin_args=()
run_name=
while (( $# )); do
  case "$1" in
    --dump-frames) dump_frames=True ;;
    --post-chief) post_chief=True ;;
    --pipe-controller) pipe_controller=True ;;
    --name)
      if (( $# < 2 )) || [[ ! $2 =~ ^[A-Za-z0-9_-]+$ ]]; then
        echo '--name requires letters, digits, underscores or hyphens' >&2
        exit 2
      fi
      run_name=$2
      shift ;;
    *) dolphin_args+=("$1") ;;
  esac
  shift
done
app=/Applications/Dolphin.app/Contents/MacOS/Dolphin
if [[ ! -x "$app" ]]; then
  echo 'Install Dolphin first: brew install --cask dolphin' >&2
  exit 1
fi
if [[ ! -f build/disc-decrypted.iso ]]; then
  echo 'Missing extracted disc: build/disc-decrypted.iso' >&2
  exit 1
fi
mkdir -p logs/dolphin-runs
if [[ -n $run_name ]]; then
  run="$PWD/logs/dolphin-runs/$run_name"
  if [[ -e $run ]]; then
    echo "Dolphin run already exists: $run" >&2
    exit 2
  fi
  mkdir "$run"
else
  run=$(mktemp -d "$PWD/logs/dolphin-runs/reference-XXXXXX")
fi
user="$run/user"
mkdir -p "$user/Config" "$user/GameSettings" "$user/Wii"
ditto saves/nand "$user/Wii"
if [[ $post_chief == True ]]; then
  cat fixtures/post-chief-data00.bin > "$user/Wii/title/00010000/524d4850/data/nocopy/data00"
  cmp -s fixtures/post-chief-data00.bin "$user/Wii/title/00010000/524d4850/data/nocopy/data00"
fi
cp scripts/dolphin-classic.ini "$user/Config/WiimoteNew.ini"
if [[ $pipe_controller == True ]]; then
  mkdir "$user/Pipes"
  mkfifo "$user/Pipes/mh3"
  cat > "$user/Config/WiimoteNew.ini" <<'EOF'
[Wiimote1]
Device = Pipe/0/mh3
Buttons/A = `Button A`
Buttons/B = `Button B`
Extension = Classic
Classic/Buttons/A = `Button A`
Classic/Buttons/B = `Button B`
Classic/Buttons/X = `Button X`
Classic/Buttons/Y = `Button Y`
Classic/Buttons/ZL = `Button L`
Classic/Buttons/ZR = `Button R`
Classic/Buttons/- = `Button Z`
Classic/Buttons/+ = `Button START`
Classic/D-Pad/Up = `Button D_UP`
Classic/D-Pad/Down = `Button D_DOWN`
Classic/D-Pad/Left = `Button D_LEFT`
Classic/D-Pad/Right = `Button D_RIGHT`
Classic/Left Stick/Up = `Axis MAIN Y -`
Classic/Left Stick/Down = `Axis MAIN Y +`
Classic/Left Stick/Left = `Axis MAIN X -`
Classic/Left Stick/Right = `Axis MAIN X +`
Classic/Right Stick/Up = `Axis C Y -`
Classic/Right Stick/Down = `Axis C Y +`
Classic/Right Stick/Left = `Axis C X -`
Classic/Right Stick/Right = `Axis C X +`
[Wiimote2]
[Wiimote3]
[Wiimote4]
[BalanceBoard]
EOF
  echo "Live controller pipe: $user/Pipes/mh3" >&2
fi
cat > "$user/GameSettings/RMHP08.ini" <<'EOF'
[Controls]
WiimoteSource0 = 1
EOF
cat > "$user/Config/Dolphin.ini" <<EOF
[Input]
BackgroundInput = True
[Wiimote]
WiimoteSource0 = 1
[Movie]
DumpFrames = $dump_frames
DumpFramesSilent = True
EOF
cat > "$user/Config/GFX.ini" <<'EOF'
[Settings]
DumpFramesAsImages = True
EOF
echo "Dolphin user directory and frame dumps: $user" >&2
echo "$$" > "$run/dolphin.pid"
exec "$app" -u "$user" -e "$PWD/build/disc-decrypted.iso" "${dolphin_args[@]}"
