#!/bin/sh
# Builds site.node, the Node-API addon solve.js loads, from site.c: freestanding (no libc), x86-64 Linux. site.c is
# compiled twice, for any x86-64 and (-DV3) for x86-64-v3; each object's own symbols are made local, so each keeps its
# own copy of everything, and the module init picks one by the CPU (see the end of site.c). V3=0 leaves the second out.
# OUT names another output; extra arguments go to gcc (-DPROF builds the phase counters).
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc() {
  gcc -O2 -std=gnu11 -fPIC -nostdlib -ffreestanding -fno-stack-protector -fno-tree-loop-distribute-patterns \
    -fvisibility=hidden -ffp-contract=off -Wall -Wno-unused-function -I"${NODE_INCLUDE:-/usr/include/node}" "$@" -c site.c
}
cc -msse2 "$@" -o "$T/base.o"
objcopy --localize-hidden "$T/base.o"
if [ "${V3:-1}" != 0 ]; then
  cc -march=x86-64-v3 -DV3 "$@" -o "$T/v3.o"
  objcopy --localize-hidden "$T/v3.o"
fi
gcc -shared -nostdlib -o "${OUT:-../site.node}" "$T"/*.o
