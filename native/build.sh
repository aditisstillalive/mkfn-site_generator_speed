#!/bin/sh
# Builds site.node, the Node-API addon solve.js loads, from site.c: freestanding (no libc), x86-64 Linux.
# OUT names another output; extra arguments go to gcc (-DPROF builds the phase counters).
cd "$(dirname "$0")" && gcc -O2 -std=gnu11 -shared -fPIC -nostdlib -ffreestanding -fno-stack-protector \
  -fno-tree-loop-distribute-patterns -fvisibility=hidden -msse2 -Wall -Wno-unused-function \
  -I"${NODE_INCLUDE:-/usr/include/node}" "$@" -o "${OUT:-../site.node}" site.c
