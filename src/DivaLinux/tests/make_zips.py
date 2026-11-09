# Zips for units_test.cpp's zip_index checks (see README.md).
import os, struct, sys, zipfile
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def stored(path, entries):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
        for name, data in entries:
            z.writestr(zipfile.ZipInfo(name), data)

# Names the index keeps (separators turned around, ".\" dropped) and ones it leaves out.
stored(os.path.join(out, 'stored.zip'), [
    ('rom/a.bin', b'0123456789'),
    ('rom/sub/', b''),
    ('rom/sub/b.txt', b'abc'),
    ('./c.txt', b'see'),
    ('../evil.txt', b'x'),
    ('/abs.txt', b'x'),
    ('C:/drive.txt', b'x'),
    ('rom/\u30df\u30af.txt', b'utf8'),
])

# More than 65535 entries: the end record's count overflows and zip64 records are written.
with zipfile.ZipFile(os.path.join(out, 'zip64.zip'), 'w', zipfile.ZIP_STORED) as z:
    for i in range(65540):
        z.writestr(zipfile.ZipInfo('f/%05d' % i), b'%d' % i)

with zipfile.ZipFile(os.path.join(out, 'deflated.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('rom/packed.bin', b'a' * 1000)

# The encrypted flag set on a stored member (central directory and local header).
data = bytearray(open(os.path.join(out, 'stored.zip'), 'rb').read())
cd = data.find(b'PK\x01\x02')
data[cd + 8] |= 1
open(os.path.join(out, 'encrypted.zip'), 'wb').write(data)

# The central directory cut short: the end record still claims all of it.
data = bytearray(open(os.path.join(out, 'stored.zip'), 'rb').read())
eocd = data.rfind(b'PK\x05\x06')
cd_size, cd_off = struct.unpack_from('<II', data, eocd + 12)
struct.pack_into('<H', data, eocd + 10, 99)        # more entries than there are
struct.pack_into('<H', data, eocd + 8, 99)
open(os.path.join(out, 'broken.zip'), 'wb').write(data)

open(os.path.join(out, 'notzip.bin'), 'wb').write(os.urandom(5000).replace(b'PK', b'pk'))
open(os.path.join(out, 'tiny.bin'), 'wb').write(b'PK\x05\x06')
