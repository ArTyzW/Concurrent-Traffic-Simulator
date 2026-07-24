#!/usr/bin/env bash
set -e  # If a command fails, terminate the script

# Compilation flags
CFLAGS="-Wall -Wextra -O2 -std=c11 -pthread"

echo "[compilation] incrocio.c -> incrocio"
gcc $CFLAGS incrocio.c -o incrocio 2>/dev/null \
  || gcc $CFLAGS incrocio.c -o incrocio -lrt

echo "[compilation] garage.c -> garage"
gcc $CFLAGS garage.c -o garage 2>/dev/null \
  || gcc $CFLAGS garage.c -o garage -lrt

# Clean output files
> incrocio.txt
> auto.txt

# Remove the shared FIFO if it already exists
rm -f /tmp/fifo_direzioni 2>/dev/null || true

# Start background processes
./garage & PID_GAR=$!
./incrocio & PID_INC=$!

echo "[start] incrocio PID=$PID_INC"
echo "[start] garage   PID=$PID_GAR"
echo

# Wait for ENTER key or for incrocio to exit
while kill -0 "$PID_INC" 2>/dev/null; do
  if read -t 0.2 -r _; then
    break
  fi
done

# If incrocio is still running, send SIGTERM
if kill -0 "$PID_INC" 2>/dev/null; then
  echo "[stop] terminating incrocio..."
  kill -TERM "$PID_INC" 2>/dev/null || true
fi

# Terminate garage (graceful -> forced)
echo "[stop] terminating garage..."
kill -TERM "$PID_GAR" 2>/dev/null || true
kill -KILL "$PID_GAR" 2>/dev/null || true

# Clean up the FIFO again
rm -f /tmp/fifo_direzioni 2>/dev/null || true

# Verify log files
if cmp -s incrocio.txt auto.txt; then
  echo "[check] OK: incrocio.txt and auto.txt are identical"
else
  echo "[check] Differences found:"
  diff -u incrocio.txt auto.txt || true
fi

# Final safety newline
echo