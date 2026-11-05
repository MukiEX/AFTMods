#!/usr/bin/env python3
"""Packs a Project DIVA Arcade Future Tone game folder's rom and mdata packs into the
uncompressed zips DivaLinux reads in their place ([loading] archives=1).

    python divalinux_pack.py H:\\Games\\PDAFT                 -> H:\\Games\\PDAFT_packed\\
    python divalinux_pack.py H:\\Games\\PDAFT --out E:\\quest  -> E:\\quest\\
    python divalinux_pack.py H:\\Games\\PDAFT --packs MZKO MPFZ (only those mdata packs)

The output mirrors the game folder: rom.zip next to diva.exe and mdata\\<pack>.zip for each
pack. Copy them into the game folder on the device; with an archive present, DivaLinux reads
it instead of the folder of the same name (which can then be deleted there).

Each zip carries its place in the game folder (rom.zip holds rom/..., MZKO.zip holds
mdata/MZKO/...), so "extract here" in the game folder rebuilds the original folders.

Files are stored, not compressed: DivaLinux reads a member as a plain range of the zip, and
the game's data (FARC archives, textures, movies) is compressed already. A zip is only
rewritten when its folder changed since it was made (--force rewrites everything).
Needs Python 3.8 or later; nothing else.
"""

import argparse
import os
import sys
import time
import zipfile


def files_in(folder):
    """Every file and folder under `folder`, as (relative path with '/', is_dir), sorted."""
    out = []
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        rel = os.path.relpath(base, folder).replace(os.sep, '/')
        rel = '' if rel == '.' else rel + '/'
        if rel:
            out.append((rel, True))
        for f in sorted(files):
            out.append((rel + f, False))
    return out


def up_to_date(zip_path, folder, prefix, entries):
    """True if the zip already holds exactly these files, made after the newest of them."""
    if not os.path.exists(zip_path):
        return False
    try:
        with zipfile.ZipFile(zip_path) as z:
            have = {i.filename: i.file_size for i in z.infolist() if not i.is_dir()}
            if any(i.compress_type != zipfile.ZIP_STORED for i in z.infolist()):
                return False
    except zipfile.BadZipFile:
        return False
    want = {}
    newest = 0.0
    for rel, is_dir in entries:
        if is_dir:
            continue
        st = os.stat(os.path.join(folder, rel))
        want[prefix + rel] = st.st_size
        newest = max(newest, st.st_mtime)
    return have == want and os.path.getmtime(zip_path) >= newest


def pack(folder, zip_path, prefix, force, verify):
    entries = files_in(folder)
    count = sum(1 for _, d in entries if not d)
    if not force and up_to_date(zip_path, folder, prefix, entries):
        print(f"  {os.path.basename(zip_path)}: up to date ({count} files)")
        return
    os.makedirs(os.path.dirname(zip_path), exist_ok=True)
    tmp = zip_path + '.tmp'
    started = time.time()
    total = 0
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
        # The prefix's own folders first, so "extract here" makes them even if empty
        parts = prefix.strip('/').split('/')
        for i in range(len(parts)):
            z.writestr(zipfile.ZipInfo('/'.join(parts[:i + 1]) + '/'), b'')
        for n, (rel, is_dir) in enumerate(entries, 1):
            path = os.path.join(folder, rel)
            if is_dir:
                z.write(path, prefix + rel)
                continue
            info = zipfile.ZipInfo.from_file(path, prefix + rel)
            info.compress_type = zipfile.ZIP_STORED
            size = os.path.getsize(path)
            # force_zip64 for members over 4 GB (movies can get close)
            with open(path, 'rb') as src, z.open(info, 'w', force_zip64=size >= 0xFFFFFFFF) as dst:
                while True:
                    chunk = src.read(8 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
            total += size
            if n % 500 == 0:
                print(f"    {n}/{len(entries)}", end='\r', flush=True)
    if verify:
        with zipfile.ZipFile(tmp) as z:
            bad = z.testzip()
            if bad:
                os.remove(tmp)
                sys.exit(f"  {os.path.basename(zip_path)}: {bad} failed its check; nothing written")
    os.replace(tmp, zip_path)
    secs = time.time() - started
    print(f"  {os.path.basename(zip_path)}: {count} files, {total / 1048576:.0f} MB in {secs:.0f} s")


def find_child(parent, name):
    """The entry of `parent` called `name`, ignoring case (game folders mix cases)."""
    if not os.path.isdir(parent):
        return None
    for n in os.listdir(parent):
        if n.lower() == name.lower():
            return n
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', help='the game folder (the one with diva.exe, rom and mdata)')
    ap.add_argument('--out', help='where to write rom.zip and mdata\\ (default: <game>_packed next to it)')
    ap.add_argument('--packs', nargs='*', help='only these mdata packs (default: all)')
    ap.add_argument('--no-rom', action='store_true', help="don't pack rom")
    ap.add_argument('--no-mdata', action='store_true', help="don't pack mdata")
    ap.add_argument('--force', action='store_true', help='rewrite zips even if they are up to date')
    ap.add_argument('--no-verify', action='store_true', help="skip reading each zip back to check it")
    args = ap.parse_args()

    game = os.path.abspath(args.game)
    out = os.path.abspath(args.out) if args.out else game.rstrip('\\/') + '_packed'
    if os.path.normcase(out) == os.path.normcase(game):
        sys.exit("--out must be a different folder from the game folder")
    verify = not args.no_verify
    print(f"Packing {game} into {out}")

    if not args.no_rom:
        rom = find_child(game, 'rom')
        if rom:
            pack(os.path.join(game, rom), os.path.join(out, 'rom.zip'), 'rom/', args.force, verify)
        else:
            print("  no rom folder")

    if not args.no_mdata:
        mdata = find_child(game, 'mdata')
        if not mdata:
            print("  no mdata folder")
            return
        root = os.path.join(game, mdata)
        packs = sorted(n for n in os.listdir(root) if os.path.isdir(os.path.join(root, n)))
        if args.packs:
            wanted = {p.lower() for p in args.packs}
            missing = wanted - {p.lower() for p in packs}
            if missing:
                print(f"  not found in mdata: {', '.join(sorted(missing))}")
            packs = [p for p in packs if p.lower() in wanted]
        for p in packs:
            pack(os.path.join(root, p), os.path.join(out, 'mdata', p + '.zip'), f'mdata/{p}/', args.force, verify)
        # Loose files in mdata (outside any pack) are left as they are
        loose = [n for n in os.listdir(root) if os.path.isfile(os.path.join(root, n))]
        if loose:
            print(f"  note: {len(loose)} loose file(s) in mdata aren't packed; copy them as they are: {', '.join(loose[:5])}")
    print("Done. Copy the output into the game folder on the device.")


if __name__ == '__main__':
    main()
