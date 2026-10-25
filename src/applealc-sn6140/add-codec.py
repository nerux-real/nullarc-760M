#!/usr/bin/env python3
"""Adds the Conexant SN6140 (HDA vendor/device 14F1:1F87, Lenovo IdeaPad Slim 5 16AHP10 / 83HW) to an AppleALC source tree.

AppleALC knows its sibling CX8070 (14F1:1F86). The SN6140 is the same codec family with the same node layout (DACs 0x10/0x11,
headphone pin 0x16, speaker pin 0x17, mics 0x19/0x1a, ADCs 0x13/0x14), and its pin defaults come from the codec itself, so the
CX8070 ThinkPad E14 resources (layout/platform 21) are cloned with the codec ID moved from 0x1F86 to 0x1F87. The AppleHDA binary
patches in the CX8070 Info.plist turn AppleHDA's built-in AD198x codec ID into the CX8070 one; for the SN6140 that replacement is
0x14F11F87 instead (little-endian 87 1f f1 14).

usage: add-codec.py <AppleALC checkout>
"""
import base64, os, plistlib, shutil, sys

root = sys.argv[1]
src, dst = os.path.join(root, "Resources", "CX8070"), os.path.join(root, "Resources", "SN6140")
if not os.path.isdir(src): sys.exit("no %s" % src)
shutil.rmtree(dst, ignore_errors=True)
os.makedirs(dst)
for f in ("layout21.xml", "Platforms21.xml"):
    s = open(os.path.join(src, f), encoding="utf-8").read()
    assert s.count("351346566") <= 1
    open(os.path.join(dst, f), "w", encoding="utf-8").write(s.replace("351346566", "351346567"))   # 0x14F11F86 -> 0x14F11F87

with open(os.path.join(src, "Info.plist"), "rb") as fh: p = plistlib.load(fh)
OLD, NEW = bytes.fromhex("861ff114"), bytes.fromhex("871ff114")
p["CodecID"] = 8071
p["CodecName"] = "SN6140"
p["Author"] = "nullarc (cloned from CX8070 by MacPeet / Andres ZeroCross)"
files = p["Files"]
files["Layouts"] = [e for e in files["Layouts"] if e["Id"] == 21]
files["Platforms"] = [e for e in files["Platforms"] if e["Id"] == 21]
for e in files["Layouts"] + files["Platforms"]:
    e["Comment"] = "Conexant SN6140 (14F1:1F87) for Lenovo IdeaPad Slim 5 16AHP10 (83HW), cloned from CX8070 layout 21"
n = 0
for patch in p["Patches"]:
    if patch.get("Replace") == OLD:
        patch["Replace"] = NEW; n += 1
if n != 1: sys.exit("expected exactly one patch that writes the CX8070 codec ID, found %d" % n)
with open(os.path.join(dst, "Info.plist"), "wb") as fh: plistlib.dump(p, fh)
print("SN6140 added: CodecID 8071, layout/platform 21, %d codec-ID patch moved" % n)
