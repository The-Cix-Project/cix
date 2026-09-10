#!/usr/bin/env python3
"""Render an SVG to Unicode half-block art by actually sampling the shape.

No PIL in this sandbox, so the PNG is decoded here: PNG is just zlib-compressed
scanlines with a per-line filter byte, which is ~50 lines to undo.
"""
import zlib, struct, subprocess, sys

def png_decode(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
    pos, idat, w = 8, b'', None
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ = d[pos+4:pos+8]
        body = d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8, "need 8-bit, got %d" % depth
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    nch = {0:1, 2:3, 4:2, 6:4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-nch] if i >= nch else 0
            b = prev[i]
            c = prev[i-nch] if i >= nch else 0
            if f == 1:   line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a+b)//2) & 255
            elif f == 4:
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out.append(bytes(line)); prev = line
    return w, h, nch, out

def coverage(path):
    """Per-pixel ink coverage 0..1 (alpha if present, else darkness)."""
    w, h, nch, rows = png_decode(path)
    cov = []
    for r in rows:
        line = []
        for x in range(w):
            px = r[x*nch:(x+1)*nch]
            if nch == 4:   line.append(px[3] / 255.0)
            elif nch == 2: line.append(px[1] / 255.0)
            elif nch == 3: line.append(1.0 - (px[0]*0.299 + px[1]*0.587 + px[2]*0.114) / 255.0)
            else:          line.append(1.0 - px[0] / 255.0)
        cov.append(line)
    return w, h, cov

def render(svg, cols, aspect, ss=8, thresh=0.42):
    """cols text columns; aspect = image h/w; ss = supersample factor."""
    rows_px = max(2, int(round(cols * aspect)))   # square px; 2 px rows per text row
    if rows_px % 2: rows_px += 1
    W, H = cols * ss, rows_px * ss
    png = '/tmp/_svg2blocks.png'
    subprocess.run(['rsvg-convert', '-w', str(W), '-h', str(H), svg, '-o', png], check=True)
    w, h, cov = coverage(png)
    # average each ss x ss block down to one sample
    grid = []
    for gy in range(rows_px):
        line = []
        for gx in range(cols):
            tot = n = 0.0
            for yy in range(gy*ss, min((gy+1)*ss, h)):
                for xx in range(gx*ss, min((gx+1)*ss, w)):
                    tot += cov[yy][xx]; n += 1
            line.append(tot / n if n else 0.0)
        grid.append(line)
    art = []
    for ty in range(0, rows_px, 2):
        s = ''
        for x in range(cols):
            up = grid[ty][x] >= thresh
            lo = grid[ty+1][x] >= thresh if ty+1 < rows_px else False
            s += '█' if (up and lo) else ('▀' if up else ('▄' if lo else ' '))
        art.append(s.rstrip())
    while art and not art[0].strip(): art.pop(0)
    while art and not art[-1].strip(): art.pop()
    return art

if __name__ == '__main__':
    svg = sys.argv[1]; cols = int(sys.argv[2]); aspect = float(sys.argv[3])
    th = float(sys.argv[4]) if len(sys.argv) > 4 else 0.42
    for l in render(svg, cols, aspect, thresh=th): print(l)
