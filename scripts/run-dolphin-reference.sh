#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
dump_frames=False
post_chief=False
dolphin_args=()
run_name=
while (( $# )); do
  case "$1" in
    --dump-frames) dump_frames=True ;;
    --post-chief) post_chief=True ;;
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
cat > "$user/GameSettings/RMHP08.ini" <<'EOF'
[Controls]
WiimoteSource0 = 1
EOF
cat > "$user/Config/Dolphin.ini" <<EOF
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
