# Dead Space (2011 mobile) — native PC port

An unofficial, fan-made port that runs EA / IronMonkey's 2011 Android game *Dead Space* natively on
Windows (and Linux), without an Android emulator. The game's own 32-bit ARM code (`libDeadSpace.so`) runs
on the dynarmic JIT; everything it expects from Android (libc, pthreads, OpenGL ES 1.1, JNI/Java, audio,
assets) is reimplemented on the host with SDL2 and desktop OpenGL.

On top of the original game the port adds keyboard + mouse and controller play, an optional new renderer
(dynamic lights, baked shadow maps, ambient occlusion, bloom, anti-aliasing), higher frame rates, texture
packs, a Quit button, skippable cutscenes and an extended Options menu.

**No game data is included.** You need your own copy of the Android game as an APK file. This project is
not affiliated with or endorsed by Electronic Arts; Dead Space is a trademark of Electronic Arts Inc.

## Installing (Windows)

1. Download `DeadSpaceMobilePCPort-Setup.exe` from the
   [Releases](../../releases) page and run it.
2. Choose where to install (the default is a folder in your user profile; no administrator rights are
   needed) and pick **your Dead Space APK file** when asked. The installer copies the game's data out of
   the APK into the install folder; this takes a moment and needs about 400 MB.
3. Start the game from the Start menu or the desktop shortcut.

Windows may warn that the installer is from an unknown publisher: it is not code-signed.

- **Updating**: run a newer installer and choose the same folder. Leave the APK field empty to keep the
  game data that is already there; saves and settings are kept either way.
- **Uninstalling**: Settings > Apps > Installed apps > *Dead Space Mobile PC Port*, or run
  `uninstall.exe` in the install folder. It asks whether to delete your saves too; texture packs you
  added are left alone.
- **Which APK**: both the original, unmodified APK and the "fan-fixed" one work. The importer notes which
  kind it was (the original has ATC textures, the fan-fixed one ETC2 under the same labels). If textures
  look garbled, start the game once with `--atc-textures` (original APK) to check the other decoding and
  report it. *(Only the fan-fixed APK has been tested by the author.)*
- **Where things are** (all inside the install folder): `userdata/` saves and `port_settings.ini`,
  `gamedata/` the imported game files, `mods/textures/` texture packs, `deadspace.log` the last run's log.

### Without the installer

`deadspace.exe` is a single, statically linked program. Put it in a folder of its own and either place
your APK next to it (it is imported on first start) or run:

```
deadspace --apk <path to Dead Space APK>      # first run: imports the game data into gamedata/
deadspace                                     # afterwards
```

### Requirements

Windows 10 or 11, 64-bit; a graphics card and driver with OpenGL 3.0 or newer (for the new renderer; a legacy renderer is
available in Options > Graphics); about 400 MB of disk space. A controller is optional: any pad SDL
recognises works, with the Dead Space remake's layout by default.

## Playing

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
| Space / Enter / click during a cinematic | skip it: the cinematic is ended at once through the game's own end-of-animation path (state 2, then 3), so control, HUD, objective and tutorial follow as at its natural end. If one is still playing half a second later, the game clock is fast-forwarded through it instead |
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

These are the default bindings. The **Options menu** (title screen or pause menu) has six pages,
named in its header; the last three (Graphics, Controls, Controller) are the port's and scroll vertically (mouse wheel, drag, or just
moving up / down past the last visible row with the keyboard; a bar at the right shows the position),
with smaller rows than the game's own pages. Turning a page with the keyboard highlights its first row.
- **General**, **Game**, **Camera**: the game's own options
- **Graphics**: **Anti-Aliasing** (MSAA Off/2x/4x/8x), **Renderer** (Legacy / Native), then, for the native renderer (see Rendering below):
  **Texture Filtering** (Off, 2x-16x anisotropic), **Bloom**, **Sharpening**, **Ambient Occlusion**,
  **Dynamic Lights**, **Specular Highlights**, **Shadows** (Off/Low/Medium/High each),
  **Light Shadows** (No/Yes), **Light Contrast** (Off/Low/Medium/High) and **Vignette** (No/Yes); **V-Sync** (No/Yes, on by default; with it on, the
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
`mouse_smoothing`, `free_look`, `renderer`, `anisotropy`, `reversed_depth`, `bloom`, `sharpen`, `ambient_occlusion`, `dynamic_lights`, `specular`, `shadows`, `light_shadows`, `light_contrast`, `vignette`, `vsync`, `show_fps`, `fps_limit`, `bind_<control>`; key names as SDL spells them, e.g. `Left Shift`).

