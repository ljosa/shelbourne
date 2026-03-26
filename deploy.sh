#!/bin/sh
# Deploy player and airplayd to the Shelbourne device.
# Builds, copies binaries, and restarts via shepherd.
#
# Usage:
#   ./deploy.sh [device-ip] [config-file]
#   ./deploy.sh                           # deploy to 192.168.2.42 (Mr Noisy)
#   ./deploy.sh 192.168.2.40 lmc.conf     # deploy to Little Miss Chatterbox

set -e

DEVICE=${1:-192.168.2.42}
CONFIG=${2:-}
DEST=/mnt/nv

echo "Building..."
make all

echo "Copying binaries to $DEVICE..."
scp build/player root@$DEVICE:$DEST/player.new
scp build/airplayd root@$DEVICE:$DEST/airplayd.new

if [ -n "$CONFIG" ] && [ -f "$CONFIG" ]; then
    echo "Deploying config $CONFIG..."
    scp "$CONFIG" root@$DEVICE:$DEST/player.conf
fi

echo "Swapping binaries and restarting..."
ssh root@$DEVICE "
    mv $DEST/player.new $DEST/player &&
    mv $DEST/airplayd.new $DEST/airplayd &&
    chmod +x $DEST/player $DEST/airplayd &&
    kill \$(pidof player) 2>/dev/null
"
# Shepherd restarts player automatically

sleep 3
pid=$(ssh root@$DEVICE "pidof player")
echo "Deployed. Player running (pid $pid)."
