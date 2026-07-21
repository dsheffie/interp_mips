#!/bin/bash
# boot_irix.sh -- boot the reconfigured IRIX 6.5.22 install on the interp_mips
# ISS, with the onboard ethernet (Seeq 8003 + HPC3 ENET DMA) attached to a host
# TAP device.
#
# Drops you on the IP22 serial console (SCC ttyd1). Log in as "root" (no
# password) and press Enter at "TERM = (vt100)" to reach the shell.
#
# The reconfigured image already has its kernel relinked, so with DETTIME=1
# (RTC frozen at 2030, matching henry/FPGA) IRIX skips the slow per-boot
# reconfigure. unix.clean is the matching 6.5.22f kernel extracted from iris.
#
# Networking: eth0 rides host TAP $SEEQ_TAP. Create it once (persistent, yours):
#   sudo ip tuntap add dev tap0 mode tap user "$USER"
#   sudo ip addr add 192.168.7.1/24 dev tap0 && sudo ip link set tap0 up
# then from the guest side the host is 192.168.7.1.
#
# Override any path/knob via the environment, and pass extra interp_mips flags
# through, e.g.:
#   DISK=/path/to/other.img ./boot_irix.sh
#   SEEQ_TAP= ./boot_irix.sh               # no tap (register model only)
#   ./boot_irix.sh -m 4000000000           # cap instruction count
set -euo pipefail
cd "$(dirname "$0")"

# Matching kernel + reconfigured disk + Henry ARCS firmware (boots /unix at 0xbfc00000).
KERNEL=${KERNEL:-/home/dsheffie/code/chd-dumper/extracted/unix.clean}
PROM=${PROM:-/home/dsheffie/code/r9999/arcs/henry_arcs.bin}
DISK=${DISK:-/home/dsheffie/code/iris/irix65-reconfigured.img}

# RTC frozen at 2030 so the reconfigured /unix reads as current (else IRIX
# re-runs the reconfigure every boot). Attach the onboard ethernet to a host
# TAP; empty SEEQ_TAP = no tap (register model only).
export DETTIME=${DETTIME:-1}
export SEEQ_TAP=${SEEQ_TAP:-tap0}

for f in "$KERNEL" "$PROM" "$DISK" ./interp_mips; do
    [ -e "$f" ] || { echo "boot_irix: missing $f" >&2; exit 1; }
done

exec ./interp_mips \
    --file "$KERNEL" \
    --prom "$PROM" \
    --start-pc 0xbfc00000 \
    --disk "$DISK" \
    "$@"
