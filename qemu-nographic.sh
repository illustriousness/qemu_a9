#!/bin/sh

if ! command -v qemu-system-arm >/dev/null 2>&1; then
  echo "qemu-system-arm is not installed or not in PATH." >&2
  echo "On Ubuntu, install it with: sudo apt-get install qemu-system-arm" >&2
  exit 127
fi

if [ ! -f "sd.bin" ]; then
dd if=/dev/zero of=sd.bin bs=1024 count=65536
fi

qemu-system-arm -M vexpress-a9 -smp cpus=2 -kernel rtthread.bin -nographic -sd sd.bin
