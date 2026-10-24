#!/bin/bash
# <wtl> project: legionbtfix | file: tools/bt-hci-logging.sh | author: watchthelight </wtl>
# Workaround for the A2DP link drop on the MT7925 (see README): bluetoothd's own HCI packet logging with raw
# audio payloads keeps AirPods playing. The setting lives in /Library/Preferences/com.apple.MobileBluetooth.debug,
# bluetoothd reads it at start, and it survives reboots. Logs: /var/root/Library/Logs/Bluetooth/bluetoothd-hci-*.pklg
# (three files of about 4 MB, rotated). They contain device addresses; keep them private.
#   sudo tools/bt-hci-logging.sh on | off ; tools/bt-hci-logging.sh status
set -euo pipefail
P=/Library/Preferences/com.apple.MobileBluetooth.debug
case "${1:-status}" in
  on)
    [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
    defaults write "$P" HCITraces -dict StackDebugEnabled -bool true RawAudioTrace -bool true HCIFileTraces -bool true
    killall bluetoothd 2>/dev/null || true
    echo "HCI tracing with raw audio on; bluetoothd restarted (reconnect headphones: case in and out)"
    ;;
  off)
    [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
    defaults delete "$P" HCITraces 2>/dev/null || true
    killall bluetoothd 2>/dev/null || true
    echo "HCI tracing off; bluetoothd restarted"
    ;;
  status)
    defaults read "$P" HCITraces 2>/dev/null || echo "HCITraces not set (workaround off)"
    ;;
  *) echo "usage: $0 on|off|status"; exit 1 ;;
esac
