# <wtl> project: legion-hackintosh | file: scripts/diag/bt/pklg-stream-timing.py | author: watchthelight </wtl>
# Reads a bluetoothd PacketLogger .pklg (big-endian length, 64-bit time, type, payload). Usage: python3 -I pklg-stream-timing.py <file.pklg> [...]
import struct, sys, datetime, statistics as st
d = open(sys.argv[1], 'rb').read(); i = 0; R = []
while i + 13 <= len(d):
    n, ts, t = struct.unpack('>IQB', d[i:i+13]); p = d[i+13:i+4+n]; i += 4 + n
    R.append(((ts >> 32) + (ts & 0xffffffff) / 1e6, t, p))
def win(a, b):
    sends = []; comp = []; ev = {}; other = []
    for ts, t, p in R:
        if not (a <= ts <= b): continue
        if t == 2 and len(p) >= 8:
            cid = struct.unpack('<H', p[6:8])[0]
            sends.append((ts, cid, struct.unpack('<H', p[2:4])[0]))
        if t == 1 and p and p[0] == 0x13:
            nh = p[2]; c = sum(struct.unpack('<H', p[3+2*nh+2*k:5+2*nh+2*k])[0] for k in range(nh)); comp.append((ts, c))
        if t == 1 and p: ev[p[0]] = ev.get(p[0], 0) + 1
        if t == 3 and len(p) >= 8:
            cid = struct.unpack('<H', p[6:8])[0]
            if cid not in (0x30b,): other.append((round(ts - a, 3), hex(cid), p[8:14].hex()))
    med = [s for s in sends if s[1] == 0x307]
    gaps = [round((y[0]-x[0])*1000, 1) for x, y in zip(med, med[1:])]
    # outstanding packets over time
    q = 0; qmax = 0; evs = sorted([(s[0], 1) for s in sends] + [(c[0], -c[1]) for c in comp])
    for _, v in evs: q += v; qmax = max(qmax, q)
    print(' media', len(med), 'other tx', len(sends)-len(med), 'gaps median', st.median(gaps) if gaps else None, 'min', min(gaps) if gaps else None, 'max', max(gaps) if gaps else None, 'completed', sum(c[1] for c in comp), 'max in flight', qmax, 'events', {hex(k): v for k, v in sorted(ev.items())})
    print(' rx (non-media) first 12:', other[:12])
import calendar
for lab, s in [('raw OK 08:33:32', sys.argv[2]), ('noraw FAIL 08:35:56', sys.argv[3])]:
    a = float(s); print(lab); win(a - 1, a + 9)
