# File layer tests (Linux)

`units_test.cpp` checks the parts that need no Windows: `../vfs/zip_index` (central
directories, zip64, names that could escape the folder, compressed and encrypted members,
broken and non-zip files, data offsets) and `../vfs/wildcard` (pattern matching):

```
python3 make_zips.py zips
g++ -std=c++17 -O1 -fsanitize=address,undefined units_test.cpp ../vfs/zip_index.cpp ../vfs/wildcard.cpp -o units_test
./units_test zips
```

`vfs_test.cpp` builds the whole file layer (`../vfs/*.cpp` and `../common/*.cpp`, included into
the test) against a small stand-in for the Win32 file API
(`inc/windows.h`, and the fake kernel at the top of the test) that behaves like Wine over Linux
folders: names match regardless of case, and errors are the ones Windows gives. It compares
the file layer reading a "zipped" game folder against the original:

- every file's bytes, read in odd-sized chunks, plus seeks, overlapped reads, sizes, file
  mappings and file info
- every name and folder looked up, plus missing ones (same errors)
- every folder listing, wide and ANSI
- archive layouts: `rom/...`, `mdata/MZKO/...` with folder entries, `MPFZ/...`, bare
  contents with zip64, a stale folder next to its zip (the zip wins), and a compressed zip
  (rejected, folder used)
- write refusal, and eight threads reading at once

```
python3 make_trees.py orig zipped
g++ -std=c++17 -O1 -fsanitize=address,undefined -Wno-unknown-pragmas -Iinc -I.. vfs_test.cpp -o vfs_test
ASAN_OPTIONS=detect_leaks=0 ./vfs_test $PWD/orig $PWD/zipped 1    # last argument: lookup_cache
```

Regenerate the trees before each run (the test writes a file into a loose pack). Archives
stay open for the life of the process by design, hence `detect_leaks=0`. Also run it with
`-fsanitize=thread`.

To test `tools/divalinux_pack.py`, pack `orig` with it and point the test at the output:
`python3 ../../../tools/divalinux_pack.py orig --out packed`, then
`./vfs_test $PWD/orig $PWD/packed 1`. Two checks fail there by design: they write into the
loose pack `MLOOSE`, which the tool has zipped.
