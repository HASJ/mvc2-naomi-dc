"""psp_snd0.py MUSIC OUT.AT3 VGMSTREAM ATRACDENC: a looping stream (BRSTM or anything vgmstream reads) to the XMB's
SND0.AT3: decoded once by vgmstream-cli, resampled to 44.1 kHz with the loop's end running on into its start, encoded
by atracdenc as ATRAC3 132 kbps, and put in a RIFF with Sony's fact (1093 samples skipped) and smpl (the loop) chunks.
The build packs port/psp/SND0.AT3."""
import math
import os
import struct
import subprocess
import sys
import tempfile

SKIP = 1093         # ATRAC3's delay, which Sony's files skip


def chunks(path):
    d = open(path, 'rb').read()
    i, out = 12, {}
    while i + 8 <= len(d):
        t, n = d[i:i + 4], struct.unpack_from('<I', d, i + 4)[0]
        out[t] = d[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    return out


def loop_start(music, vgmstream):
    for line in subprocess.run([vgmstream, '-m', music], capture_output=True, text=True, check=True).stdout.splitlines():
        if line.startswith('loop start:'):
            return int(line.split()[2])
    return 0


def resample(pcm, rate, loop):
    """stereo 16-bit samples at rate to 44.1 kHz by a windowed sinc; past the end the loop again"""
    g = math.gcd(44100, rate)
    up, down, half = 44100 // g, rate // g, 32
    fc = min(20000, rate * 0.45) / rate
    taps = []
    for p in range(up):
        row = []
        for k in range(-half + 1, half + 1):
            x = k - p / up
            w = 0.42 + 0.5 * math.cos(math.pi * x / half) + 0.08 * math.cos(2 * math.pi * x / half)
            row.append(2 * fc * (math.sin(2 * math.pi * fc * x) / (2 * math.pi * fc * x) if x else 1) * w)
        taps.append(row)
    chans = [pcm[0::2], pcm[1::2]]
    n = len(chans[0])

    def at(c, j):
        if j >= n:
            j = loop + (j - n) % (n - loop)
        return c[j] if j >= 0 else 0

    out = []
    for i in range(n * up // down):
        base, p = divmod(i * down, up)
        row = taps[p]
        for c in chans:
            acc = sum(row[k] * at(c, base - half + 1 + k) for k in range(2 * half))
            out.append(max(-32768, min(32767, round(acc))))
    return out


def main():
    music, out_path, vgmstream, atracdenc = sys.argv[1:5]
    with tempfile.TemporaryDirectory() as tmp:
        wav, wav44, at3 = (os.path.join(tmp, f) for f in ('once.wav', 'once44.wav', 'once.at3'))
        subprocess.run([vgmstream, '-i', '-o', wav, music], capture_output=True, check=True)
        c = chunks(wav)
        _, chans, rate, _, _, bits = struct.unpack('<HHIIHH', c[b'fmt '][:16])
        assert chans == 2 and bits == 16, (chans, bits)
        pcm = struct.unpack('<%dh' % (len(c[b'data']) // 2), c[b'data'])
        loop = loop_start(music, vgmstream)
        s = resample(pcm, rate, loop) if rate != 44100 else list(pcm)
        raw = struct.pack('<%dh' % len(s), *s)
        open(wav44, 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(raw)) + b'WAVEfmt ' +
                                struct.pack('<IHHIIHH', 16, 1, 2, 44100, 44100 * 4, 4, 16) + b'data' +
                                struct.pack('<I', len(raw)) + raw)
        subprocess.run([atracdenc, '-e', 'atrac3', '-i', wav44, '-o', at3], capture_output=True, check=True)
        d = open(at3, 'rb').read()
        fmt = d[12:52]
        assert fmt[:4] == b'fmt ' and d[52:56] == b'data'
        frames = d[60:]             # all of atracdenc's frames, past the size its header gives
    n = len(s) // 2
    a = round(loop * 44100 / rate)
    fact = b'fact' + struct.pack('<III', 8, n, SKIP)
    smpl = b'smpl' + struct.pack('<10I', 60, 0, 0, 22676, 60, 0, 0, 0, 1, 0) + \
        struct.pack('<6I', 0, 0, a + SKIP, n - 1 + SKIP, 0, 0)
    body = b'WAVE' + fmt + fact + (smpl if loop else b'') + b'data' + struct.pack('<I', len(frames)) + frames
    open(out_path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
    print('%s: %d samples (%.2f s), loop from %d, %d bytes' % (out_path, n, n / 44100, a, len(body) + 8))


main()
