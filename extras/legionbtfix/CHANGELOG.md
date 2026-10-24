# Changelog

## 0.2.0 (2026-10-17)

- Second patch: the A2DP Read Clock completion stores the clock even though the MT7925 returns handle 0. `Failed to read BT clock` is gone and the stream's RTP timestamps are anchored.
- Boot-args `-legionbtfixnoaddr` and `-legionbtfixnoclock` to disable one patch each.
- `tools/bt-hci-logging.sh` for the link-drop workaround, and the `.pklg` parsers used to measure it.

## 0.1.0 (2026-10-17)

- First patch: LE Create Connection always uses the public own address, which ends the `bluetoothd` crash loop (STATUS 718) on the MT7925 with macOS 26.7.1 (25G241).
