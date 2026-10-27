#!/bin/sh
# Print the shairport-sync.conf for a device, using the AirPlay name from
# its player config file.
#
# The pipe output is 32-bit so shairport-sync's software volume costs no
# resolution and its dither stays far below audibility; the player reads
# S32_LE stereo at 44100 Hz from /tmp/airplay.pcm.
#
# Usage: ./shairport-conf.sh <config-file>

set -e

CONFIG="$1"
if [ -z "$CONFIG" ] || [ ! -f "$CONFIG" ]; then
    echo "Usage: $0 <config-file>" >&2
    exit 1
fi

DEVNAME=$(grep '^name' "$CONFIG" | head -1 | sed 's/^name[[:space:]]*=[[:space:]]*//')
if [ -z "$DEVNAME" ]; then
    echo "Error: config file must contain a 'name' line" >&2
    exit 1
fi

cat <<EOF
general = {
  name = "$DEVNAME";
  port = 8080;
  udp_port_base = 49152;
  udp_port_range = 3;
};
pipe = {
  name = "/tmp/airplay.pcm";
  output_rate = 44100;
  output_format = "S32_LE";
};
EOF
