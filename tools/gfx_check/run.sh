#!/bin/sh
set -eu
POKE=""
scene=$1
name=${2:-$scene}
: "${GAME_DIR:?set GAME_DIR}" "${SAVE_SRC:?set SAVE_SRC}" "${WORK:?set WORK}"
FIRST=${FIRST:-2350}
LAST=${LAST:-2950}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
EXE="$ROOT/build/release/runtime/windfall.exe"
OUT="$WORK/gfx_$name"
case "$scene" in
    cam) IN="2400:RX+:120,2560:RX-:120,2720:RY+:40,2800:RY-:40,2860:LY-:60" ;;
    flick) IN="2400:RX+:12,2412:RX-:12,2424:RX+:20,2444:RX-:8,2452:RY+:10,2462:RX+:30,2492:RX-:30,2522:LY-:60,2530:RX+:40,2570:RX-:20" ;;
    walk) IN="2400:LY-:80,2500:LX+:60,2580:LY+:80,2680:LX-:60,2760:LY-:100,2880:A:4" ;;
    sword) IN="2400:B:6,2440:B:6,2480:B:6,2540:B:40,2620:LX+:30,2660:B:6,2700:R:60,2720:B:6,2800:Y:6,2860:A:4" ;;
    swim) IN="2400:LX-:200,2620:LY-:100,2740:LX+:100" ;;
    fade) IN="2400:LY-:30${FADEIN:-}"; POKE="2450:104741F0:736561000000000000002CFF,2452:104741FC:01" ;;
    *) echo "unknown scene $scene"; exit 1 ;;
esac
rm -rf "$OUT"
mkdir -p "$OUT/A" "$OUT/exA"
cp -r "$SAVE_SRC" "$OUT/saveA"
: > "$OUT/empty_settings.ini"
LOAD=$(python -c "print(','.join(f'{f}:A:8' for f in range(1300,1700,70)))")
SHOTS=$(python -c "print(','.join(str(f) for f in range($FIRST,$LAST)))")
env ${GAMEENV:-} ${POKE:+WF_POKE=$POKE} WF_SAVE_DIR="$OUT/saveA" WF_INPUT="$LOAD,$IN" WF_GAME_DIR="$GAME_DIR" WF_NO_MSGBOX=1 \
    WF_TIMEOUT=$((LAST / 15 + 30)) WF_LOG_QUIET=hle,fs,thread WF_SETTINGS_FILE="$OUT/empty_settings.ini" \
    WF_SHOT="$SHOTS" WF_SHOT_DIR="$OUT/A" WF_EXPLORER_DUMP="$FIRST-$LAST:$OUT/exA" \
    timeout $((LAST / 15 + 45)) "$EXE" > "$OUT/gameA.log" 2>&1 || true
echo "$(ls "$OUT/A" | wc -l) frames -> $OUT"
python "$HERE/../mp_check/analyze.py" "$OUT"
