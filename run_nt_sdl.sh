#!/bin/bash
# run_nt_sdl.sh -- boot Windows NT 4.0 (MIPS) on interp_mips --jazz with a live SDL window.
#
# The window shows the G364 display and captures keyboard + mouse:
#   click the window to focus, type/move/click to drive NT, close the window to stop.
# Needs a display (DISPLAY/Wayland); headless just runs windowless (Ctrl-C to stop).
#
# Disk is copy-on-write by default (the image is never modified); pass --disk-write to persist.
# Extra args pass straight through, e.g.:
#   ./run_nt_sdl.sh --disk-write            # let NT changes persist to the image
#   ./run_nt_sdl.sh --keys login.txt        # also feed a scripted key file
#   ./run_nt_sdl.sh --net-tap tap0          # bring up the SONIC on a host tap
#   ./run_nt_sdl.sh -m 40000000000          # cap instructions (default: run until window closes)
#
# Paths can be overridden via env: JAZZ_J0, JAZZ_PROM, JAZZ_DISK, JAZZ_NVRAM, JAZZ_WORK.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/interp_mips_el"
J0="${JAZZ_J0:-$HOME/code/jazz/j0}"
PROM="${JAZZ_PROM:-$J0/setup/NTPROM.RAW}"
DISK="${JAZZ_DISK:-$J0/nt4img/nt4.raw}"
NVRAM_SRC="${JAZZ_NVRAM:-$J0/nt4img/nvram}"
WORK="${JAZZ_WORK:-$HERE/nt_run}"

[ -x "$BIN" ] || { echo "error: $BIN not built -- run 'make EL=1' in $HERE first"; exit 1; }
for f in "$PROM" "$DISK" "$NVRAM_SRC"; do
  [ -f "$f" ] || { echo "error: missing $f (set JAZZ_* env or convert the qcow2 to nt4.raw)"; exit 1; }
done

mkdir -p "$WORK"
# NVRAM is written in place, so keep a persistent working copy: settings survive across runs and the
# pristine image stays untouched.  Delete "$WORK/nvram" to reset the ARC environment.
[ -f "$WORK/nvram" ] || cp "$NVRAM_SRC" "$WORK/nvram"

echo "booting NT 4.0 / Jazz on r9999 ISS"
echo "  disk : $DISK  (copy-on-write unless --disk-write)"
echo "  nvram: $WORK/nvram"
echo "  click the window to focus; close it to stop."

exec "$BIN" --jazz \
  --prom "$PROM" \
  --nvram "$WORK/nvram" \
  --disk "$DISK" \
  --sdl \
  "$@"
