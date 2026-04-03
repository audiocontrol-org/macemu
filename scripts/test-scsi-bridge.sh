#!/usr/bin/env bash
#
# Automated SCSI bridge integration test
#
# Single Docker container with source mounted as a volume.
# Builds SheepShaver, configures prefs, runs it, checks for SCSI activity.
#
# Usage: ./scripts/test-scsi-bridge.sh [s2p-host]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
DATA_DIR="${SCRIPT_DIR}/../../sheepshaver-data"

S2P_HOST="${1:-s3k.local}"
S2P_PORT="6868"
PI_SSH="orion@${S2P_HOST}"
ROM_FILE="${DATA_DIR}/rom"
DISK_FILE="${DATA_DIR}/disk.hfv"
CONTAINER_NAME="sheepshaver-dev"

echo "=== SheepShaver SCSI Bridge Test ==="
echo "  s2p: ${S2P_HOST}:${S2P_PORT}"
echo ""

# Validate files
for f in "$ROM_FILE" "$DISK_FILE"; do
  [ -f "$f" ] || { echo "ERROR: $f not found"; exit 1; }
done

# Resolve s2p host to IP (container can't use .local mDNS)
S2P_IP=$(python3 -c "import socket; print(socket.gethostbyname('$S2P_HOST'))" 2>/dev/null || echo "$S2P_HOST")

# Build the image (just the base — source is mounted)
echo "Step 1: Building Docker image..."
docker build --platform linux/amd64 -t sheepshaver-dev -f "$REPO_DIR/Dockerfile.sheepshaver" "$REPO_DIR" 2>&1 | tail -3
echo "  ✓ Image ready"

# Ensure s2p is running on Pi
echo "Step 2: Checking s2p on Pi..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_SSH" "nc -z localhost $S2P_PORT" 2>/dev/null; then
  echo "  Starting s2p..."
  ssh "$PI_SSH" "sudo systemctl stop s2p 2>/dev/null; true" && sleep 1
  ssh "$PI_SSH" "sudo -n /tmp/s2p-midi --port $S2P_PORT > /tmp/sheepshaver-s2p.log 2>&1 &" && sleep 3
fi
ssh "$PI_SSH" "echo '--- TEST START ---' >> /tmp/sheepshaver-s2p.log"
echo "  ✓ s2p running at $S2P_IP"

# Stop any existing container
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

# Start container with source + data mounted
echo "Step 3: Starting container..."
docker run -d \
  --name "$CONTAINER_NAME" \
  --platform linux/amd64 \
  -v "$REPO_DIR:/src" \
  -v "$ROM_FILE:/sheepshaver/rom:ro" \
  -v "$DISK_FILE:/sheepshaver/disk" \
  -p 16080:6080 \
  sheepshaver-dev \
  bash -c "
    set -e

    # Build SheepShaver (incremental — artifacts persist via volume)
    echo '=== Building SheepShaver ==='
    cd /src/SheepShaver
    make links 2>/dev/null || true
    cd src/Unix
    if [ ! -f Makefile ]; then
      autoheader
      ./configure --enable-sdl-video --enable-sdl-audio --enable-scsi-s2p --without-esd
    fi
    make -j\$(nproc)
    echo '=== Build complete ==='

    # Write prefs
    mkdir -p /root/.config/SheepShaver
    cat > /root/.config/SheepShaver/prefs << PREFS
rom /sheepshaver/rom
disk /sheepshaver/disk
ramsize 134217728
frameskip 0
nocdrom true
nosound true
nogui true
extfs /sheepshaver/shared
s2p_host ${S2P_IP}
s2p_port ${S2P_PORT}
scsi0 s2p:0
scsi1 s2p:1
scsi2 s2p:2
scsi3 s2p:3
scsi4 s2p:4
scsi5 s2p:5
scsi6 s2p:6
PREFS

    # Start display
    Xvfb :0 -screen 0 1024x768x24 &
    sleep 1
    DISPLAY=:0 fluxbox &>/dev/null &
    x11vnc -display :0 -forever -nopw -shared -rfbport 5900 &>/dev/null &
    websockify --web /usr/share/novnc 6080 localhost:5900 &>/dev/null &

    # Auto-dismiss Disk First Aid dialog after boot
    (sleep 20 && DISPLAY=:0 xdotool key Return) &

    echo '=== Starting SheepShaver ==='
    echo 'VNC: http://localhost:16080/vnc.html'
    DISPLAY=:0 exec /src/SheepShaver/src/Unix/SheepShaver
  "

echo "  ✓ Container started"

# Wait for build + boot
echo "Step 4: Waiting for build + SCSI probes (120s max)..."
for i in $(seq 1 120); do
  sleep 1

  if ! docker ps | grep -q "$CONTAINER_NAME"; then
    echo "  Container exited!"
    docker logs "$CONTAINER_NAME" 2>&1 | tail -20
    exit 1
  fi

  # Check container logs for our scsi_s2p init messages
  if docker logs "$CONTAINER_NAME" 2>&1 | grep -q "scsi_s2p: init complete"; then
    echo "  ✓ SheepShaver SCSI init complete at ${i}s"
    docker logs "$CONTAINER_NAME" 2>&1 | grep "scsi_s2p:" | sed 's/^/    /'
    break
  fi

  # Check s2p log for SCSI_EXEC
  if ssh "$PI_SSH" "grep 'SCSI_EXEC' /tmp/sheepshaver-s2p.log 2>/dev/null" | grep -q "SCSI_EXEC"; then
    echo "  ✓ SCSI commands detected at ${i}s!"
    ssh "$PI_SSH" "grep 'SCSI_EXEC' /tmp/sheepshaver-s2p.log" | tail -5 | sed 's/^/    /'
    break
  fi

  if [ $((i % 10)) -eq 0 ]; then
    # Show last line of container output
    LAST=$(docker logs "$CONTAINER_NAME" 2>&1 | tail -1)
    echo "  ... ${i}s: $LAST"
  fi
done

echo ""
echo "VNC: http://localhost:16080/vnc.html"
echo "Logs: docker logs $CONTAINER_NAME"
echo "Shell: docker exec -it $CONTAINER_NAME bash"
echo "Stop: docker rm -f $CONTAINER_NAME"
