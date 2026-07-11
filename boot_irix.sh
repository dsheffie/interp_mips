#!/bin/bash
# boot_irix.sh -- boot the clean IRIX 6.5.22 install on the interp_mips ISS.
#
# Drops you on the IP22 serial console (SCC ttyd1). Log in as "root" (no
# password) and press Enter at "TERM = (vt100)" to reach the shell. The first
# boot runs a one-time kernel reconfigure that is slow on the ISS, so the
# login prompt can take a few minutes to appear.
#
# The clean image was built from the original 6.5.22 media in iris (see
# ~/code/iris); unix.clean is the matching 6.5.22f kernel extracted from it.
#
# Override any path via the environment, and pass extra interp_mips flags
# through, e.g.:
#   DISK=/path/to/other.img ./boot_irix.sh
#   ./boot_irix.sh -m 4000000000          # cap instruction count
set -euo pipefail
cd "$(dirname "$0")"

# Matching kernel + clean disk + Henry ARCS firmware (boots /unix at 0xbfc00000).
KERNEL=${KERNEL:-/home/dsheffie/code/chd-dumper/extracted/unix.clean}
PROM=${PROM:-/home/dsheffie/code/r9999/arcs/henry_arcs.bin}
DISK=${DISK:-/home/dsheffie/code/iris/irix65-clean.img}

for f in "$KERNEL" "$PROM" "$DISK" ./interp_mips; do
    [ -e "$f" ] || { echo "boot_irix: missing $f" >&2; exit 1; }
done

exec ./interp_mips \
    --file "$KERNEL" \
    --prom "$PROM" \
    --start-pc 0xbfc00000 \
    --disk "$DISK" \
    --disk-delta irix.delta \
    "$@"
