#!/bin/sh
set -eu
name=$1
: "${GAME_DIR:?set GAME_DIR}" "${SAVE_SRC:?set SAVE_SRC}" "${WORK:?set WORK}"
SERVER=${SERVER:-ws://127.0.0.1:27016}
FIRST=${FIRST:-2700}
LAST=${LAST:-3300}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
EXE="$ROOT/build/release/runtime/windfall.exe"
OUT="$WORK/check_$name"
rm -rf "$OUT"
mkdir -p "$OUT/A" "$OUT/B" "$OUT/exA" "$OUT/exB"
cp -r "$SAVE_SRC" "$OUT/saveA"
cp -r "$SAVE_SRC" "$OUT/saveB"
: > "$OUT/empty_settings.ini"

LOAD=$(python -c "print(','.join(f'{f}:A:8' for f in range(1300,1700,70)))")
SHOTS=$(python -c "print(','.join(str(f) for f in range($FIRST,$LAST)))")
HIN="2720:B:6,2770:LX-:8,2820:B:6,2870:LX+:8,2920:B:6,2970:LY+:6,3020:B:6,3060:LY-:10,3064:A:4,3120:B:6,3170:LX-:8,3220:B:6,3260:LX+:8"
GIN="2500:LX+:12,2710:B:6,2750:LY-:10,2790:B:6,2830:LY+:10,2880:B:6,2920:LX-:10,2960:B:6,3000:LX+:12,3004:A:4,3060:B:6,3100:LY-:8,3150:B:6,3200:LY+:8,3250:B:6"

game() {  # side, name, inputs, extra env
    env ${4:-} WF_SAVE_DIR="$OUT/save$1" WF_MP_SERVER="$SERVER" WF_MP_ROOM="$name" WF_MP_NAME="$2" WF_INPUT="$LOAD,$3" \
        WF_GAME_DIR="$GAME_DIR" WF_NO_MSGBOX=1 WF_TIMEOUT=$((LAST / 30 + 15)) WF_LOG_QUIET=hle,fs,thread \
        WF_SETTINGS_FILE="$OUT/empty_settings.ini" WF_SHOT="$SHOTS" WF_SHOT_DIR="$OUT/$1" \
        WF_EXPLORER_DUMP="$FIRST-$LAST:$OUT/ex$1" timeout $((LAST / 30 + 30)) "$EXE" > "$OUT/game$1.log" 2>&1 || true
}
game A Host "$HIN" "${HOSTENV:-}" &
sleep 3
game B Guest "$GIN" "${GUESTENV:-}"
wait
echo "host $(ls "$OUT/A" | wc -l) frames, guest $(ls "$OUT/B" | wc -l) frames -> $OUT"
python "$HERE/analyze.py" "$OUT"
