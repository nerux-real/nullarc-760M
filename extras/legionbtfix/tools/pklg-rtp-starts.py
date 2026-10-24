# <wtl> project: legion-hackintosh | file: scripts/diag/bt/pklg-rtp-starts.py | author: watchthelight </wtl>
# Reads a bluetoothd PacketLogger .pklg (big-endian length, 64-bit time, type, payload). Usage: python3 -I pklg-rtp-starts.py <file.pklg> [...]
import struct, sys, datetime
d = open(sys.argv[1], 'rb').read(); i = 0; out = []
while i + 13 <= len(d):
    n, ts, t = struct.unpack('>IQB', d[i:i+13]); p = d[i+13:i+4+n]; i += 4 + n
    if t == 2 and len(p) >= 20:
        hf, ln = struct.unpack('<HH', p[:4]); l2, cid = struct.unpack('<HH', p[4:8])
        if cid == 0x307 or (ln > 300):
            seq, rts = struct.unpack('>HI', p[10:16])
            out.append(((ts >> 32) + (ts & 0xffffffff) / 1e6, ln, cid, seq, rts, len(p)))
print(sys.argv[1].split('/')[-1], 'media', len(out))
starts = [k for k in range(len(out)) if out[k][3] == 1]
for k in starts:
    seg = out[k:k+5]
    print(' START at', datetime.datetime.fromtimestamp(seg[0][0]).strftime('%H:%M:%S'), [(o[3], o[4], o[1], o[5]) for o in seg])
