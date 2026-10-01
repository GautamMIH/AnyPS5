#!/bin/bash
# Repeatable gameplay test: relinks a game into a scratch folder, restores a frozen copy of a save
# into it (the original save is never written), presses buttons on a schedule to reach gameplay,
# dumps every DUMP_EVERY-th presented frame and reports the frame times of the run's second half.
#
#   GAME=<dumped game folder> SAVE=<folder holding the save's _sd> tools/gameplay_test.sh [label]
#
# Environment:
#   GAME            the game's folder (as passed to relinker --game)               (required)
#   SAVE            a folder containing _sd (e.g. a played relink folder); copied once into
#                   $WORK/save-snapshot and restored from there on every run         (optional)
#   WORK            scratch folder for the snapshot, relink and logs   (default: ./gameplay-test)
#   SCRIPT          ANYPS5_SCRIPTED_INPUT schedule  (default: 25:cross,30:cross,35:cross,40:cross,50:cross,55:cross)
#   SECONDS_TO_RUN  run length in seconds                                           (default: 150)
#   DUMP_EVERY      dump every n-th presented frame as PPM                          (default: 600)
#   RELINKER        relinker binary                          (default: build/core/relinker/relinker)
# Rebuild the libs target first: the relinker copies build/core/libs/libs.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${GAME:?set GAME to the game folder}"
WORK=${WORK:-$PWD/gameplay-test}
RELINKER=${RELINKER:-$ROOT/build/core/relinker/relinker}
LABEL=${1:-gameplay}
RUN=$WORK/run
LOG=$WORK/$LABEL.log
mkdir -p "$WORK"

if [ -n "${SAVE:-}" ] && [ ! -d "$WORK/save-snapshot/_sd" ]; then
  mkdir -p "$WORK/save-snapshot" && cp -a "$SAVE/_sd" "$WORK/save-snapshot/_sd" || { echo "cannot snapshot $SAVE/_sd"; exit 1; }
  echo "save snapshot taken from $SAVE"
fi
rm -rf "$RUN"
"$RELINKER" --game "$GAME" "$RUN" > "$WORK/$LABEL-relink.log" 2>&1 || { echo "relink failed (see $WORK/$LABEL-relink.log)"; exit 1; }
[ -d "$WORK/save-snapshot/_sd" ] && cp -a "$WORK/save-snapshot/_sd" "$RUN/_sd"
cd "$RUN" || exit 1
# -k: a game that hangs while shutting down is killed 10 s after the timeout.
ANYPS5_SCRIPTED_INPUT="${SCRIPT:-25:cross,30:cross,35:cross,40:cross,50:cross,55:cross}" ANYPS5_DUMP_FRAMES="${DUMP_EVERY:-600}" \
  timeout -k 10 "${SECONDS_TO_RUN:-150}" ./eboot.elf > "$LOG" 2>&1
python3 - "$LOG" <<'EOF'
import re, sys
times = [float(m.group(1)) for line in open(sys.argv[1], errors='ignore') if (m := re.search(r'\[FrameTiming\] frame=\d+ .*?flip_interval_ms=([\d.]+)', line))]
if not times:
    print('no frames presented')
    sys.exit()
late = sorted(times[len(times) // 2:])
mean = sum(late) / len(late)
print(f'frames {len(times)}; second half: mean {mean:.1f} ms ({1000 / mean:.1f} fps), median {late[len(late) // 2]:.1f} ms, p95 {late[int(len(late) * 0.95)]:.1f} ms')
EOF
grep -a -m2 -E "what\(\)|unhandled SIG" "$LOG" | cut -c1-200
echo "log: $LOG"
ls "$RUN"/anyps5-frame-*.ppm 2>/dev/null | tail -3
