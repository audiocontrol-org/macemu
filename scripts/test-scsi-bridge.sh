#!/usr/bin/env bash
#
# Automated SCSI bridge integration test
#
# 1. Builds SheepShaver with scsi_s2p backend (if not already built)
# 2. Builds runtime container with VNC
# 3. Ensures s2p is running on the Pi
# 4. Starts SheepShaver container
# 5. Waits for SCSI probes to appear in s2p logs
# 6. Reports pass/fail
#
# Usage: ./scripts/test-scsi-bridge.sh [s2p-host]
#
# Prerequisites:
#   - Docker running
#   - ROM file at ~/Downloads/Mac OS ROM
#   - Disk image at ~/Downloads/Macintosh HD
#   - s2p running on Pi (or specify host)

set -euo pipefail

S2P_HOST="${1:-s3k.local}"
S2P_PORT="6868"
PI_SSH="orion@${S2P_HOST}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/../../sheepshaver-data"
ROM_FILE="${DATA_DIR}/rom"
DISK_FILE="${DATA_DIR}/disk.hfv"

echo "=== SheepShaver SCSI Bridge Integration Test ==="
echo ""
echo "  s2p host: ${S2P_HOST}:${S2P_PORT}"
echo "  ROM:      ${ROM_FILE}"
echo "  Disk:     ${DISK_FILE}"
echo ""

# Validate files exist
if [ ! -f "$ROM_FILE" ]; then
  echo "ERROR: ROM file not found: $ROM_FILE"
  exit 1
fi
if [ ! -f "$DISK_FILE" ]; then
  echo "ERROR: Disk image not found: $DISK_FILE"
  exit 1
fi

# Step 1: Build SheepShaver (if image doesn't exist)
echo "Step 1: Building SheepShaver..."
if ! docker image inspect sheepshaver-build &>/dev/null; then
  docker build --platform linux/amd64 -t sheepshaver-build -f Dockerfile.sheepshaver . 2>&1 | tail -3
fi
echo "  ✓ Build image ready"

# Step 2: Build runtime container
echo "Step 2: Building runtime container..."
docker build --platform linux/amd64 -t sheepshaver-run -f Dockerfile.sheepshaver-run . 2>&1 | tail -3
echo "  ✓ Runtime image ready"

# Step 3: Ensure s2p is running on Pi
echo "Step 3: Checking s2p on Pi..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_SSH" "nc -z localhost $S2P_PORT" 2>/dev/null; then
  echo "  Starting s2p on Pi..."
  ssh "$PI_SSH" "sudo systemctl stop s2p 2>/dev/null; true"
  sleep 1
  ssh "$PI_SSH" "sudo -n /tmp/s2p-midi --port $S2P_PORT > /tmp/sheepshaver-s2p.log 2>&1 &"
  sleep 3
fi
# Clear the log for this test
ssh "$PI_SSH" "echo '--- SCSI BRIDGE TEST START ---' >> /tmp/sheepshaver-s2p.log"
echo "  ✓ s2p running"

# Step 4: Start SheepShaver container
echo "Step 4: Starting SheepShaver..."

# Stop any existing container
docker rm -f sheepshaver-test 2>/dev/null || true

# Resolve s2p host to IP (container can't use .local mDNS)
S2P_IP=$(python3 -c "import socket; print(socket.gethostbyname('$S2P_HOST'))" 2>/dev/null || echo "$S2P_HOST")
echo "  s2p IP: $S2P_IP"

docker run -d \
  --name sheepshaver-test \
  --platform linux/amd64 \
  -e S2P_HOST="$S2P_IP" \
  -e S2P_PORT="$S2P_PORT" \
  -v "$ROM_FILE:/sheepshaver/rom:ro" \
  -v "$DISK_FILE:/sheepshaver/disk" \
  -p 6080:6080 \
  -p 5900:5900 \
  sheepshaver-run

echo "  ✓ Container started"
echo "  VNC: http://localhost:6080/vnc.html"

# Step 5: Wait for SCSI probes
echo ""
echo "Step 5: Waiting for SCSI probes (30s max)..."
echo "  Watching s2p log for INQUIRY commands..."

FOUND=false
for i in $(seq 1 30); do
  sleep 1
  # Check if SheepShaver is still running
  if ! docker ps | grep -q sheepshaver-test; then
    echo "  SheepShaver exited!"
    echo "  Container logs:"
    docker logs sheepshaver-test 2>&1 | tail -20
    break
  fi
  # Check s2p log for SCSI_EXEC commands (INQUIRY = CDB starting with 0x12)
  if ssh "$PI_SSH" "grep 'SCSI_EXEC' /tmp/sheepshaver-s2p.log 2>/dev/null" | grep -q "SCSI_EXEC"; then
    FOUND=true
    echo "  ✓ SCSI commands detected at ${i}s!"
    break
  fi
  if [ $((i % 5)) -eq 0 ]; then
    echo "  ... ${i}s"
  fi
done

# Step 6: Report results
echo ""
echo "=== Results ==="
if $FOUND; then
  echo "  ✓ PASS: SheepShaver sent SCSI commands via the network bridge"
  echo ""
  echo "  s2p log (SCSI_EXEC entries):"
  ssh "$PI_SSH" "grep 'SCSI_EXEC' /tmp/sheepshaver-s2p.log" | sed 's/^/    /'
else
  echo "  ✗ FAIL: No SCSI commands detected in s2p log"
  echo ""
  echo "  SheepShaver container logs:"
  docker logs sheepshaver-test 2>&1 | tail -30 | sed 's/^/    /'
  echo ""
  echo "  s2p log (last 10 lines):"
  ssh "$PI_SSH" "tail -10 /tmp/sheepshaver-s2p.log" | sed 's/^/    /'
fi

echo ""
echo "Container 'sheepshaver-test' is still running."
echo "  VNC: http://localhost:6080/vnc.html"
echo "  Stop: docker rm -f sheepshaver-test"
