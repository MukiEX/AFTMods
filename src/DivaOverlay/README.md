# DivaOverlay

An in-game settings menu, built into DivaLinux.dva. It opens over the running game, in the
style of the game's Key Config screen, and edits three files in one place:

| Group | File | What's in it |
|---|---|---|
| DivaLinux | `plugins\DivaLinux.ini` | Stereo 3D, VR, movies, the pointer, reflections, performance, loading |
| Game | `plugins\config.ini` (and `components.ini`, `playerdata.ini`, `keyconfig.ini`) | PD Launcher's pages, with the launcher's keys, defaults and help (from LauncherFS). On Android, only what the runtime reads |
| Android | `divarecomp_env.txt` | The DivaRecomp runtime's switches (only under the runtime) |

Each group's sections are buttons on the home page; long sections have a button per
sub-section. Settings marked *Applies right away* change the running game (stereo 3D through
DivaGL's `SetOptionInt`, reflections, the pointer); the rest are saved and wait for a restart,
marked with an orange dot, and **Restart game** on the home page (or the orange reminder at the
bottom right) restarts the game with them.

Switches in `divarecomp_env.txt` are commented out rather than deleted when set back to
*Default*, so the file keeps its notes and values.

## Opening it

| Where | Open / close | Move | Change | Select | Back |
|---|---|---|---|---|---|
| Windows / Wine | F10, or View + Start | arrows, D-pad, left stick | left / right | Enter, A | Esc, B |
| Android (gamepad) | Select + Start | D-pad, left stick | left / right | A | B, Back |
| Android (touch) | hold the top right corner for a second | tap and drag | tap the arrows | tap | the arrow by the title |
| Quest | the left controller's menu button | point | point at the arrows | A/X or a trigger | B/Y |

Mouse and touch: tap a row to open it or switch it, tap the arrows on its value box to step
it, drag to scroll, the X at the top right closes the menu.

While the menu is open the game doesn't see your input (the song keeps playing). On Windows
its window gets no keyboard or mouse messages, and `GetAsyncKeyState`, `GetKeyState`,
`GetKeyboardState` and `XInputGetState` report nothing pressed to callers outside DivaLinux
until everything is released after closing. Under the runtime, the runtime holds gamepad,
slider controller and touch panel input back.

`[overlay]` in `DivaLinux.ini`:

```ini
[overlay]
enabled=1   ; 0 turns the menu off
key=121     ; Windows virtual-key code to open it (121 = F10)
```

With the launcher's own in-game menu on (`Launcher.dva`, `[launcher] overlay=1`), the launcher
gets the game window and DivaOverlay stays off.

## How it's drawn

The menu is Dear ImGui draw lists (no ImGui windows), rasterised on the CPU by LauncherFS's
renderer at most 1080 lines tall, and only when it changes. DivaLinux blends it over each frame
before it's presented (`src/DivaLinux/overlay.cpp`), in both halves in side-by-side 3D. The font
is M PLUS Rounded 1c (SIL Open Font License, `../LauncherFS/res/OFL.txt`), subset to Latin by
`tools/make_fonts.py` into `res/ov_fonts.cpp`.

## Files

- `ov_model.*`: the settings: what each section lists, how values read and write.
- `env_file.*`: `divarecomp_env.txt` editing.
- `ov_ui.*`: the menu.
- `ov_host.cpp`: DivaLinux's side: opening, input, live changes, restarting.
- `tools/preview.cpp`: a Linux tool that draws the menu to PNGs with sample files
  (`cmake -S src/DivaOverlay -B build-ovpreview && cmake --build build-ovpreview`, then
  `build-ovpreview/ov_preview out/ [background.rgba W H] [--xr] [--windows]`).

The runtime's side (input, the shared struct, restarting the app) is in DivaDroid's
`runtime/src/overlay_io.c`.
