#!/bin/bash
# Flash the STM32 attached to the bench host: flash-remote.sh <zephyr.hex>
set -euo pipefail
HOST=${RC_BENCH_HOST:-seokyoungjeong@seoks-macbook-air}
KEY=${RC_BENCH_KEY:-$HOME/.ssh/id_ed25519_alpental_pi}
scp -q -i "$KEY" "$1" "$HOST:/tmp/rc-io-card.hex"
ssh -i "$KEY" -o BatchMode=yes "$HOST" \
  '/opt/homebrew/bin/openocd -f board/stm32f3discovery.cfg -c "program /tmp/rc-io-card.hex verify reset exit" 2>&1 | grep -E "Verified|Error|error"'
