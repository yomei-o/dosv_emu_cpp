# -*- coding: utf-8 -*-
"""Where the green range frame is on a screen.

    python tools/greenbox.py tmp/plot/with.raw tmp/plot/without.raw

入出力 → ②ﾌﾟﾛｯﾀ draws the area it is about to plot as a green rectangle.
A visitor said it was enormous, and "enormous" is not something to argue
about from a description -- this prints where its edges actually are.
"""
import sys

W, H = 640, 480
GREEN = (0, 255, 0)

def box(path):
    d = open(path, 'rb').read()
    per = len(d) // (W * H)
    xs, ys = [], []
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * per
            if (d[i], d[i + 1], d[i + 2]) == GREEN:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys), len(xs)

for path in sys.argv[1:]:
    b = box(path)
    if not b:
        print('%-28s no green at all' % path)
        continue
    print('%-28s x %3d..%3d  y %3d..%3d  (%d x %d)  %d pixels'
          % (path, b[0], b[2], b[1], b[3], b[2] - b[0] + 1, b[3] - b[1] + 1, b[4]))
