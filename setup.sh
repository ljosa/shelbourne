#!/bin/sh
#
# Set up a new Bose SoundTouch device to run the Shelbourne player at boot.
#
# Prerequisites:
#   1. Device is on the network with SSH enabled (USB stick with remote_services)
#   2. A config file exists for the device (e.g., lmc.conf)
#
# Usage:
#   ./setup.sh <device-ip> <config-file>
#
# Examples:
#   ./setup.sh 192.168.2.40 lmc.conf       # Little Miss Chatterbox
#   ./setup.sh 192.168.2.42 mrnoisy.conf   # Mr Noisy
#
# What this does:
#   1. Builds player + airplayd binaries
#   2. Deploys binaries to /mnt/nv/
#   3. Deploys config file to /mnt/nv/player.conf
#   4. Generates and deploys shairport-sync.conf (AirPlay name from config)
#   5. Installs shepherd override for auto-boot (player + shairport-sync + ntpd)
#   6. Verifies the setup
#
# After setup:
#   1. Test manually:  ssh root@<ip> /mnt/nv/player
#   2. If it works:    ssh root@<ip> reboot
#   3. Player will auto-start on every boot
#
# Recovery:
#   - If SSH works:   make DEVICE=<ip> revert-shepherd
#   - Factory reset:  hold Preset 1 + Volume Down during boot (clears /mnt/nv)
#   - Factory reset disables SSH — re-plug USB stick with remote_services file
#
set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <device-ip> <config-file>"
    echo "Example: $0 192.168.2.40 lmc.conf"
    exit 1
fi

DEVICE="$1"
CONFIG="$2"
DEST=/mnt/nv

if [ ! -f "$CONFIG" ]; then
    echo "Error: config file '$CONFIG' not found"
    exit 1
fi

# Extract device name from config for shairport-sync
DEVNAME=$(grep '^name' "$CONFIG" | head -1 | sed 's/^name[[:space:]]*=[[:space:]]*//')
if [ -z "$DEVNAME" ]; then
    echo "Error: config file must contain a 'name' line"
    exit 1
fi

echo "=== Setting up $DEVNAME ($DEVICE) ==="
echo ""

# Step 1: Verify SSH
echo "Step 1: Checking SSH connectivity..."
if ! ssh -o ConnectTimeout=5 root@$DEVICE "echo ok" >/dev/null 2>&1; then
    echo "Error: Cannot SSH to root@$DEVICE"
    echo "Make sure SSH is enabled (USB stick with remote_services file)"
    exit 1
fi
echo "  SSH OK"

# Step 2: Build
echo "Step 2: Building..."
make all
echo "  Build OK"

# Step 3: Deploy binaries
echo "Step 3: Deploying binaries..."
scp build/player root@$DEVICE:$DEST/player
scp build/airplayd root@$DEVICE:$DEST/airplayd
ssh root@$DEVICE "chmod +x $DEST/player $DEST/airplayd"
echo "  Binaries deployed"

# Step 4: Deploy config
echo "Step 4: Deploying config..."
scp "$CONFIG" root@$DEVICE:$DEST/player.conf
echo "  Config deployed"

# Step 5: Generate and deploy shairport-sync.conf
echo "Step 5: Deploying shairport-sync config..."
cat > /tmp/shairport-sync.conf <<EOF
general = {
  name = "$DEVNAME";
  port = 8080;
  udp_port_base = 49152;
  udp_port_range = 3;
};
pipe = {
  name = "/tmp/airplay.pcm";
};
EOF
scp /tmp/shairport-sync.conf root@$DEVICE:$DEST/shairport-sync.conf
rm /tmp/shairport-sync.conf
echo "  shairport-sync.conf deployed (AirPlay name: $DEVNAME)"

# Step 6: Install shepherd override
echo "Step 6: Installing shepherd override..."
ssh root@$DEVICE "mkdir -p $DEST/shepherd"
scp shepherd-override/Shepherd-spotty.xml root@$DEVICE:$DEST/shepherd/
echo "  Shepherd override installed"

# Step 7: Copy shairport-sync binary if not already on device
echo "Step 7: Checking shairport-sync binary..."
if ! ssh root@$DEVICE "test -x $DEST/shairport-sync" 2>/dev/null; then
    if ssh root@192.168.2.42 "test -x $DEST/shairport-sync" 2>/dev/null; then
        echo "  Copying shairport-sync from Mr Noisy..."
        ssh root@192.168.2.42 "cat $DEST/shairport-sync" | ssh root@$DEVICE "cat > $DEST/shairport-sync && chmod +x $DEST/shairport-sync"
    else
        echo "  WARNING: shairport-sync not found on device or Mr Noisy"
        echo "  AirPlay via shairport-sync won't work until binary is deployed"
    fi
else
    echo "  shairport-sync already present"
fi

# Step 8: Verify
echo ""
echo "=== Verification ==="
echo ""
echo "Files on device:"
ssh root@$DEVICE "ls -la $DEST/player $DEST/player.conf $DEST/airplayd $DEST/shairport-sync $DEST/shairport-sync.conf $DEST/shepherd/Shepherd-spotty.xml" 2>&1
echo ""
echo "Config:"
ssh root@$DEVICE "cat $DEST/player.conf"
echo ""
echo "=== Setup complete for $DEVNAME ==="
echo ""
echo "Next steps:"
echo "  1. Test manually:  ssh root@$DEVICE $DEST/player"
echo "  2. If it works:    ssh root@$DEVICE reboot"
echo "  3. Player will auto-start on boot"
echo ""
echo "Recovery:"
echo "  - Remove override: make DEVICE=$DEVICE revert-shepherd"
echo "  - Factory reset:   hold Preset 1 + Volume Down during boot"
