#!/bin/bash
# Build and install rc-central on the controllers: deploy-central.sh [host...]
set -euo pipefail
KEY=${RC_PI_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
HOSTS=("$@")
if [ ${#HOSTS[@]} -eq 0 ]; then
  HOSTS=(alpental@192.168.45.50 alpental@192.168.45.176)
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for h in "${HOSTS[@]}"; do
  rsync -a --delete -e "ssh -i $KEY" "$ROOT/common" "$ROOT/central" "$h:rc-src/"
  ssh -i "$KEY" -o BatchMode=yes "$h" '
    set -e
    cmake -S rc-src/central -B rc-src/build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build rc-src/build
    sudo cmake --install rc-src/build --prefix /usr/local >/dev/null
    sudo install -m 644 rc-src/central/rc-central.service /etc/systemd/system/rc-central.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now rc-central.service
    sudo systemctl restart rc-central.service
    sudo install -D -m 644 rc-src/common/rclog.py /usr/local/lib/rc-central/rclog.py
    sudo install -D -m 755 rc-src/central/rc_logd.py /usr/local/lib/rc-central/rc_logd.py
    sudo install -m 644 rc-src/central/rc-logd.service /etc/systemd/system/rc-logd.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now rc-logd.service
    sudo systemctl restart rc-logd.service
    echo "$(hostname): rc-central $(systemctl is-active rc-central), rc-logd $(systemctl is-active rc-logd)"'
done