Options: `--msaa N`, `--width W --height H` (game resolution, default = desktop resolution), `--fullscreen` (borderless),
`--fps N` (frame cap for this run, 0 = off; overrides the Frame Rate Limit setting), `--verbose`, `--log FILE`.

Touch instructions are reworded for keyboard and mouse (English only): tutorial boxes and loading
hints name the keys bound when the game starts, and the tutorials' touch graphics (arrows, hand, device
pictures) are removed from the layouts, leaving the text box. On the loading screen that waits for
input ("Press any key to continue"), any key or mouse button continues and the left / right arrow keys
page through the hints.

### Controller
Any controller SDL recognises works (plug in at any time). Default layout, after the Dead Space remake:

| Input | Action |
|---|---|
| Left stick / Right stick | move / look and aim |
| LB (hold) or L3 (click) | run; the click keeps running until the stick is let go or the weapon is raised |
| LT / RT | aim / fire (melee swing while not aiming) |
| RB | alt fire while aiming, stomp otherwise |
| A | interact, kinesis; break free of a grab |
| X / Y | reload / stasis |
| B | quick turn; zero-G jump while aiming |
| D-pad | the four weapons |
| R3 | navigation line |
| Back / Start | RIG / pause |

In menus the D-pad or left stick moves the highlight, A confirms, B or Start goes back. The **Controller**
page of the Options menu has the camera stick sensitivity (10-300%) and one row per button (press the
row's arrow, then the button; Start cannot be bound; two actions may share a button when one applies
only while aiming). Settings: `pad_sensitivity`, `padbind_<action>`.

Light bar: on a DualSense or DualShock 4 (any controller with an RGB light) the light shows the RIG's
health colour: blue above half health, yellow down to a quarter, red below that and when dead; blue
outside gameplay.

Prompts follow the device last used: tutorial boxes, loading hints and the grab prompt name controller
buttons after any controller input and keys again after any key press, mouse click or mouse movement
(an open tutorial box is rewritten on the spot). Button names are Xbox style on every controller.

Power node locks: using one opens a small dialog. While it is up the mouse is free and the menu keys
or the controller work on its buttons (first press highlights, Space / Enter / A confirms).

Quitting: the main menu has a **Quit** button at the top right (click it, or move the highlight up to it
and confirm). It opens the game's own "Are you sure you want to exit?" prompt, which Esc / B on the main
menu also does; confirming closes the game. The window's close button and Alt+F4 work at any time.

Bench and store: while either is open the game is a menu, not gameplay: the camera does not move, the
mouse is free, and the menu keys or the controller work on it (W/A/S/D or arrows / D-pad or left stick,
Space / Enter / A to confirm, Esc / B to go back). The first press highlights the top-left control.
Moving onto an entry of the store's list or the bench's weapon row selects it (its details show) and the
list scrolls to follow; confirm on the selected entry is the same as Purchase / Upgrade. On the bench's
upgrade grid the highlight moves from node to node, confirm puts a power node in, and Purchase applies.

Grabs: when a necromorph grabs the player the game asks for touch gestures (swipe along an arrow, tap
repeatedly; swarms are tapped off one by one). With the port, the Melee key, the Interact key or Fire
each make whatever move the struggle is waiting for, and a prompt near the bottom of the screen shows
`PRESS [ key ]` or a blinking `TAP [ key ]` (it names the Melee key). The game's own "Swipe" / "Tap"
labels read "Press <key>" / "Tap <key>" (English).

