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
| F5 | debug: painted vertex lighting normal / hidden / alone |
| F6 | debug view: light placement |
| F7 | debug view: hitboxes |
| F8 | switch renderer (legacy / native) |

These are the default bindings. The **Options menu** (title screen or pause menu) has five pages,
named in its header; the last two are the port's and scroll vertically (mouse wheel or drag; a bar at
the right shows the position), with smaller rows than the game's own pages:
- **General**, **Game**, **Camera**: the game's own options
- **Graphics**: **Anti-Aliasing** (MSAA Off/2x/4x/8x), **Renderer** (Legacy / Native), then, for the native renderer (see Rendering below):
  **Texture Filtering** (Off, 2x-16x anisotropic), **Bloom**, **Sharpening**, **Ambient Occlusion**,
  **Dynamic Lights**, **Specular Highlights**, **Shadows** (Off/Low/Medium/High each),
  **Light Shadows** and **Vignette** (No/Yes); **V-Sync** (No/Yes, on by default; with it on, the
  monitor's refresh rate is the ceiling); **Frame Rate Limit** (60 / 120 / 144 / 240 / Unlimited,
  default 60; rates above 60 are untested beyond walking speed) and **Show FPS** (No/Yes, off by default)
- **Controls**: **Mouse Sensitivity** (bar, 10-300%), **Mouse Smoothing**, **Free Look (Standing)**,
  then one row per control: press its arrow, then the key or mouse button to use (Esc cancels). A key
  already used by another control swaps with it. **Reset Controls** (last row) restores the defaults.

1-4 / mouse wheel (weapons), Mouse 4 (melee) and middle mouse (alt fire) are fixed extras.

Mouse look is linear (no acceleration): at 100% each mouse count turns the camera 0.05 degrees.
Mouse smoothing averages the last 3 frames of movement (same total turn, less jitter). Free look:
standing still, the mouse orbits the camera around Isaac; moving, aiming or any action turns him
to face the camera. The game's own Look Sensitivity (Camera page) multiplies on top; at its default it
changes nothing.

All of these are saved in `userdata/port_settings.ini` (`msaa`, `mouse_sensitivity`,
`mouse_smoothing`, `free_look`, `renderer`, `anisotropy`, `reversed_depth`, `bloom`, `sharpen`, `ambient_occlusion`, `dynamic_lights`, `specular`, `shadows`, `light_shadows`, `vignette`, `vsync`, `show_fps`, `fps_limit`, `bind_<control>`; key names as SDL spells them, e.g. `Left Shift`).

Options: `--msaa N`, `--width W --height H` (game resolution, default = desktop resolution), `--fullscreen` (borderless),
`--fps N` (frame cap for this run, 0 = off; overrides the Frame Rate Limit setting), `--verbose`, `--log FILE`.

The game's habit of showing the navigation line by itself when the player stands idle (which also
tilts the camera down) is patched out; the line only appears on its key.

Saves go to `userdata/` next to the executable.

### Rendering
The game draws with OpenGL ES 1.1 (fixed function). Two paths are built in:
- **Legacy**: each GL ES call is translated to desktop OpenGL's own fixed-function pipeline.
- **Native**: the port mirrors the game's GL state, converts every draw's vertex data itself and draws
  it through its own GLSL shader (modern OpenGL). Output matches the legacy path pixel for pixel
  (checked with tools/framediff.py); it is the base for lighting and texture improvements to come.

World tints (the red alarm lighting, GameWorld::setTintOverride) reach the renderer as GL lighting
with no lights: scene ambient x vertex colour. The native shader implements that ambient-only case.

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
- **Reconstructed normals**: the game draws without surface normals, so the native renderer builds
  them. Per draw, face normals are averaged per vertex (vertices at the same position share one, so
  texture seams do not show); the shader blends that smooth normal with the exact face normal from
  screen-space derivatives, keeping hard edges hard. Opaque scene geometry writes the result to a
  second color target, which ambient occlusion uses (no faceting on low-poly characters) and which
  dynamic lighting builds on. Dev command `normals 1` shows it.
- **Dynamic lights** (`dynamic_lights`, 0-3, default 2 = Medium): the game has no lights of its own,
  so the port makes point lights from game events (src/lights.cpp): a flash at the muzzle when a
  weapon fires, a flash where a shot lands, a light that follows projectiles in flight (line gun
  bolts, enemy projectiles) and a larger flash for explosives. They light nearby surfaces using the
  reconstructed normals, on top of the baked look. Only with the gameplay camera
  (not in cinematics). Dev commands: `dlights N`, `light ...` (test light), `lighttrace 1`.
- **RIG glow**: two of the dynamic lights sit on the player's back. The spine light takes the health
  bar's colour (cyan above 50%, yellow 25-50%, red below 25%, the game's own thresholds) and moves
  down the back as health drops; the stasis module's light dims as the meter empties and pulses
  brighter while kinesis is held. Part of Dynamic Lights (off when that is off).
- **Visor, saw and navigation line glow** (also part of Dynamic Lights): a faint blue light in front
  of the helmet visor; a flickering cyan light at the weapon while a melee swing plays, with a brighter
  flash when it connects; and a row of cyan lights along the navigation line's path while the line is
  shown.
