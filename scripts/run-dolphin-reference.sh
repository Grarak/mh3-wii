#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
dump_frames=False
post_chief=False
dolphin_args=()
for arg in "$@"; do
  case "$arg" in
    --dump-frames) dump_frames=True ;;
    --post-chief) post_chief=True ;;
    *) dolphin_args+=("$arg") ;;
  esac
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
user="$PWD/logs/dolphin-user"
mkdir -p "$user/Config" "$user/GameSettings" "$user/Wii"
if [[ ! -f "$user/Wii/title/00010000/524d4850/data/nocopy/data00" ]]; then
  ditto saves/nand "$user/Wii"
fi
if [[ $post_chief == True ]]; then
  cat fixtures/post-chief-data00.bin > "$user/Wii/title/00010000/524d4850/data/nocopy/data00"
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
exec "$app" -u "$user" -e "$PWD/build/disc-decrypted.iso" "${dolphin_args[@]}"
