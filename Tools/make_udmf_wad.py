"""Write a UDMF test PWAD: Freedoom's E1M1 converted to TEXTMAP (namespace "doom") with the
GL nodes of a ZGL3 test WAD (castle-doom's tools/make_znodes.py) as ZNODES, so the port's
UDMF loader must give the same 682 subsectors and area as the vanilla map.

Usage: python make_udmf_wad.py Content/WADs/freedoom1.wad Tests/wads/e1m1_zgl3.wad Tests/wads/e1m1_udmf.wad
"""
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


def name8(b):
    return b.rstrip(b'\0').decode('ascii', 'replace')


def main():
    iwad, zgl3, out = sys.argv[1:4]
    lumps = read_wad(iwad)
    names = [n for n, _ in lumps]
    m = names.index('E1M1')
    L = dict(lumps[m + 1:m + 11])
    t = ['namespace = "doom";']
    for i in range(0, len(L['VERTEXES']), 4):
        x, y = struct.unpack_from('<hh', L['VERTEXES'], i)
        t.append('vertex { x = %d.0; y = %d.0; }' % (x, y))
    for i in range(0, len(L['SECTORS']), 26):
        f, c, fp, cp, light, special, tag = struct.unpack_from('<hh8s8shhh', L['SECTORS'], i)
        t.append('sector { heightfloor = %d; heightceiling = %d; texturefloor = "%s"; textureceiling = "%s"; lightlevel = %d; special = %d; id = %d; }'
                 % (f, c, name8(fp), name8(cp), light, special, tag))
    for i in range(0, len(L['SIDEDEFS']), 30):
        xo, yo, up, lo, mid, sec = struct.unpack_from('<hh8s8s8sH', L['SIDEDEFS'], i)
        t.append('sidedef { offsetx = %d; offsety = %d; texturetop = "%s"; texturebottom = "%s"; texturemiddle = "%s"; sector = %d; }'
                 % (xo, yo, name8(up), name8(lo), name8(mid), sec))
    flagnames = ['blocking', 'blockmonsters', 'twosided', 'dontpegtop', 'dontpegbottom', 'secret', 'blocksound', 'dontdraw', 'mapped']
    for i in range(0, len(L['LINEDEFS']), 14):
        v1, v2, flags, special, tag, s0, s1 = struct.unpack_from('<HHHhhHH', L['LINEDEFS'], i)
        f = ''.join(' %s = true;' % n for b, n in enumerate(flagnames) if flags & (1 << b))
        back = -1 if s1 == 0xFFFF else s1
        t.append('linedef { v1 = %d; v2 = %d; sidefront = %d; sideback = %d; special = %d; arg0 = %d;%s }' % (v1, v2, s0, back, special, tag, f))
    for i in range(0, len(L['THINGS']), 10):
        x, y, angle, typ, flags = struct.unpack_from('<hhhHH', L['THINGS'], i)
        f = ''
        if flags & 1: f += ' skill1 = true; skill2 = true;'
        if flags & 2: f += ' skill3 = true;'
        if flags & 4: f += ' skill4 = true; skill5 = true;'
        if flags & 8: f += ' ambush = true;'
        if not flags & 16: f += ' single = true;'
        f += ' dm = true; coop = true;'
        t.append('thing { x = %d.0; y = %d.0; angle = %d; type = %d;%s }' % (x, y, angle, typ, f))
    textmap = ('// DoomUE test map converted from Freedoom E1M1\n' + '\n'.join(t) + '\n').encode('ascii')
    znodes = dict(read_wad(zgl3))['SSECTORS']
    write_wad(out, [('E1M1', b''), ('TEXTMAP', textmap), ('ZNODES', znodes), ('ENDMAP', b'')])
    print('wrote', out, len(textmap), 'bytes of TEXTMAP,', len(znodes), 'bytes of', znodes[:4])


if __name__ == '__main__':
    main()
