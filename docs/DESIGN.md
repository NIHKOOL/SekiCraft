# SekiCraft — Design Doc

> Play Sekiro as the main game while *being* a Minecraft player: Minecraft movement, inventory, blocks, digging and combat inside the real Sekiro world, fighting Sekiro's enemies.

Status: draft v0.1 · 2026-10-07 · working name, rename freely

---

## 1. Core principle

Same as [SkyCraft](https://github.com/chasmlol/SkyCraft) (MIT), which this project builds on:

**Neither game is rewritten.**
- Minecraft runs its own logic: physics, inventory, crafting, blocks, combat math.
- Sekiro runs its world: maps, enemies, AI, bosses, saves.

The two mods only translate between them:
- Sekiro tells Minecraft what the world is shaped like and where the enemies are.
- Minecraft tells Sekiro where the player is, what the player hit, and what to draw.

## 2. What is reused vs. new

| Part | Source | Work |
|---|---|---|
| Minecraft side (Fabric mod, ~9k lines): collision injection, input bridge, overlay/world export, dig system, actor proxies | **Fork of SkyCraft `fabric/`** (MIT, keep the license notice) | Rename; replace Skyrim-specific bits (damage scaling, skill XP, Skyrim weather) with Sekiro equivalents |
| Shared-memory protocol | **Fork of SkyCraft `protocol/skycraft_protocol.h`** | Swap coordinate mapping; Sekiro-specific fields (posture) |
| Game side | **New: `sekiro/` C++ DLL** | Everything. SkyCraft's version uses SKSE + CommonLibSSE, and Sekiro has no equivalent, so the game structures must be reverse engineered |

Reference only, **do not copy code** because of their licenses:
- [sekiro-coop](https://github.com/mstampfli/sekiro-coop) (AGPL-3.0): ChrIns layout, AOB patterns, v1.06 offsets
- [SekiroImGui](https://github.com/rootBrz/SekiroImGui) (GPL-3.0): camera and FOV offsets
- [SekiroFpsUnlockAndMore](https://github.com/uberhalit/SekiroFpsUnlockAndMore): camera and FOV offsets
- [Sekiro-Practice-CT](https://github.com/ElaDiDu/Sekiro-Practice-CT): cheat-engine pointer paths (teleport, HP, posture)

## 3. Target environment

| Thing | Value | Notes |
|---|---|---|
| Sekiro | **Steam v1.06** | Last patch, 2020. Offsets will not move again |
| Loader | **me3** | Disables Arxan anti-tamper and loads our DLL. Mod Engine 2 is the fallback |
| Hooking | MinHook (BSD-2) | |
| Minecraft | 26.3 + Fabric | Same pin as SkyCraft, so its Mixins work unchanged |
| Java | 25 | |
| C++ | Visual Studio 2022+ (MSVC), CMake | |
| RE tools | Cheat Engine, x64dbg, Ghidra, RenderDoc | |

Sekiro is offline-only and has no anti-cheat, so there is no ban risk. Arxan (anti-tamper) crashes the game if code is patched; me3 neutralizes it.

## 4. Components

```
┌──────────── sekiro.exe ─────────────┐                  ┌────── javaw.exe (Minecraft 26.3) ──────┐
│ sekicraft.dll (loaded by me3)        │                  │ sekicraft Fabric mod (SkyCraft fork)   │
│                                      │                  │                                        │
│ WorldProbe  ─ collision near player ─┼─────────────────▶│ CollisionField → MC collision queries  │
│ EnemyMirror ─ enemies (pos,box,HP) ──┼─────────────────▶│ ActorProxy entities (invisible)        │
│ InputBridge ─ keyboard/mouse/pad ────┼─────────────────▶│ MC input handlers                      │
│ HitBridge   ─ "enemy hit player" ────┼─────────────────▶│ player.hurt(...)                       │
│                                      │                  │                                        │
│ Puppet      ◀─ player pos / look ────┼──────────────────│ MC LocalPlayer physics                 │
│ CameraDriver◀─ view + projection ────┼──────────────────│ MC camera                              │
│ DamageApply ◀─ "you hit enemy X" ────┼──────────────────│ ActorProxy.hurt()                      │
│ Compositor  ◀─ hand/GUI/world layers ┼──────────────────│ offscreen render                       │
└──────────────────────────────────────┘                  └────────────────────────────────────────┘
                     shared memory (Local\SekiCraft_v1) + D3D11 Present hook
```

## 5. Coordinate mapping (measured 2026-10-07)

FromSoftware's engine is **Y-up, 1 unit = 1 meter**: **1 block = 1 Sekiro unit**. Wolf is about 1.7–1.8 m tall and the MC player is 1.8 blocks.

Sekiro is **left-handed**: running a circle to the left *decreases* `atan2(dx, dz)`, where Minecraft's right-handed axes would increase it. Wolf's facing angle θ (radians, physics +0x74) has forward = (−sin θ, −cos θ) in Sekiro's (x, z).

```
mc.x   = sek.x
mc.y   = sek.y
mc.z   = -sek.z
mc.yaw = degrees(θ)     (after the flip, forward is (-sin θ, cos θ): Minecraft's own convention)
```

Implemented in `sekiro/src/game.h`. Measured with `tools/re/record.py`: a forward run at θ = −20.8° moved along (0.355, 0.935) in MC axes, exactly as predicted.

### 5.1 Known memory (Sekiro 1.06)

From the community Sekiro Practice cheat table, verified live:

| What | Where |
|---|---|
| `WorldChrMan` | static `sekiro.exe+0x3D7A1E0`. AOB `48 8B C6 48 89 05 ?? ?? ?? ?? 48 85 C0`, +3 is `mov [rip+rel32], rax` |
| Player physics module | `[[[[WorldChrMan]+0x88]+0x1FF8]+0x68]` |
| Position | physics +0x80 / +0x84 / +0x88 (float). +0x90 holds a copy |
| Facing θ | physics +0x74 (float, radians). +0xBC = cos θ |
| Player draw flag | `[[WorldChrMan]+0x88] + 0x1A11`, bit 3 (1 = drawn). Clearing it hides Wolf entirely: body, weapon and shadow |
| `FieldArea` | static `sekiro.exe+0x3D5C0A0`. AOB `48 3b c7 48 0f 44 c5 48 89 05`, +7 is `mov [rip+rel32], rax` |
| Debug free camera | `[[FieldArea]+0x20]+0xE8`. 4×4 matrix at +0x10: rows right, up, forward, position (left-handed, right × up = forward). Vertical FOV (rad) +0x50, aspect +0x54, near/far +0x58/+0x5C |
| Free-cam mode | byte `[[FieldArea]+0x20]+0xE0`: 0 normal, 1 free camera |

### 5.2 Input (Sekiro 1.06)

- **Keyboard:** DirectInput 8 `GetDeviceState`. Sekiro polls four keyboard-type devices; none of them is a mouse.
- **Mouse look:** `GetCursorPos` / `SetCursorPos`, about 360 and 60 calls/s. Sekiro also calls `GetRawInputData`.
- **SekiCraft's approach** (`sekiro/src/input.cpp`):
  - Hooks DirectInput `GetDeviceState`/`GetDeviceData` and user32 `GetCursorPos`/`SetCursorPos` with MinHook.
  - While Minecraft has control, keys become SDL scancodes for Minecraft. Cursor motion is measured, the cursor is parked again, and the motion becomes look.
  - Sekiro sees an idle keyboard and a still cursor. Esc still reaches Sekiro and hands control back.

**Puppet result (Phase 2 test):** writing the position every frame from MC moves Wolf smoothly, and the camera follows. Sekiro keeps the horizontal position (drift < 2 cm) but nudges the height by a few cm, presumably snapping to its real ground. **Wolf does not animate**: he glides with his legs still, because his locomotion animation only runs when Sekiro's own movement drives him. That doesn't matter now: per the decision below, Wolf is hidden while Minecraft controls.

**Decision (2026-10-07): you play as the Minecraft player**, as in SkyCraft. While Minecraft has control:
- The camera is first person at Minecraft's eye (Sekiro's free camera, written every frame).
- Wolf is hidden (draw flag), and Wolf's position and facing follow Minecraft's player.
- A "play as Wolf" mode could come later.

Hotkeys: **F10** gives Minecraft control; **F9** or **Esc** gives it back to Sekiro.

Sekiro maps are split into areas with their own origins: `m10_00` (Hirata), `m11_00` (Ashina), `m20_00` (Sunken Valley), and so on.
- Each **area gets its own MC dimension**, like Skyrim worldspaces in SkyCraft.
- Area changes (bonfire/idol warps, loading screens) bump `collisionEpoch` and teleport the MC player.

## 6. Game-side subsystems and how we get each one

| Subsystem | Approach | Confidence |
|---|---|---|
| **Load DLL** | me3 profile with our DLL | High, proven by sekiro-coop |
| **Read player state** | `WorldChrMan` → player `ChrIns` → physics module position/rotation. AOB-scan for `WorldChrMan`, verify live with Cheat Engine | High |
| **Puppet the player** (MC drives position) | Write the **physics module** position every frame (what cheat-table teleport/noclip does), and zero Sekiro's movement input. sekiro-coop found that writing `ChrIns` position alone is ignored by rendering, so we write the Havok-side position | Medium, **first risk to test** |
| **Camera** | Override the field camera's matrix and FOV after Sekiro updates it. Hide Wolf's body in first person | Medium |
| **Input** | Hook DirectInput8 / XInput / raw input, swallow it from Sekiro, forward it to MC (SkyCraft's routing modes) | High |
| **Collision for MC** | **Option A:** call the engine's Havok ray cast (used by camera collision) on a grid around the player, like SkyCraft stage A. **Option B:** extract map collision meshes offline from the player's own game files and stream triangles (SkyCraft's `TriCollider` already consumes triangles). Spike both in Phase 3 | Medium |
| **Overlay (hotbar, inventory, hand)** | D3D11 `Present` hook, CPU pixel path from shared memory first (SkyCraft protocol already has it), GPU sharing later | High |
| **World layer (placed blocks)** | Composite MC's color+depth against Sekiro's depth buffer. Depth target and format found with RenderDoc | Medium |
| **Enemies** | Walk the `ChrIns` list → actor table (pos, hitbox, HP, **posture**, hostile/dead) | High |
| **Player hits enemy** | Apply MC damage to HP. MC hits also fill **posture**. A posture break lets the next MC attack trigger a deathblow | Medium |
| **Enemy hits player** | Hook the player damage path, cancel it in Sekiro, forward it to MC as `PlayerHurt` (MC armor, shields and totems apply) | Medium |
| **Digging into Sekiro's ground** | See §7 | Low–Medium |

## 6.1 Collision as built (Phase 3, `sekiro/src/collision.cpp`)

**Ray cast:** `FrpgCastRay` (`sekiro.exe+0x94CC50`, AOB `E8 ?? ?? ?? ?? 84 C0 74 4F 0F` → call target, prologue checked) against `[[FrpgHavokMan]+0x98]` (`sekiro.exe+0x3D6D640`) with filter `0x4E`. These interface facts come from SekiroTool (MIT).

Signature: `bool(world, filter, start xyz1, delta xyz0, out hitPos, out hitNormal, out fraction, out object)`. Vectors are 16-byte aligned.

**Measured behaviour:**
- It costs about 0.8 µs per cast.
- It doesn't hit Wolf's own body.
- Surfaces are **two-sided**, and the normal always faces the ray, so a hit doesn't say which side is solid.

**Game thread:** casts run inside our DirectInput `GetDeviceState` hook, once per frame (Sekiro's main thread), with a 2 ms budget. That hook only runs while Sekiro has focus, which is fine while playing.

**Method** (no volume guessing, because solidity is unknown):
1. Vertical multi-hit rays on a 0.5-block grid find every surface in each column.
2. Neighbouring surfaces within 0.9 blocks of height are joined into floor/ceiling triangles. The collider uses |ny|, so winding doesn't matter.
3. From every surface, horizontal rays toward each neighbour column at +0.3, +0.9 and +1.5 blocks find real walls: a cliff stops them, a doorway or awning doesn't. Each hit becomes a vertical quad, from that floor up to the far side's next surface.
4. Voxels are a 0.25-block slab under each surface. Minecraft uses them for "ground is here" checks and for other entities.

**Coverage:**
- 5×5 region columns around the player, 6 regions tall (3 below, 2 above).
- Nearest first. The full area takes about 5 s; the player's own region takes a fraction of a second.
- The band rescans when the player changes height by 2+ regions. A jump of more than 48 blocks starts a new epoch.

**Result:** walls stop the player, slopes and stairs walk smoothly, and there's no stutter.

**Not yet:**
- Moving geometry (doors, elevators) isn't refreshed.
- There's no scanning while Sekiro is unfocused.
- Overhangs thinner than 0.5 blocks horizontally can be missed.

## 7. Digging (the hard part)

Skyrim's ground is a heightmap that SkyCraft can deform. Sekiro's ground is **static meshes with baked Havok collision**, which cannot be reshaped at runtime. The plan:

1. **Physics:** the MC side already owns dig state (SkyCraft `SkyDig`). Dug cells are removed from the CollisionField, so **for the MC player the hole is real** at no extra cost.
2. **Visuals ("hole mask"):**
   - MC renders the hole's inside (dirt/stone walls, `DigWalls`) into the world layer.
   - Where a pixel lies inside a hole opening **and** Sekiro's depth there is within ε of the original ground surface, the compositor shows MC's pixels instead of Sekiro's.
   - Enemies and objects standing in front of the hole still draw normally because they are nearer than the ground surface.
3. **Drops:** the material comes from the surface type (grass → dirt, then stone with ores below, bedrock at the bottom), as in SkyCraft.
4. **Not in v1:**
   - Enemies falling into holes. Sekiro AI follows navmesh; we can still kill an enemy that would fall in.
   - Holes through walls of buildings.

## 8. Phases

Each phase ends in something you can see working.

| # | Phase | Done when |
|---|---|---|
| 0 ✅ | **Toolchain + link** | DLL loads via me3 and logs. Fabric mod (forked) builds and runs with `runClient`. Both handshake over shared memory. A tiny **fake-Sekiro harness** (`tools/fakegame`) lets the MC side be tested without the game |
| 1 ◐ | **See Sekiro's state** (player position, facing and handedness done; enemies remain) | In-game debug window shows live player position, rotation, area and nearby enemies. Coordinate handedness pinned |
| 2 ✅ | **Puppet** | Walking in MC (on a flat floor at Sekiro ground height) moves Wolf. Camera follows MC's view |
| 3 ✅ | **Walk Ashina in MC physics** (ray-cast stage) | Collision field (A or B) lets you sprint-jump around Ashina Outskirts; cliffs and slopes behave |
| 4 | **Overlay** | MC hotbar, hearts, inventory and hand drawn in Sekiro |
| 5 | **Blocks** | Place and break blocks on Sekiro surfaces, depth-correct |
| 6 | **Combat** | Fight Ashina soldiers with MC weapons; they hit back; posture and deathblows |
| 7 | **Digging** | Dig holes into Sekiro's ground with correct drops (§7) |
| 8 | **Polish** | Idol warps across dimensions, save snapshots, resurrection ↔ MC death, auto-launch |

## 9. Risks

| Risk | Mitigation |
|---|---|
| Position writes ignored or fought by Sekiro's character controller | Test in Phase 2 first. Fallbacks: write the Havok rigid body directly, or hook the controller update and replace its velocity |
| Arxan crashes on hooks | me3's Arxan disabling; hook only through MinHook after me3 init |
| Collision extraction (no CommonLib, no documented Havok API) | Two candidate approaches (§6); the offline one avoids runtime RE entirely |
| Digging visuals (§7) | Hole-mask compositing; scope limits stated up front |
| Two games' RAM/GPU cost | MC renders almost nothing (void world). Cap JVM heap ~3 GB |

## 10. Repo layout

```
docs/DESIGN.md
protocol/      shared-memory layout (C++ header + Java mirror)
sekiro/        game-side DLL (CMake, MinHook)
fabric/        Minecraft mod (fork of SkyCraft fabric/)
tools/         fakegame harness, me3 profile, dev launch scripts
```