Fog banks and light shafts: the game fades these effects out by distance as the player walks into them,
over a band about two metres wide, which at a run looks like an instant cut. The port spreads the change
over time (about 1.4 s from fully shown to gone, and the same coming back): each frame, after the game
has set an effect's opacity and before it is drawn, the opacity actually used moves toward the game's
value at a limited speed (src/lights.cpp, `smooth_fades`).

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
- **Light shadows** (`light_shadows`, default 1): every light that stays in the world (fixtures, lamps,
  panel glows, fans, beacons) has an identity, a fixed position and a fixed facing. A lamp looks out
  toward the open space around it (found once by casting rays through the still scene) and covers 150
  degrees; spotlights look along their cone. The **still scene** is kept in world space: every mesh that
  has stayed exactly in place for a few frames is copied into it the first time it is drawn (and taken
  out again if it starts to move, like a door). Each fixed light has a kept ("baked") map of that still
  scene and, near the player, a live tile for what moves: see *Baked shadow maps* below. Because the
  fixtures' light is already painted into the level, a blocked fixture darkens the scene there by the
  blocked share of the fixtures reaching that pixel (characters 40% of that). Short-lived lights (shots,
  impacts, RIG glow) use a screen-space trace instead. Lamps less than 0.8 m apart are one light.
  Dev: `shadows 2 debug` logs map and still-scene counts, `lightshadows 1 N`, `lightshade 0-1`,
  `probe X Y Z R` lists the lights and maps near a point.
- **Light Contrast** (`light_contrast`, 0-3, default 2 = Medium): the environment light. The game's
  painted lighting cannot react to anything, so it is turned down to a base level (100 / 85 / 70 /
  55%) on the level, props and characters alike, and the added lights bring their surroundings back
  up. Lamp faces and other bright pixels are left alone. Needs Dynamic Lights.
- **Player glows**: a glowing effect drawn at the player (the plasma saw's blade during a swing) gets
  a light exactly where it is drawn, in its own colour, for as long as it is drawn.
- **World tints**: the red alarm lighting and the hallucinations' blackouts (fade to black, then a dim
  grey a0a0a0 room) tint the level through the GL light-model colour. The renderer takes the colour that
  most of a frame's geometry is drawn with as the world tint (single objects such as an orange door lock
  have their own and are outvoted), tints the added point lights and highlights with it, and measures
  "bright pixel" thresholds relative to it, so a tinted or blacked-out room keeps its colour and darkness.
  `tools/tinttour.sh` and `tools/tintcmp.py` compare forced tints (dev command `tint RRGGBB AMOUNT`,
  `tint off`) between the legacy and native renderers.
- **See-through effects are not lit as surfaces**: fog banks, light shafts, beacon flares and steam are
  drawn over the finished scene, and the lighting pass only knows the wall behind them. Their draws also
  write their coverage to a third colour target, and what the lights, shadows and occlusion did to a
  pixel is applied only to the share they leave open. Fog banks (recognised by their vertex data) count
  in full; any other alpha-blended (not additive) draw counts where it floats more than a few centimetres in front of the
  solid scene (its depth is copied before the first such draw), so decals and holograms lying on a
  surface are still lit with it. Dev: `fogveil 0|1`.
- **RIG glow stays on the spine**: two lights of 36 cm reach along the lit part of the health meter,
  close to the back, instead of one a metre wide that tinted the whole suit; stasis module and visor
  lights shortened to match.
- **Baked shadow maps**: every fixed light (strip lights, detected lamps, fan lights, panel glows) owns a
  kept depth map of the still level as it sees it: 128 maps of 512 x 512 in one 8192 x 4096 atlas, each
  light's view passed to the shader through a small float texture. A map is drawn when its light is
  first met (nearest first, at most six per frame, fading in over 0.4 s) and again only when the still
  scene around it changes (a door opening, new geometry coming into view), and it is remembered when the
  light leaves the view (the one unseen longest goes first when all 128 are taken). A lamp's facing is
  settled once from the level around it and no longer follows the player. The 24 live tiles now hold
  only what moves (the player, enemies, doors, props) for the lights nearest the player, plus the turning
  alarm beacons, which can keep nothing. A fixed light without a map casts no shadow; the screen-space
  estimate is left to passing lights only (muzzle flashes, impacts). Shadows therefore do not depend on
  the camera, on which lights are in view, or on how the player got there.
  `tools/hallwalk.sh` walks chapter 1's second hallway at maximum settings and records filmstrips;
  `tools/shadowrepeat.sh` measures wall brightness at three spots before and after a detour.
