# DivaOnline

Aime card and ALL.Net support for Hatsune Miku: Project DIVA Arcade Future Tone (diva.exe
7.10, not Mega Mix+), as a PD Loader plugin. It lets the game log in with a card and play
on an [ARTEMiS](https://gitea.tendokyu.moe/Hay1tsme/artemis) server, without starting the
game through segatools' `inject.exe`.

It's segatools' `divahook` (the
[TeamTofuShop fork](https://gitea.tendokyu.moe/TeamTofuShop/segatools), `games/divahook`)
with the parts PD Loader already handles taken out. The emulation code is segatools' own,
in `third_party/`, unchanged apart from one line of scope (below).

| segatools divahook | DivaOnline |
|---|---|
| keychip (`nusec`), DNS, netenv, clock, PCB id, hwmon, hwreset, misc | same |
| AMEX DS, EEPROM, SRAM | same |
| AMEX GPIO | off by default (`[gpio] enable=1` turns it on) |
| Aime reader on COM10 (TN32MSEC003S) | same, card number from the .ini |
| JVS I/O, touch slider, Elo touch screen, printer, graphics | not included: PD Loader's TLAC and DivaLinux do these |
| amvideo, vfs | not included: diva.exe doesn't load amvideo.dll, and PD Loader already moves the game's drive paths into the game folder |

## Install

1. Put `DivaOnline.dva` and `DivaOnline.ini` in the game's `plugins` folder.
2. In `DivaOnline.ini`, set your card and server:
   ```ini
   [aime]
   accessCode=5012 3456 7890 1234 5678

   [dns]
   default=artemis.example.net
   ```
3. Start the game as usual (PD Loader, not `divahook.bat`). On the card screen, hold
   **Insert** to tap the card. `[aime] autoInsert=1` keeps the card on the reader
   instead, for controller-only setups.

If you play on a community server, use the keychip serial it gives you (`[keychip] id`).

PD Loader's `[patches] quick_start` skips the card screen; DivaOnline puts the screen back
(`[divaonline] undo_quick_start=1`), so it doesn't need turning off. Leave the launcher's
"Use divahook.bat" off.

Messages start with `[DivaOnline]` in PD Loader's console. `[divaonline] log_file=DivaOnline.log`
writes them to a file too.

## The card

The game reads the card through an emulated Aime reader on COM10, the same one segatools
emulates. Where the card number comes from:

1. `[aime] accessCode`: a 20-digit access code, sent as a MIFARE Aime card.
2. `[aime] felicaIdm`: 16 hex digits, sent as a FeliCa card (when there's no access code).
3. segatools' files, `DEVICE\aime.txt` / `DEVICE\felica.txt` (`aimePath`, `felicaPath`,
   `felicaGen`), when neither is set.
4. `[aimeio] path=some_aimeio.dll`: a segatools aimeio DLL, for a real reader. It replaces
   all of the above and reads its own settings, usually from `segatools.ini` in the game
   folder.

The scan key only counts while the game's window is in front. Its default is Insert rather
than segatools' Enter, because TLAC uses Enter for START.

## Settings

`DivaOnline.ini` lists every setting with a comment. The sections and keys are
segatools.ini's (`[keychip]`, `[dns]`, `[netenv]`, `[clock]`, `[ds]`, `[eeprom]`, `[sram]`,
`[aime]`, `[aimeio]`, ...), so a working `segatools.ini` can be pasted in. Two defaults
differ when a key is missing: `[netenv] enable` is on, `[gpio] enable` is off.

DivaOnline's own section:

| Key | Default | |
|---|---|---|
| `enable` | 1 | 0 turns the plugin off |
| `undo_quick_start` | 1 | Undo PD Loader's `quick_start` patch, which skips the card screen |
| `undo_network_patches` | 0 | Undo the three startup network check patches PD Loader always applies (skip part of the check, no DHCP wait, ignore the location server result). Players have gone online through segatools with them in place, so they're kept unless the check fails |
| `preload_divalinux` | 1 | Load `DivaLinux.dva` before hooking (see below) |
| `log_file` | (none) | Also write the log to this file |

The patches are undone when the game calls `glutInit`, after every plugin (including
`Patches.dva`) has loaded and before the game's startup checks run. A site is only
changed when it holds exactly the bytes PD Loader writes there.

## How it fits with the other plugins

segatools hooks the imports of every module in the process, because it runs before
anything else is loaded. A PD Loader plugin loads after the other plugins have hooked
diva.exe themselves, so DivaOnline only hooks diva.exe's own imports
(`hook_table_exe_only` in `third_party/capnhook/hook/table.c`, the one change to the
vendored code). ALL.Net, the Aime reader task and the keychip checks are all linked into
diva.exe, so nothing is missed.

Hooks chain onto whatever is in an import when DivaOnline installs, so they go on top of
DivaLinux's file layer. DivaLinux, in turn, leaves imports another plugin already hooked
alone, and without CreateFile/ReadFile it can't read `rom.zip` and the `mdata` zips. So
DivaOnline loads `DivaLinux.dva` first if it's in the folder and enabled in `config.ini`
(PD Loader then skips it; it has no `InitializeDVA` to miss). PD Loader's own directory
order would usually do the same.

## Building

With [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), like segatools and LauncherFS
(the plugin is plain C and shares nothing with the game's MSVC runtime, so it isn't in
`AFTMods.sln`):

```sh
LLVM_MINGW=/path/to/llvm-mingw src/DivaOnline/build-mingw.sh   # -> build/win/DivaOnline.dva
```

`tests/smoke.c` loads the plugin the way PD Loader does and checks the keychip registry
values, the ALL.Net host names and a card read from COM10. It runs under Wine:

```sh
$LLVM_MINGW/bin/x86_64-w64-mingw32-clang -O1 -o smoke.exe src/DivaOnline/tests/smoke.c -lws2_32
# smoke.exe next to plugins/DivaOnline.dva and the plugins/DivaOnline.ini from tests/smoke.ini
wine smoke.exe
```

`tests/fixups_test.c` maps the real diva.exe 7.10 next to it (without running it), writes
PD Loader's quick start and network check patches, and checks DivaOnline puts the original
bytes back.

## Licenses

- segatools: public domain (Unlicense), `third_party/segatools/LICENSE`
- capnhook: MIT, `third_party/capnhook/LICENSE`

The vendored files are from upstream segatools (djhackersdev, `ca9c72d`) and capnhook
(`69f7e3b`), which the TeamTofuShop fork grew from; upstream is the copy that can be cloned
from GitHub. The fork's divahook adds the touch screen, printer and graphics hooks, which
DivaOnline leaves out anyway, and passes a reader generation to `sg_reader_hook_init`
(Gen 1 for Diva). Upstream's reader is that Gen 1 reader (it reports `TN32MSEC003S F/W
Ver1.2`). Newer fixes in the fork's keychip, DNS or netenv code aren't in this copy.
