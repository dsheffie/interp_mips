#!/bin/bash
# build.sh -- build interp_mips (Jazz/Magnum) as a WebAssembly bundle with emscripten.
#   ./wasm/build.sh        # -> wasm/site/{interp_mips.js,.wasm,index.html}
# Needs a sourced emsdk (emcc on PATH) -- the script sources ~/emsdk/emsdk_env.sh if present.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
[ -x "$(command -v emcc)" ] || source ~/emsdk/emsdk_env.sh >/dev/null 2>&1
command -v emcc >/dev/null || { echo "emcc not found (source emsdk_env.sh)"; exit 1; }

SF="$ROOT/build/_deps/softfloat-src"
[ -d "$SF/source" ] || { echo "softfloat source missing -- run the native cmake build once first"; exit 1; }
SFSRC="$SF/source"
SFBUILD="$SF/build/Linux-x86_64-GCC"     # for platform.h
SFFLAGS="-DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 -I$SFBUILD -I$SFSRC/8086-SSE -I$SFSRC/include"

OBJ="$HERE/obj"; OUT="$HERE/site"; DATA="$HERE/data"
mkdir -p "$OBJ" "$OUT" "$DATA"

# ---- 1. softfloat -> wasm objects (compile exactly the FAST_INT64 set the native .a built,
#         not a glob: globbing pulls in the non-FAST_INT64 *M.c multi-word variants) ----
echo ">> softfloat"
SFOBJ=""
for b in $(cd "$SFBUILD" && ls *.o 2>/dev/null | sed 's/\.o$//'); do
  c="$SFSRC/$b.c"; [ -f "$c" ] || c="$SFSRC/8086-SSE/$b.c"
  [ -f "$c" ] || { echo "softfloat: no source for $b"; exit 1; }
  o="$OBJ/sf_$b.o"
  [ "$o" -nt "$c" ] || emcc -c -O2 $SFFLAGS "$c" -o "$o"
  SFOBJ="$SFOBJ $o"
done

# ---- 2. stage the bundled clean-room Linux guest (firmware + kernel w/ embedded rootfs) ----
cp -u ~/code/jazz/jazz-mini/jazz_arcs.bin "$DATA/"
cp -u ~/code/linux-jazz/vmlinux.32        "$DATA/"

# ---- 3. the simulator (native main.cc / gdbstub / cosim / disassemble are excluded) ----
echo ">> interp_mips"
# sgi_* are the SGI/Indy platform (unused in jazz mode) but interpret.cc references their symbols;
# include them so the link resolves (they pull no sockets/unwind/capstone).
SRCS="wasm/em_main.cc interpret.cc loadelf.cc helper.cc pseudo_bios.cc sparse_mem.cc \
      cache_model.cc saveState.cc jazz.cc jazz_esp.cc jazz_sonic.cc jazz_sdl.cc \
      sgi_mc.cc sgi_hpc.cc sgi_scc.cc sgi_scsi.cc sgi_seeq.cc"
cd "$ROOT"
em++ -std=c++17 -O2 -DMIPS_EL -DNO_BOOST_SERIALIZATION \
  -I"$ROOT" -I"$SFSRC/include" \
  $SRCS $SFOBJ \
  -sUSE_SDL=2 \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 \
  -sMODULARIZE=1 -sEXPORT_NAME=InterpMips \
  -sEXPORTED_FUNCTIONS=_main,_boot_linux,_boot_nt \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS \
  -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1 \
  --embed-file "$DATA/jazz_arcs.bin@/jazz_arcs.bin" \
  --embed-file "$DATA/vmlinux.32@/vmlinux.32" \
  -o "$OUT/interp_mips.js"

cp -f "$HERE/index.html" "$OUT/index.html" 2>/dev/null || true
echo ">> done: $OUT/{interp_mips.js,interp_mips.wasm,index.html}"
ls -la "$OUT"
