#!/bin/bash
# Collect logs for an experiment: collect.sh <minutes> <console-seconds>
set -euo pipefail
KEY=${RC_PI_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
MIN=${1:-5}
for h in alpental@192.168.45.50 alpental@192.168.45.176; do
  echo "== $h"
  ssh -i "$KEY" -o BatchMode=yes "$h" "journalctl -u rc-central --since '-${MIN} min' --no-pager -o cat | grep -E 'event=(role_changed|referee_|peer_|fault|pbit|bit|healthy|unhealthy|mode_changed|tcp_command)'"
done
echo "== io-card"
"$(dirname "$0")/console-remote.sh" "${2:-3}" | grep -E 'active:|ready|io_bit|mode:'
