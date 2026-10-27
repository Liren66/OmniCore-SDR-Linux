"""IIO TX test helper (AD9361 digital loopback).

  gen  <file>         two complex tones, int16 I1,Q1,I2,Q2: TX1 at +fs/32, TX2 at +fs/16
  check <capture>     RX capture (int16 I1,Q1,I2,Q2): tone power vs noise at +-fs/32, +-fs/16
"""
import cmath, math, struct, sys

N = 4096                      # cyclic buffer length; the tones are bin 128 and bin 256
TONES = {'TX1': 128, 'TX2': 256}
AMP = 0.5 * 32767


def gen(path):
    out = bytearray()
    for n in range(N):
        a1 = 2 * math.pi * TONES['TX1'] * n / N
        a2 = 2 * math.pi * TONES['TX2'] * n / N
        out += struct.pack('<4h', int(AMP * math.cos(a1)), int(AMP * math.sin(a1)),
                           int(AMP * math.cos(a2)), int(AMP * math.sin(a2)))
    open(path, 'wb').write(out)


def bin_power(x, k):
    w = cmath.exp(-2j * math.pi * k / N)
    acc, p = 0j, 1 + 0j
    for v in x:
        acc += v * p
        p *= w
    return abs(acc) ** 2


def check(path):
    d = open(path, 'rb').read()
    n = len(d) // 8
    s = struct.unpack('<%dh' % (4 * n), d[:8 * n])
    blocks = min(8, n // N)
    probe = {'+fs/32': 128, '-fs/32': N - 128, '+fs/16': 256, '-fs/16': N - 256}
    noise_bins = [37, 501, 777, 1500, 2900, 3333]
    ok = True
    for ch, (iq, want) in {'RX1': (0, '+fs/32'), 'RX2': (2, '+fs/16')}.items():
        acc = {k: 0.0 for k in list(probe) + ['noise']}
        for b in range(blocks):
            x = [complex(s[4 * i + iq], s[4 * i + iq + 1]) for i in range(b * N, (b + 1) * N)]
            for name, k in probe.items():
                acc[name] += bin_power(x, k)
            acc['noise'] += sum(bin_power(x, k) for k in noise_bins) / len(noise_bins)
        db = {k: 10 * math.log10(acc[k] / acc['noise'] + 1e-12) for k in probe}
        best = max(db, key=db.get)
        good = best == want and db[want] > 40
        ok &= good
        print('%s: tone %s expected, strongest %s; dB over noise: %s -> %s' % (
            ch, want, best, ', '.join('%s %.1f' % (k, v) for k, v in db.items()), 'PASS' if good else 'FAIL'))
    return ok


if __name__ == '__main__':
    if sys.argv[1] == 'gen':
        gen(sys.argv[2])
    else:
        sys.exit(0 if check(sys.argv[2]) else 1)
