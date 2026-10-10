# Dead Space (2011 mobile) — native PC port

Runs EA/IronMonkey's Android Dead Space on Windows and Linux without an Android emulator.
The game's own 32-bit ARM code (`libDeadSpace.so`) runs on the dynarmic JIT; everything it
expects from Android (libc, pthreads, OpenGL ES 1.1, JNI/Java, audio, assets) is reimplemented
on the host with SDL2 + desktop OpenGL.

No game data is included: you need your own Dead Space APK.

## Running

```
deadspace --apk <path to Dead Space APK>      # first run: imports the game data into gamedata/
deadspace                                     # afterwards
```

An APK placed next to the executable is imported automatically on first start.

In gameplay the mouse is captured; in menus it works as a touch pointer (moving it hides the keyboard
highlight). The arrows beside //Options at the top of the Options screen turn its pages.

| Input | Action |
|---|---|
| W A S D | move (Shift = run) |
| Mouse | look / aim |
| Right mouse (hold) | raise weapon / aim |
| Left mouse | fire while aiming (hold right mouse); melee when not aiming; launch a kinesis object |
| R | reload |
| F | stasis |
| E | use what is in front of you (doors, lockers, pickups, panels, benches, stores...); kinesis on an object further along your line of sight |
| Q | quick turn (180°) |
| V or Mouse 4 | melee (plasma saw swing) |
| C | stomp |
| X or middle mouse | switch fire mode (alt fire) |
| Space | zero-G jump |
| 1-4, mouse wheel | select weapon |
| Tab | open / close the RIG (weapon select) |
| B | navigation line (locator) without opening the RIG |
| Esc | pause / back |
| W A S D / arrow keys (menus) | move the highlight between buttons; at the edge of an Options page, turn the page |
| Space / Enter (menus) | press the highlighted button |
| Space / Enter / click during a cinematic | skip it (fast-forwards to the end) |
| hold Left Alt | free the cursor to click on screen (acts as touch) |
| F11 / Alt+Enter | toggle fullscreen (borderless) |
| F12 | screenshot (shots/) |
| ` (~) | console (see below) |
| ` (~) | console (see below) |
| F9 | interaction report: writes what E would use and every usable object nearby to deadspace.log |
| F10 | interaction debug view: hitbox (cyan), usable objects (white), what E would use (green) |
| F8 | switch renderer (legacy / native) |

These are the default bindings. The **Options menu** (title screen or pause menu) has the
port's settings on pages 4-13:
- page 3: **Anti-Aliasing** (MSAA Off/2x/4x/8x)
- page 4: **Mouse Sensitivity** (bar, 10-300%), **Mouse Smoothing**, **Free Look (Standing)**
- page 5: **Renderer** (Legacy / Native), **Texture Filtering** (Off, 2x-16x anisotropic) and
  **Bloom** (Off/Low/Medium/High); the last two are native renderer only, see Rendering below
- page 6: **Sharpening** and **Ambient Occlusion** (Off/Low/Medium/High, native renderer only) and
  **Show FPS** (No/Yes, off by default): a frame-rate counter at the top centre of the screen
- page 7: **Frame Rate Limit** (60 / 120 / 144 / 240 / Unlimited, default 60). Vsync stays on, so
  the monitor's refresh rate is the ceiling. Rates above 60 are untested beyond walking speed.
- pages 7-13: one row per control; press its arrow, then the key or mouse button to use
  (Esc cancels). A key already used by another control swaps with it. **Reset Controls**
  (last page) restores the defaults.

1-4 / mouse wheel (weapons), Mouse 4 (melee) and middle mouse (alt fire) are fixed extras.

Mouse look is linear (no acceleration): at 100% each mouse count turns the camera 0.05 degrees.
Mouse smoothing averages the last 3 frames of movement (same total turn, less jitter). Free look:
standing still, the mouse orbits the camera around Isaac; moving, aiming or any action turns him
to face the camera. The game's own Look Sensitivity (page 3) multiplies on top; at its default it
changes nothing.

All of these are saved in `userdata/port_settings.ini` (`msaa`, `mouse_sensitivity`,
`mouse_smoothing`, `free_look`, `renderer`, `anisotropy`, `reversed_depth`, `bloom`, `sharpen`, `ambient_occlusion`, `show_fps`, `fps_limit`, `bind_<control>`; key names as SDL spells them, e.g. `Left Shift`).

Options: `--msaa N`, `--width W --height H` (game resolution, default = desktop resolution), `--fullscreen` (borderless),
`--fps N` (frame cap for this run, 0 = off; overrides the Frame Rate Limit setting), `--verbose`, `--log FILE`.

Saves go to `userdata/` next to the executable.

### Rendering
The game draws with OpenGL ES 1.1 (fixed function). Two paths are built in:
- **Legacy**: each GL ES call is translated to desktop OpenGL's own fixed-function pipeline.
- **Native**: the port mirrors the game's GL state, converts every draw's vertex data itself and draws
  it through its own GLSL shader (modern OpenGL). Output matches the legacy path pixel for pixel
  (checked with tools/framediff.py); it is the base for lighting and texture improvements to come.

