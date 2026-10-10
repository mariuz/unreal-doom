"""Write glBSP-style GL node test PWADs (GL_E1M1 + GL_VERT/GL_SEGS/GL_SSECT/GL_NODES)
in versions 1, 2, 3 and 5, re-encoding the GL nodes of an XGLN test WAD (made by
castle-doom's tools/make_znodes.py). The normal NODES/SEGS/SSECTORS are emptied,
so the port must use the GL nodes. Each must give the same subsector area.

Usage: python make_gl_wads.py Tests/wads/e1m1_xgln.wad Tests/wads
"""
import os
import struct
import sys


def read_wad(path):
    d = open(path, 'rb').read()
    _, n, off = struct.unpack('<4sii', d[:12])
    out = []
    for i in range(n):
        p, s, name = struct.unpack('<ii8s', d[off + 16 * i:off + 16 * i + 16])
        out.append((name.rstrip(b'\0').decode(), d[p:p + s]))
    return out


def write_wad(path, lumps):
    data, directory, pos = b'', b'', 12
    for name, blob in lumps:
        directory += struct.pack('<ii8s', pos, len(blob), name.encode())
        data += blob
        pos += len(blob)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sii', b'PWAD', len(lumps), pos) + data + directory)


def parse_xgln(blob):
    assert blob[:4] == b'XGLN'
    p = 4
    org, new = struct.unpack_from('<II', blob, p); p += 8
    verts = [struct.unpack_from('<ii', blob, p + 8 * i) for i in range(new)]; p += 8 * new
    nsub, = struct.unpack_from('<I', blob, p); p += 4
    counts = [struct.unpack_from('<I', blob, p + 4 * i)[0] for i in range(nsub)]; p += 4 * nsub
    nseg, = struct.unpack_from('<I', blob, p); p += 4
    segs = []
    for i in range(nseg):
        v1, partner, line, side = struct.unpack_from('<IIHB', blob, p); p += 11
        segs.append((v1, line, side))
    nnode, = struct.unpack_from('<I', blob, p); p += 4
    nodes = []
    for i in range(nnode):
        nodes.append(struct.unpack_from('<4h8hII', blob, p)); p += 32
    return org, verts, counts, segs, nodes


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    lumps = read_wad(src)
    names = [n for n, _ in lumps]
    org, verts, counts, segs, nodes = parse_xgln(dict(lumps)['SSECTORS'])
    level = [(n, b'' if n in ('NODES', 'SEGS', 'SSECTORS') else b) for n, b in lumps[names.index('E1M1'):names.index('E1M1') + 11]]

    # closed subsectors: each seg ends where the next one in its subsector starts
    full = []
    first = 0
    for c in counts:
        for k in range(c):
            v1, line, side = segs[first + k]
            v2 = segs[first + (k + 1) % c][0]
            full.append((v1, v2, line, side))
        first += c

    for version in (1, 2, 3, 5):
        flag = {1: 0x8000, 2: 0x8000, 3: 0x40000000, 5: 0x80000000}[version]
        ref = lambda v: (v - org) | flag if v >= org else v
        if version == 1:
            gl_vert = b''.join(struct.pack('<hh', round(x / 65536), round(y / 65536)) for x, y in verts)
        else:
            gl_vert = (b'gNd2' if version == 2 else b'gNd3' if version == 3 else b'gNd5') + b''.join(struct.pack('<ii', x, y) for x, y in verts)
        if version <= 2:
            gl_segs = b''.join(struct.pack('<HHHHH', ref(a), ref(b), line, side, 0xFFFF) for a, b, line, side in full)
            gl_ssect = b''
            first = 0
            for c in counts:
                gl_ssect += struct.pack('<HH', c, first); first += c
        else:
            gl_segs = (b'gNd3' if version == 3 else b'') + b''.join(
                struct.pack('<IIHHI', ref(a), ref(b), line, side, 0xFFFFFFFF) for a, b, line, side in full)
            gl_ssect = b'gNd3' if version == 3 else b''
            first = 0
            for c in counts:
                gl_ssect += struct.pack('<II', c, first); first += c
        gl_nodes = b''
        for nd in nodes:
            c0, c1 = nd[12], nd[13]
            if version == 5:
                gl_nodes += struct.pack('<4h8hII', *nd[:12], c0, c1)
            else:
                to16 = lambda c: (c & 0x7FFF) | 0x8000 if c & 0x80000000 else c
                gl_nodes += struct.pack('<4h8hHH', *nd[:12], to16(c0), to16(c1))
        out = level + [('GL_E1M1', b''), ('GL_VERT', gl_vert), ('GL_SEGS', gl_segs), ('GL_SSECT', gl_ssect), ('GL_NODES', gl_nodes)]
        path = os.path.join(outdir, 'e1m1_gl_v%d.wad' % version)
        write_wad(path, out)
        print('wrote', path, len(full), 'segs', len(counts), 'subsectors')


if __name__ == '__main__':
    main()
