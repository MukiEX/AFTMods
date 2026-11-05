# Builds an original game tree and a "zipped" one standing in for it (see test.cpp).
import os, random, shutil, sys, zipfile
random.seed(7)
orig, zipped = sys.argv[1], sys.argv[2]
for d in (orig, zipped):
    shutil.rmtree(d, ignore_errors=True)

def write(path, size):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(bytes(random.getrandbits(8) for _ in range(size)) if size < 4096 else os.urandom(size))

def tree(root, n, sub):
    for i in range(n):
        d = random.choice(sub)
        write(os.path.join(root, d, f"{random.choice(['obj','Tex','SPR_','pv_'])}{i:03d}.{random.choice(['bin','farc','txt','DSC'])}"),
              random.choice([0, 1, 17, 4095, 4096, 4097, 65536, random.randint(1, 300000)]))

tree(os.path.join(orig, 'rom'), 250, ['objset', 'script', '2d', 'Light_Param', 'deep/a/b/c', '.'])
write(os.path.join(orig, 'rom', 'pv_db.txt'), 1234)
packs = ['MZKO', 'MPFZ', 'M220', 'MBTH', 'MCMP', 'MLOOSE']
for p in packs:
    tree(os.path.join(orig, 'mdata', p, 'rom'), 60, ['objset', 'Skin_Param', 'script', '.'])
os.makedirs(os.path.join(orig, 'mdata', 'MZKO', 'rom', 'empty'))

def zip_dir(src, dst, prefix='', method=zipfile.ZIP_STORED, force64=False, dir_entries=False):
    with zipfile.ZipFile(dst, 'w', method, allowZip64=True) as z:
        if dir_entries:   # what 7-Zip writes: an entry for each folder of the prefix
            parts = prefix.strip('/').split('/')
            for i in range(len(parts)):
                z.writestr(zipfile.ZipInfo('/'.join(parts[:i + 1]) + '/'), b'')
        for base, dirs, files in os.walk(src):
            rel = os.path.relpath(base, src).replace(os.sep, '/')
            rel = '' if rel == '.' else rel + '/'
            if not files and not dirs:
                z.writestr(zipfile.ZipInfo(prefix + rel), b'')   # explicit empty folder
            for f in sorted(files):
                info = zipfile.ZipInfo.from_file(os.path.join(base, f), prefix + rel + f)
                info.compress_type = method
                with open(os.path.join(base, f), 'rb') as fh, z.open(info, 'w', force_zip64=force64) as out:
                    shutil.copyfileobj(fh, out)

os.makedirs(os.path.join(zipped, 'mdata'))
zip_dir(os.path.join(orig, 'rom'), os.path.join(zipped, 'rom.zip'), prefix='rom/', dir_entries=True)
zip_dir(os.path.join(orig, 'mdata', 'MZKO'), os.path.join(zipped, 'mdata', 'MZKO.zip'), prefix='mdata/MZKO/', dir_entries=True)
zip_dir(os.path.join(orig, 'mdata', 'MPFZ'), os.path.join(zipped, 'mdata', 'MPFZ.zip'), prefix='MPFZ/')
zip_dir(os.path.join(orig, 'mdata', 'M220'), os.path.join(zipped, 'mdata', 'm220.zip'), force64=True)
zip_dir(os.path.join(orig, 'mdata', 'MBTH'), os.path.join(zipped, 'mdata', 'MBTH.zip'), prefix='mdata/MBTH/')
shutil.copytree(os.path.join(orig, 'mdata', 'MBTH'), os.path.join(zipped, 'mdata', 'MBTH'))
for base, _, files in os.walk(os.path.join(zipped, 'mdata', 'MBTH')):   # stale folder: the zip must win
    for f in files:
        open(os.path.join(base, f), 'wb').write(b'STALE')
zip_dir(os.path.join(orig, 'mdata', 'MCMP'), os.path.join(zipped, 'mdata', 'MCMP.zip'), method=zipfile.ZIP_DEFLATED)
shutil.copytree(os.path.join(orig, 'mdata', 'MCMP'), os.path.join(zipped, 'mdata', 'MCMP'))  # compressed zip -> folder used
shutil.copytree(os.path.join(orig, 'mdata', 'MLOOSE'), os.path.join(zipped, 'mdata', 'MLOOSE'))
n = sum(len(f) for _, _, f in os.walk(orig))
print(f"original: {n} files; zipped root has {sum(len(f) for _, _, f in os.walk(zipped))} files")
