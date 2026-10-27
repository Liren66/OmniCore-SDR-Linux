"""IIO data-integrity helper (AD9361 digital loopback, bit-exact path).

  gen   <file>                 cyclic TX buffer, int16 I1,Q1,I2,Q2 per sample: four
                               12-bit counters (MSB-aligned) with different steps
  check <capture> <block> [nch]
                               RX capture (int16, 12-bit LSB-aligned; nch = 4 for I1,Q1,I2,Q2
                               or 2 for I1,Q1): inside every <block>-sample DMA block each
                               channel must advance by its step mod 4096. Jumps at block
                               boundaries are not errors (the DMA overruns between blocks when
                               the client is slower than the sample rate) but are counted as
                               gaps: 0 gaps = the stream kept up with the sample rate.
  gaps  <capture> <block> [nch] like check, but reads only the samples around the block
                               boundaries (fast, for long captures)
"""
import sys
from array import array

N = 4096                                # cyclic TX length = counter period
STEPS = (1, 3, 5, 7)                    # per channel: I1, Q1, I2, Q2


def gen(path):
    out = array('h')
    for n in range(N):
        for st in STEPS:
            v = (n * st) % 4096 - 2048      # signed 12-bit
            out.append(v << 4)              # DAC takes the 12 MSBs of 16
    out.tofile(open(path, 'wb'))


def check(path, block, nch=4):
    d = array('h'); d.frombytes(open(path, 'rb').read())
    n = len(d) // nch
    bad = gaps = 0
    first_bad = None
    for c, st in enumerate(STEPS[:nch]):
        x = d[c::nch]
        for k in range(1, n):
            if (x[k] - x[k - 1] - st) % 4096:
                if k % block == 0:
                    gaps += c == 0
                    continue
                bad += 1
                if first_bad is None:
                    first_bad = (c, k, x[k - 1], x[k])
    blocks = (n + block - 1) // block
    print('samples=%d blocks=%d step errors=%d gaps=%d%s' % (
        n, blocks, bad, gaps, '' if first_bad is None else ' first (ch, k, prev, cur)=%s' % (first_bad,)))
    return bad == 0


def gaps(path, block, nch=4):
    import os
    size = os.path.getsize(path)
    n = size // (2 * nch)
    g = 0
    with open(path, 'rb') as f:
        for k in range(block, n, block):
            f.seek((k - 1) * 2 * nch)
            a = array('h'); a.frombytes(f.read(4 * nch))
            g += (a[nch] - a[0] - STEPS[0]) % 4096 != 0
    print('samples=%d blocks=%d gaps=%d' % (n, (n + block - 1) // block, g))
    return g == 0


if __name__ == '__main__':
    if sys.argv[1] == 'gen':
        gen(sys.argv[2])
    elif sys.argv[1] == 'gaps':
        sys.exit(0 if gaps(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 4) else 1)
    else:
        sys.exit(0 if check(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 4) else 1)