- **Lamps everywhere**: every still level mesh is examined once by sampling its texture over its surface
  (a point every 6 cm). Bright, near-white samples that the level's painted lighting also shows lit are
  gathered into fixtures by position; a fixture between 0.006 and 0.9 m2 gets a point light whose
  strength and reach grow with its size. A mesh that is more than a fifth bright is a white surface and
  gives none. (Chapter 1 models its strip lights as separate triangles; later chapters paint them inside
  wall panels, which the earlier triangle test missed: 4-8 lights per view there, 40-90 now.)
- **Light Contrast is local**: the painted lighting is turned down only around added lamps (by how much
  unshadowed lamp light there is at a pixel), so places no lamp reaches keep the game's own brightness
  instead of just getting darker. A lamp that barely reaches a pixel cannot shadow it strongly, and a
  lamp's own face is never darkened by the light that sits on it.
- **Texture packs** (Options > Graphics > Texture Packs; setting `texture_packs`; read when the game
  starts): `mods/textures/<pack>/` next to the exe. Each texture is identified by a hash of its original
  pixels; a pack holds `<hash>.png` replacement colour textures (any size). `--dump-textures-png` (or
  `texture_dump=1`) writes every texture the game loads to `mods/textures/_dump/`. The folder's
  `README.txt` (written by the game) explains this for pack makers. (Derived PBR material maps were tried
  and removed.)
- **Bright rooms**: the scene as the game lit it is reduced to one pixel each frame (its average colour).
  In a room the game already lit brightly (average luma above about 0.18, fully by 0.34) the added lamps
  add little, the Light Contrast dimming is nearly off, overlapping lamps are compressed and the vignette
  is lighter; shadows and occlusion stay. Without this, a white room with a hundred lamps had its dark
  tones lifted and its whites greyed (washed out). Dark levels are untouched. `tools/tonecheck.sh LEVEL`
  compares tone percentiles with the legacy renderer at maximum settings (`tools/pngtone.py`).
- **Headroom**: added light only fills the headroom a pixel has left. The gain is scaled by
  (1 - the game's own colour) squared, capped at 0.45, and the result never passes 0.92 unless the game
  itself drew it brighter. Dark corridors take the lights fully; the white decks of chapter 1's later
  levels, which the game already lights to near white, gain almost nothing instead of burning out. A
  mesh in which more than a fifth of the triangles (or more than 40) look like lamps is a bright wall,
  not a row of lamps, and gives none. `tools/lighttour.sh LEVEL N` visits N doors of a level and prints
  the share of clipped pixels with the added lighting off and on (`tools/blowout.py`).
- **Solid cables, level lamps**: blended meshes that are really solid objects (the hanging cable
  bundles) are drawn opaque, so they take light and cast shadows. Small bright patches of the level's
  textures are taken as lamps and get a point light where the lockdown meshes give none. Only meshes of
  the still scene that are at least 2.5 m across are searched (the level's structure, not weapons, props
  or pickups), and a lamp goes away with its mesh. This finds only some of the lamps so far.
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

### Dev mode
Dev command `perf 1` logs slow frames with where the renderer's own CPU time went (lamp search, lamp
facing, shadow-map drawing, still-scene upload). The lamp search is capped at about 2.5 ms per frame and
skips texture areas with nothing bright in them (a 16 x 16 mask per texture).

Lighting survey: `tools/survey.sh LEVEL [COUNT]` stands in front of COUNT doors looking down the corridor
and prints, per stop, brightness / clipped / near-black share with the added lighting off and on and the
lights in use; shots go to `survey/`. `tools/surveysum.py FILES` averages per level. `DS_DEV=name` in
front of any dev script runs it as a separate instance (own command folder `name/`, own profile
`name_data/`), so several levels can be surveyed at once. Do not put MSYS2 on the PATH before calling
the scripts: MSYS2's bash started from Git Bash drops environment variables (the scripts call each other
with `"$BASH"` for the same reason).

