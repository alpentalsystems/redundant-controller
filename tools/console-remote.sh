#!/bin/bash
# Print the STM32 console from the bench host for N seconds: console-remote.sh [seconds]
set -euo pipefail
HOST=${RC_BENCH_HOST:-seokyoungjeong@seoks-macbook-air}
KEY=${RC_BENCH_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
ssh -i "$KEY" -o BatchMode=yes "$HOST" \
  "P=\$(ls /dev/cu.usbmodem* | head -1); { stty 115200 raw -echo; cat; } < \$P & c=\$!; sleep ${1:-5}; kill \$c" |
  LC_ALL=C sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
