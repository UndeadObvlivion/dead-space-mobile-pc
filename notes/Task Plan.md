# Task Plan (from User Notes)

Ordered by priority: progress blockers first, then feel/controls, then the settings UI that exposes
them, then visual bugs. Numbers in brackets refer to lines in "User Notes.txt".

## A. Interaction (blocks progress)
**Task 1: Facing-based interaction + stage 2 control panel** [4, 6]
- E picks only what the player is facing (nearest object inside a cone in front of the camera /
  character, not anything within reach).
- Find why the stage 2 lockdown control panel ignores E (probably the same selection logic or a
  different object type); verify on a fresh profile at that spot.

## B. Camera and aiming feel
**Task 2: Mouse sensitivity + smoothing** [1]
- Lower the default sensitivity, make the scale linear and predictable.
- Optional mouse smoothing (on/off), saved in port settings for now (menu comes in Task 5).

**Task 3: Stop the camera auto-recentering while moving** [2]
- Remove the camera's drift back to its default angle when walking/running, so mouse aim holds.

**Task 4: Free look while standing still** [8]
- When idle, the camera orbits around the character without turning him; moving or aiming snaps
  the character to face the camera direction.
- Done right after Task 3 because both change the same player/camera facing code.

## C. Controls
**Task 5 (small, can ride along with Task 2-4): Left click = melee when not aiming** [7]
- Left click without right mouse held swings the plasma saw; with right mouse held it fires.

## D. Settings menu
**Task 6: New settings pages** [3, plus toggles from 1]
- New Options pages built with the same menu patching used for Anti-Aliasing:
  - Mouse: sensitivity percentage bar, smoothing on/off (from Task 2).
  - Controls: key remapping page for keyboard and mouse.
- Last in the controls group so every setting it shows already exists.

## E. Rendering
**Task 7: Mirrored reflection moves the wrong way (stage 2 glass)** [5] - DONE 2026-10-09
- Likely a reflection-matrix / culling difference between GLES 1.1 and desktop GL in the
  translation layer; reproduce at the stage 2 glass and fix.

## Order
1. Task 1 (A)
2. Task 2 + Task 5 (B, C)
3. Task 3 (B)
4. Task 4 (B)
5. Task 6 (D)
6. Task 7 (E), independent; can be moved earlier if it bothers you.