Switch with F8, the Options row, or `renderer = native|legacy` in port_settings.ini. The legacy
path stays as the fallback.

Enhancements on the native path:
- **Texture filtering** (`anisotropy`, default 16): world (depth-tested) draws sample through the
  port's own sampler objects: trilinear + anisotropic filtering. Textures the game uploaded without
  mipmaps get a generated mip chain (regenerated if the game changes the image). HUD and menu draws
  keep the game's filtering; render targets and nearest-filtered textures are left alone.
- **Depth precision** (`reversed_depth`, default 1): the game framebuffer has a 32-bit float depth
  buffer and native draws use reversed-Z (near = 1, far = 0, needs glClipControl / GL 4.5).
- **Post-processing** (`bloom`, `sharpen`, 0-3, default 1 = Low each): applied to the 3D scene just
  before the game draws its HUD, so HUD, menus and text are never touched. Bloom: bright areas
  (lights, glowing suit parts, screens) are blurred at 1/2, 1/4 and 1/8 resolution and added back.
  Sharpening: an unsharp mask limited to the local brightness range, so it adds no halos.
- **Ambient occlusion** (`ambient_occlusion`, 0-3, default 1 = Low): soft shading in corners, recesses
  and where characters' parts overlap, computed from the depth buffer alone (the game supplies no
  normals or lights). Runs in the same pass as the effects above, so the HUD is untouched; bright
  pixels (lights, glows) are left alone. Higher levels use more samples, a larger radius and more
  darkening.

### Console
Press the ~ key to open or close it; Enter runs a line, Up/Down recall earlier ones, Esc closes.
Typing / shows matching commands above the input line (filtered as you type); Tab completes the
highlighted one. Words in a command are joined with _ (spaces work too).
- /spawn_NAME or /spawn_NAME_COUNT: spawns COUNT (1-20) of that enemy in front of you, e.g.
  /spawn_slasher_4. NAME: slasher, lurker, brute, exploder, pregnant, swarm, boss
- /give_all: gives every weapon
- /god: toggles god mode (no damage)
- /bandana: toggles infinite ammo
- /level_MAP: loads a level, e.g. /level_03_water_01 (type /level_ to list them). Uses the game's
  developer level select, which also moves the current save profile to that level.
- /kill: kills the player (normal death and Continue screen)
- /help, /clear

### How E picks
A box in front of Isaac (1.8 m deep, 1.4 m wide, floor to above head height) points where the
camera looks. Every usable object has a zone: its 3D bounding box, padded to at least 0.5 m; objects
without a physical body (control panels, stores, power node locks, O2 stations) get a zone built
around the spot where the game shows their tap icon. E uses the one object whose zone overlaps the
box nearest your line of sight, and nothing else. With nothing in reach, E grabs a kinesis target
within ~9 m straight ahead. Plain shoot switches are not used with E (shoot them); covered ones are opened with E.

### Dev mode (testing)
`deadspace.exe --dev DIR` reads commands from `DIR/cmd.txt` while running; `tools/dev.py` drives
it (`start`, `send "cmd; cmd"`, `stop`). Commands are listed at the top of `src/devmode.cpp`
(teleport, goto an object, press keys, screenshots, `look`, `audit`, `god`...).

## Building

### Windows (MSYS2 MINGW64)

`external/dynarmic` is a clone of https://github.com/azahar-emu/dynarmic with its submodules
`fmt mcl robin-map xbyak zycore zydis` checked out (both platforms).

```
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,SDL2,boost,zlib}
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
```

The Windows build is linked statically: `deadspace.exe` needs no extra DLLs.

### Linux (x86-64)

```
sudo apt install build-essential cmake ninja-build libsdl2-dev libboost-dev zlib1g-dev libgl-dev
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
```

## Layout

- `src/cpu.*` — dynarmic wrapper, guest threads, host<->guest calls (SVC stubs)
- `src/memory.*` — 4 GiB guest address space, heap, mmap regions
- `src/elf_loader.*` — loads/relocates the ARM shared library, symbolizes addresses
- `src/hle/` — bionic libc, stdio, printf/scanf, pthreads, liblog, GLES 1.1 -> OpenGL, timing
- `src/jni/` — JNIEnv/JavaVM tables and host versions of the Java classes the game calls
- `src/apk.*` — APK (zip) import
- `src/sbin.*`, `src/game_patches.*` — patches the game's menu data (SBIN databases) and hooks
  OptionsPanel to add the Anti-Aliasing option
- `src/settings.*` — port settings (`userdata/port_settings.ini`)
- `tools/` — dex disassembler, ARM disassembly helpers, texture/screenshot utilities

## Notes on this APK

The "fan-fixed" APK re-encoded the textures as ETC2 while the game still labels them ATC;
the port decodes them as ETC2. For an unmodified APK pass `--atc-textures`.
