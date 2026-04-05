#!/usr/bin/env bash
#
# rebuild-and-boot.sh — Build SheepShaver and restart it in the runtime container.
#
# Cycle: graceful OS shutdown → build → truncate log → start → wait for boot.
# Logs go to the shared volume (sheepshaver-data/sheepshaver.log) so the
# host can read them without docker exec.
#
# Usage:
#   ./scripts/rebuild-and-boot.sh              # build + boot, wait for idle hook
#   ./scripts/rebuild-and-boot.sh --no-wait    # build + boot, exit immediately
#   ./scripts/rebuild-and-boot.sh --log-only   # just tail the log (no build/boot)
#
# Prerequisites:
#   - Container "sheepshaver-os9-minimal" running with /src and /sheepshaver/shared mounted
#   - Xvfb + fluxbox already running in the container (persistent runtime container)
#
# Design: all waits are EVENT-BASED, never polling. We use:
#   - tail --pid=PID -f /dev/null  → blocks until PID exits (kernel event)
#   - tail -f file | grep -m1      → blocks until pattern appears (inotify event)
#   - grep on existing file         → instant check for already-happened events

set -euo pipefail

CONTAINER="sheepshaver-os9-minimal"
DATA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/sheepshaver-data"
LOG="${DATA_DIR}/sheepshaver.log"
HOOK_LOG="${DATA_DIR}/hook.log"
COMMAND_FILE="${DATA_DIR}/command.txt"
BUILD_DIR="/src/SheepShaver/src/Unix"
BINARY="${BUILD_DIR}/SheepShaver"
WAIT=true

for arg in "$@"; do
  case "$arg" in
    --no-wait)  WAIT=false ;;
    --log-only) echo "Tailing $LOG (Ctrl-C to stop)"; exec tail -f "$LOG" ;;
    *) echo "Unknown arg: $arg"; exit 1 ;;
  esac
done

# Verify container is running
if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
  echo "ERROR: Container '$CONTAINER' is not running."
  exit 1
fi

run() { docker exec "$CONTAINER" bash -c "$1"; }

# --- Graceful shutdown ---
# Sends SHUTDOWN via script_hook command file. Mac OS shuts down and
# SheepShaver exits on its own. We detect exit via tail --pid (kernel event).
# Ensures ALL SheepShaver instances are stopped (not just one).
shutdown_graceful() {
  local pids
  pids=$(run "pgrep SheepShaver 2>/dev/null" || true)
  if [ -z "$pids" ]; then
    echo "  Not running."
    return 0
  fi

  local count
  count=$(echo "$pids" | wc -l | tr -d ' ')
  echo "  Found $count SheepShaver process(es): $pids"

  # Send SHUTDOWN command — the script_hook or OP_IDLE_TIME picks it up,
  # triggers Mac OS _ShutDown, and SheepShaver exits when the OS finishes.
  echo "SHUTDOWN" > "$COMMAND_FILE"
  echo "  Sent SHUTDOWN command. Waiting up to 60s for graceful exit..."

  # Event-based wait: tail --pid blocks until the PID exits (kernel event, zero CPU).
  # Use the highest PID (most recent instance).
  local last_pid
  last_pid=$(echo "$pids" | tail -1)
  if timeout 60 docker exec "$CONTAINER" bash -c "tail --pid=$last_pid -f /dev/null 2>/dev/null"; then
    echo "  Graceful shutdown complete."
  else
    echo "  WARNING: Graceful shutdown timed out after 60s. Force-killing."
    run "pgrep SheepShaver | xargs -r kill -9 2>/dev/null || true"
  fi

  # Clean up any older zombie instances that predate the SHUTDOWN
  local remaining
  remaining=$(run "pgrep SheepShaver 2>/dev/null" || true)
  if [ -n "$remaining" ]; then
    echo "  Cleaning up leftover processes..."
    run "pgrep SheepShaver | xargs -r kill -9 2>/dev/null || true"
  fi

  rm -f "$COMMAND_FILE" 2>/dev/null
}

echo "=== Step 1: Stop SheepShaver ==="
shutdown_graceful

echo "=== Step 2: Build ==="
run "cd ${BUILD_DIR} && make -j\$(nproc) 2>&1" | tail -5
echo "  Build complete."

echo "=== Step 3: Truncate logs and start ==="
> "$LOG"
> "$HOOK_LOG" 2>/dev/null || true
run "DISPLAY=:0 ${BINARY} 2>/sheepshaver/shared/sheepshaver.log &"
echo "  SheepShaver started. Log: $LOG"

# Dismiss Disk First Aid dialog (appears ~10-15s into boot).
# Send Return key repeatedly until the hook log confirms boot completed.
echo "  Dismissing Disk First Aid dialog..."
for i in $(seq 1 30); do
  sleep 2
  run "DISPLAY=:0 xdotool key Return" 2>/dev/null || true
  if grep -q "Script hook initialized" "$HOOK_LOG" 2>/dev/null; then
    break
  fi
done

if [ "$WAIT" = false ]; then
  echo "  --no-wait: exiting without waiting for boot."
  exit 0
fi

echo "=== Step 4: Waiting for boot (sheepshaver.log 'BOOT_COMPLETE') ==="
BOOT_PATTERN="BOOT_COMPLETE"

if grep -q "$BOOT_PATTERN" "$LOG" 2>/dev/null; then
  echo "  Boot complete (already in log)."
elif timeout 180 bash -c "
  tail -f -n +1 '$LOG' 2>/dev/null | grep -m1 '$BOOT_PATTERN'
" >/dev/null 2>&1; then
  echo "  Boot complete."
else
  echo "  WARNING: Boot did not complete within 180s."
  echo "  Last 10 lines of log:"
  tail -10 "$LOG"
  exit 1
fi

echo ""
echo "SheepShaver is booted. Logs:"
echo "  Main:  $LOG"
echo "  Hook:  $HOOK_LOG"
echo "  VNC:   vnc://localhost:16082"
