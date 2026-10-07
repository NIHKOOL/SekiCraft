# SekiCraft

Play Sekiro: Shadows Die Twice as a Minecraft player. You walk around Ashina with Minecraft's movement and physics, see Minecraft's hotbar, hearts, hand and inventory, and collide with Sekiro's real walls and ground.

Neither game is rewritten. Both run at the same time:
- **Sekiro** runs its world, enemies and saves.
- **Minecraft** runs the player.

Two mods connect them through shared memory: a DLL inside Sekiro and a Fabric mod inside Minecraft. SekiCraft builds on [SkyCraft](https://github.com/chasmlol/SkyCraft) (Minecraft inside Skyrim), whose Minecraft-side mod it forks.

> **Status: early and experimental.** Expect rough edges, and back up your Sekiro saves.
> This is a fan project, not affiliated with FromSoftware, Activision, Mojang or Microsoft. You need to own both games; no game files are included here.

## What works

| | |
|---|---|
| Minecraft controls | **F10** switches to Minecraft: first-person camera at the Minecraft player's eye, mouse look, WASD, jump, sprint. Wolf is hidden and follows you, so Sekiro's enemies still see you. **F9** or **Esc** switches back |
| Collision | Minecraft's physics collides with Sekiro's real ground, slopes, stairs and walls. They're measured live with the game's own ray casts around the player |
| HUD | Minecraft's hotbar, hearts, hunger, crosshair, hand and inventory drawn over Sekiro. Clicking, scrolling and moving items work |
| Blocks | Place and break blocks on Sekiro's ground. They're drawn in Sekiro's 3D view with Minecraft's textures and hidden behind Sekiro's walls, rocks and trees. Not yet lit by Sekiro's light |
| Entities | Dropped items, arrows, block cracks, the targeted block's outline, Minecraft mobs and particles, and your own body in third person (F5) |
| Digging | Dig into Sekiro's ground: grass, dirt, then stone with ores. Holes are cut out of Sekiro's picture and you can climb down into them. Cliffs and walls dig approximately |
| Combat | Hit Sekiro's enemies with Minecraft weapons (HP and posture). A hit on a broken posture is a deathblow, and bosses lose one life (red dot) per deathblow. Enemies' hits cost Minecraft hearts, in proportion to Wolf's health |

**Planned:**
- Sekiro's lighting on blocks

See [docs/DESIGN.md](docs/DESIGN.md) for how it works, the memory addresses used, and the roadmap.

## Requirements

- **Sekiro: Shadows Die Twice**, Steam, version **1.06**
- **Minecraft: Java Edition**, with **Fabric** for Minecraft 26.3. Development currently runs it through Gradle (see below)
- Windows 10/11 x64
- To build:
  - **Visual Studio 2022 Build Tools** (C++ workload) and **CMake**, for the Sekiro DLL
  - **Java 25 JDK**, for the Minecraft mod
- [**me3**](https://github.com/garyttierney/me3) (tested with v0.13.0). It loads the DLL into Sekiro and neutralizes Sekiro's anti-tamper code

## Building

**Sekiro side.** From an *x64 Native Tools Command Prompt for VS 2022*:

```bat
cmake -S sekiro -B sekiro\build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build sekiro\build
```

This produces two DLLs in `sekiro\build`:
- `sekicraft.dll`, a small loader
- `sekicraft_core.dll`, the logic. The loader hot-reloads it whenever it's rebuilt, so you can iterate without restarting Sekiro.

**Minecraft side.** With `JAVA_HOME` pointing at a Java 25 JDK:

```bat
cd fabric
gradlew build
```

**me3.** Download `me3-windows-amd64.zip` from the [me3 releases](https://github.com/garyttierney/me3/releases) and unzip it into `tools\me3`, so that `tools\me3\bin\me3.exe` exists.

## Running

1. Start Minecraft with the mod: `cd fabric` then `gradlew runClient`. Its window hides itself once Sekiro connects.
2. Start Sekiro with `tools\launch-sekiro.bat`. This runs offline through me3.
3. Load a save. When you can control Wolf, press **F10**.

Logs: `sekiro\build\sekicraft.log` (Sekiro side) and the Gradle console or `fabric\run\logs` (Minecraft side).

## Controls

| Key | |
|---|---|
| **F10** | Minecraft controls |
| **F9** / **Esc** | Back to Sekiro controls. Esc closes an open Minecraft screen first |
| Minecraft keys | WASD, Space, Shift, Ctrl, E (inventory), 1–9 and mouse wheel (hotbar), mouse buttons |
| **F8** | Debug: logs every collision surface through Wolf's position |
| **F7** | Debug: cycles how many frames the block camera lags Sekiro's (0–2; 1 is right) |
| **F6** | Experimental: cycles block lighting from Sekiro's picture (normal, darker, brighter, off; off by default) |

## Project layout

```
docs/DESIGN.md   design, findings, memory map, roadmap
protocol/        shared-memory layout (C++), game-side link helpers
sekiro/          Sekiro DLL: loader + core (input, camera, collision, overlay)
fabric/          Minecraft Fabric mod (fork of SkyCraft's)
tools/           fakegame (test the Minecraft side without Sekiro), RE scripts, me3 profile
```

## Credits and licenses

SekiCraft is MIT-licensed (see [LICENSE](LICENSE)). It builds on and thanks:

- **[SkyCraft](https://github.com/chasmlol/SkyCraft)** by chasmlol (MIT, see [LICENSE-SkyCraft](LICENSE-SkyCraft)):
  - the Minecraft-side mod and the shared-memory protocol are forked from it
  - the overlay compositor and the key-code table are adapted from its Skyrim side
- **[SekiroTool](https://github.com/borgCode/SekiroTool)** by Shilkey and Centz (MIT): the ray-cast function and physics-world addresses
- **[Sekiro Practice cheat table](https://github.com/ElaDiDu/Sekiro-Practice-CT)** by ElaDiDu: player, camera and draw-flag memory paths
- **[MinHook](https://github.com/TsudaKageyu/minhook)** by Tsuda Kageyu (BSD-2, vendored in `sekiro/extern/minhook`)
- **[me3](https://github.com/garyttierney/me3)**: mod loader for FromSoftware games
