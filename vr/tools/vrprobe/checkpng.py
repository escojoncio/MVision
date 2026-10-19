"""Valida un PNG RGB8 del probador: firma, CRC de cada trozo y flujo zlib. Uso: checkpng.py fichero.png [ancho alto]"""
import struct
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n', 'firma'
i, idat, w, h = 8, b'', 0, 0
while i < len(d):
    n, t = struct.unpack('>I4s', d[i:i + 8])
    c = d[i + 8:i + 8 + n]
    assert zlib.crc32(t + c) & 0xffffffff == struct.unpack('>I', d[i + 8 + n:i + 12 + n])[0], 'crc ' + t.decode()
    if t == b'IHDR':
        w, h = struct.unpack('>II', c[:8])
    if t == b'IDAT':
        idat += c
    i += 12 + n
raw = zlib.decompress(idat)
assert len(raw) == h * (1 + w * 3), 'tamaño'
if len(sys.argv) > 3:
    assert (w, h) == (int(sys.argv[2]), int(sys.argv[3])), 'dimensiones'
print('PNG correcto: %dx%d' % (w, h))
