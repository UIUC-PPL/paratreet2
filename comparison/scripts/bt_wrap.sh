#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
stdbuf -oL -eL "$@" &
pid=$!
sleep 75
if kill -0 $pid 2>/dev/null; then
  echo "=== rank $SLURM_PROCID STILL RUNNING after 75 s; backtrace: ==="
  gdb -p $pid -batch -ex "set pagination off" -ex "thread apply all bt" 2>&1 \
    | grep -vE "^\[New LWP|^warning:|^Reading symbols" | head -70
  kill -9 $pid
else
  echo "=== rank $SLURM_PROCID finished on its own ==="
  wait $pid
fi
