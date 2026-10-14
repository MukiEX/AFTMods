# AFTMods
Various mods for Hatsune Miku: Project DIVA Mega Mix+

## DivaGL
Ports graphics of PDAFT 7.10 from legacy OpenGL to modern one. Works on Nvidia GPUs as well as on AMD and Intel GPUs (last statement needs testing).

The use of DivaGL and/or its code and/or its shaders and the author of this plugin must be acknowledged in content created using anything above.

### Stereoscopic 3D (Side-by-Side)
Optional SBS stereo output, configured in the `[stereo]` section of `DivaGL.ini` (or the PD Loader config UI):

```ini
[stereo]
stereo_mode=1           ; 0 = off, 1 = half SBS (3D TVs, XR glasses), 2 = full SBS (letterboxed, cross-eye/VR players)
stereo_separation=65    ; eye separation in mm
stereo_convergence=0    ; zero-parallax distance in cm, 0 = auto (camera interest point, usually the singer)
stereo_swap_eyes=0      ; right eye on the left (cross-eyed viewing)
stereo_hud_parallax=0   ; HUD depth in 0.1% of image width, + = behind screen
stereo_bg_infinity=1    ; draw 2D backgrounds / PV movies at infinite depth
```

The whole pipeline (shadows, SSS, reflections, 3D, post-process, sprites) is rendered once per eye using parallel off-axis cameras, so GPU cost roughly doubles. TAA is disabled while stereo is on because its history would mix the two eyes.

## OPD Play Gen
Generates OPD files on demand if not present. Also allows files to be cached. It only generates cache for missing motions or modules. If FARC with OPD is present, and OPD file inside that FARC is present too, it won't do anything and will load existing data.

## Rob Tex Anim
Allows usage of Auth3D Object's Texture Transform and Texture Pattern in Rob

## Sprite Draw Limit
Breaks limit for Sprite Draw. (Prevents displaying of mess on screen if too many sprites are shown)