- **Shadows** (`shadows`, 0-3, default 2 = Medium): shadows of things that move. The level's own
  shadows are baked into its textures, so only moving objects cast: a mesh whose vertex data changes
  between frames (CPU-skinned characters) or whose world transform changes (props, doors) is a caster
  from then on. Casters are drawn again into a 2048x2048 depth map from an assumed light above the
  camera target (16 m square); upward-facing surfaces below them are darkened, less the further the
  caster is above. That light follows the real ones: each strip light or alarm spotlight near the
  player pushes the shadow away from itself and adds to its strength (eased over ~120 ms); with no
  light near, a faint shadow falls almost straight down. The option level scales the strength. Only with the gameplay camera.
  Dev: `shadows N debug` tints shadowed pixels red and logs the caster count.
- **Light shadows** (`light_shadows`, default 1): character and prop shadows come from the lights
  themselves. Up to 16 lights get a real shadow map (a 1024x1024 tile of one atlas): every spotlight
  (alarm lamps, ceiling fans), then the fixtures nearest the player. A fixture's map looks from the
  light toward the player, so whether the light is on the ceiling, a wall or the floor, the player's
  shadow falls on the far side of it. Spotlight tiles take every opaque mesh; fixture tiles take only
  things that move (characters, props, doors), because the level's own shadows from its fixtures are
  already painted in. Because the fixtures' light is already painted into the level's textures, a blocked fixture
  also darkens the scene there, by the blocked share of the fixtures reaching that pixel: shadows are
  darker where one light dominates and fade where others fill in. The overhead shadow map of
  **Shadows** is then only a faint contact shadow. Short-lived lights (shots, impacts, RIG glow) use a
  screen-space trace instead. Dev: `lightshadows 1 N` shows what light N can see; `lightshade 0-1`.
- **Ceiling fan lights** (part of Dynamic Lights): the game fakes light falling through its ceiling
  fans with turning shaft meshes and a floor texture. Each fan gets a real spotlight pointing down from
  it, as wide as the shafts, shaded by three blades and a hub that turn with the shafts, so the blade
  shadows fall on whatever is underneath (floor, props, characters). The fan is recognised from its
  shaft mesh (upright fins through a vertical axis that keep turning); it has a shadow map like the
  alarm spotlights and stays lit while the player is within 10 m.
  The blade pattern is the game's own floor shadow (a textured quad multiplied over the floor under
  the fan): the light samples that same texture with the quad's own mapping, carried up the cone, so on
  the floor the two coincide exactly and anything above the floor gets the matching part of the
  pattern. (Until a fan's floor quad has been drawn once, a generated three-blade pattern is used.)
  The pattern is one tile of a texture sheet; its brightness range is measured from that tile. Things
  standing under the fan are also darkened where a blade's shadow crosses them (as the game does to the
  floor), so the blades read as shadows on characters. Dev: `fanshade 0-1` (2 = debug colours).
- **Panel glows** (part of Dynamic Lights): door holograms, screens and other flat glowing overlays
  cast light in the colour they are showing. The renderer finds them as it draws (flat additive or
  bright alpha-blended meshes of the 3D scene, 0.2-2.2 m across) and takes the colour from the part of
  the texture each one uses times the tint and opacity the game draws it with, so the light follows
  the panel's state by itself: another texture or tint changes the colour, a fading panel dims, a
  hidden one gives no light. Parts of one panel are merged into one light. Found lights are used from
  the next frame on, so they get a shadow map like the others. Dev: `glowgain N`.
- **Strip lights** (part of Dynamic Lights): the small ceiling and door strip lights get point lights
  whose colour follows their state: a dim cool white normally, red during a lockdown. Their positions
  are read from the level's lockdown light meshes when a level starts (src/lights.cpp). Every fixture
  the camera can see is lit (within its 30 m view distance and inside the view frustum, up to 96), so
  lights do not switch on as the player approaches. Up to 128 lights at once in total; the shader
  skips a light for every pixel outside its radius.
- **Specular highlights** (`specular`, 0-3, default 2 = Medium): Blinn-Phong shine from the dynamic
  lights. The game has no material data, so brighter, less saturated surfaces (metal, armour plates)
  shine more than dark or strongly coloured ones. Needs Dynamic Lights on.

### Debug views
Three overlays, each toggled by a key or a console command (`/debug_off` turns all of them off):
- **Vertex lighting** (F5, `/debug_vertexlight`, cycles): *hidden* draws the level's opaque,
  vertex-coloured meshes without their vertex colours (textures and added lights only); *alone* draws
  them with the vertex colours but no texture. Meshes without vertex colours are unaffected: where
  nothing changes, the lighting is painted into the textures.
- **Lights** (F6, `/debug_lights`): a cross at every light the port adds, within 30 m, in the light's
  colour, with its kind (strip light, alarm spot, RIG health, stasis module, visor, saw, nav line,
  projectile, flash), radius, and shadow map number if it has one. Lit lights get a ring showing their
  radius; alarm spotlights show their axis and cone edges. Dim crosses are strip lights that exist in
  the level but are not lit right now (out of view).
- **Hitboxes** (F7, `/debug_hitboxes`): the bounding box the game reports for each object within 18 m
  (GameObject::getBounds): green player, red enemies, pink severed limbs, orange projectiles, yellow
  everything else, named when close. These are whole-object boxes, not per-limb hit volumes.
- **Interaction** (F10, `/debug_interact`): cyan = the area in front of the player that E checks,
  white = usable objects' zones, orange = kinesis targets, green = what E would pick.

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