`tools/tomenu.sh` starts a fresh profile and stops at the main menu (a fresh profile only has one after a
game exists: it starts one, skips the intro, pauses and quits to the menu; about 40 s).

Faster test loops:

- **Quick start**: `tools/devfresh.sh [--level MAP] [--at "X Y Z"] [--renderer native|legacy]` reaches
  gameplay on a fresh profile in about 6-10 s. It runs the game unthrottled (`turbo 6`: six extra game
  frames per displayed frame, no frame limit) and keeps pressing "continue" until the player has control,
  then turns turbo off. Waits are by state, not by frame count: `waitfor play|menu|loading|notloading|button NAME [MAXMS]`,
  and `tapbtn NAME` taps a menu button by name.
- **Shader hot-reload**: `shaders` recompiles the six post-processing shaders (bloom, AO, final lighting
  pass) from the raw strings in `src/render/native.cpp` on disk, in under a second, with no rebuild or
  restart. A shader that does not compile is reported with the driver's message and the old one stays.
  The per-draw shader (`kVS` / `kFS`) still needs a rebuild.
- **Text instead of pictures**: `report` prints the state (gameplay / menu, highlighted button, usable
  buttons), what the last frame was drawn with (lights by kind, shadow tiles, fog draws, world tint,
  lockdown) and the player's position; `stat [X0 Y0 X1 Y1]` gives mean colour, luma, saturation, clipped
  and near-black share of a screen region (fractions of the screen).
- **Bursts**: `burst NAME [N] [STEP] [DIV]` records N frames STEP frames apart into one filmstrip PNG
  (four per row), for motion and flicker.

Dev command `rich [CREDITS] [NODES]` fills the wallet (default 9999999 credits, 99 power nodes) for store
and bench tests; level 01_gov_03 has both next to each other (`goto Store 0`, `goto Bench 0`, `press e`).

`tools/starttest.py N SECONDS [-- game args]` starts the dev build N times on a throwaway profile and
reports any start that exits by itself, with its exit code. Access violations outside guest memory are
logged at first chance (`access violation (first chance)` plus a host stack).

`tools/dev.py status` says whether the dev instance is running and, if it has exited, shows the crash
report from its log. `send` and `status` exit with code 3 as soon as the game is not running.
`tools/grabtest.sh ENEMY DIST` runs a grab test (slasher, lurker, swarm).
 (testing)
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

The installer (`installer/installer.cpp`, a small Win32 program with the stripped `deadspace.exe`
embedded as a resource and `src/apk.cpp` for the import) is built with:

```
bash tools/mkinstaller.sh        # -> release/DeadSpaceMobilePCPort-Setup.exe
```

`setup.exe /silent <folder> <apk>` installs without a window (exit code 0 on success), for tests.

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
- `installer/` — the Windows installer / uninstaller
- `src/render/native.*`, `src/lights.*` — the new renderer: lighting, shadow maps, post-processing
- `src/texpack.*` — texture packs
- `src/input.*`, `src/menu_nav.*` — keyboard, mouse and controller; menu navigation
- `src/sbin.*`, `src/game_patches.*` — patches the game's menu data (SBIN databases): the extra Options
  pages, keyboard / controller wording, the main menu's Quit button
- `src/settings.*` — port settings (`userdata/port_settings.ini`)
- `tools/` — dex disassembler, ARM disassembly helpers, texture/screenshot utilities

## Notes on the APK

The "fan-fixed" APK re-encoded the textures as ETC2 while the game still labels them ATC, and keeps the
original ARM library under `assets/guest_libs`; the port decodes its textures as ETC2. An unmodified APK
(library under `lib/armeabi*`) has real ATC textures: the importer leaves a note (`gamedata/atc_textures`)
and the game then uses the ATC decoder; `--atc-textures` forces it.

## Legal

This is a fan project for people who own the game. The repository and the installer contain none of the
game's code, data or assets; they are read from the user's own APK on the user's machine. Dead Space is a
trademark of Electronic Arts Inc. The port's own source is in this repository; `external/dynarmic` is
under its own licence.
