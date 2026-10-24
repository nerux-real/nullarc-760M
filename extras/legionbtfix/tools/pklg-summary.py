# <wtl> project: legion-hackintosh | file: scripts/diag/bt/pklg-summary.py | author: watchthelight </wtl>
# Reads a bluetoothd PacketLogger .pklg (big-endian length, 64-bit time, type, payload). Usage: python3 -I pklg-summary.py <file.pklg> [...]
import struct, sys, datetime, collections
d = open(sys.argv[1], 'rb').read(); i = 0; recs = []
while i + 13 <= len(d):
    n, ts, t = struct.unpack('>IQB', d[i:i+13]); p = d[i+13:i+4+n]; i += 4 + n
    sec, us = ts >> 32, ts & 0xffffffff
    recs.append((sec + us / 1e6, t, p))
print('records', len(recs), 'types', collections.Counter(r[1] for r in recs))
def tm(x): return datetime.datetime.fromtimestamp(x).strftime('%H:%M:%S.%f')[:-3]
media = collections.Counter(); sig = []
for ts, t, p in recs:
    if t in (2, 3) and len(p) >= 8:
        h, ln = struct.unpack('<HH', p[:4]); l2, cid = struct.unpack('<HH', p[4:8]); pl = p[8:]
        if t == 2 and l2 > 300: media[tm(ts)[:5]] += 1
        if cid in (0x0209, 0x0206) or (t == 3 and cid == 0x0206) or (t == 2 and cid == 0x0209):
            sig.append((tm(ts), 'tx' if t == 2 else 'rx', hex(cid), pl[:8].hex()))
    if t == 0 and len(p) >= 3 and p[:2] in (b'\x07\x14',):  # read clock 0x1407
        print(tm(ts), 'CMD ReadClock', p.hex())
    if t == 1 and len(p) >= 5 and p[0] == 0x0e and p[3:5] == b'\x07\x14':
        print(tm(ts), 'CC  ReadClock', p.hex())
    if t == 0xfc or t == 0xfb or t == 0xfa:
        txt = p.decode('utf8', 'replace').strip()
        if any(k in txt for k in ('A2DP', 'AVDTP', 'stream', 'clock', 'Clock')): print(tm(ts), 'NOTE', txt[:160])
print('media tx packets (>300 B) per minute:', dict(media))
for s in sig[:40]: print(*s)
