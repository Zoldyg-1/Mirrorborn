# Mirrorborn - Copy NPC Appearance to the Player

Mirrorborn is an SKSE plugin for Skyrim Special Edition / Anniversary Edition **1.6.1170.0**.

Select an NPC in the console and copy their appearance to the player. Inventory, stats, factions, voice, name, and other gameplay data are not copied.

Mirrorborn currently supports Skyrim **1.6.1170.0 only**.

## Requirements

* Skyrim SE/AE **1.6.1170.0**
* SKSE64 **2.2.6**
* Address Library for SKSE Plugins
* RaceMenu **0.4.20 or newer**
* Microsoft Visual C++ Redistributable (x64)

GOG, Skyrim VR, 1.5.97, and older 1.6.x runtimes are not supported.

## Installation

Install the release archive normally with Mod Organizer 2, Vortex, or another mod manager.

The plugin is installed to:

```text
SKSE/Plugins/Mirrorborn.dll
```

Mirrorborn does not use an ESP or ESL and has no load-order position.

## Usage

1. Load a save and make sure the player is not mounted.
2. Open the console and select the NPC whose appearance you want.
3. Enter:

```text
skee copy
```

Available commands:

```text
skee copy      Copy the selected NPC's appearance
skee revert    Restore the player's appearance
skee status    Show plugin status
skee help      List registered SKEE commands
```

Selecting the player and entering `skee copy` also reverts the active copy.

The player and donor may be different races, but they must be the same sex.

An active copy is stored in the SKSE co-save and reapplied when the save loads.

Run `skee revert` before using `showracemenu`, changing the player's race or sex, updating/removing Mirrorborn, or making your final uninstall save.

## What is copied

Mirrorborn copies the NPC's engine-native appearance, including:

* Face and FaceGen data
* Head parts, hair, and facial tints
* Skin and body selection
* Race skeleton
* Weight
* Effective world scale
* Matching first-person and third-person skeleton-node transforms

Vanilla war paint, dirt, makeup, and other normal FaceGen tints are also copied.

RaceMenu overlays, BodyMorphs, sculpt data, and other per-reference RaceMenu data are not copied.

Inventory, equipment, stats, factions, voice, name, AI packages, spells, and other gameplay data are not copied.

## Compatibility

The normal adult vanilla playable races are supported, including humans, elves, Orcs, Khajiit, and Argonians.

Child and creature races are not supported.

Custom races may work if they use Skyrim's standard `defaultmale` or `defaultfemale` behavior graph and have a valid skeleton, but compatibility is not guaranteed.

Copying another race's appearance does not permanently change the player's gameplay race. Racial abilities and other character data remain unchanged.

## Troubleshooting

Run:

```text
skee status
```

A working installation should report the hooks and command interface as ready.

The log file is located at:

```text
Documents/My Games/Skyrim Special Edition/SKSE/Mirrorborn.log
```

## Building

Mirrorborn uses **xmake**, MSVC, and C++23.

Requirements:

* Visual Studio 2022 with the C++ toolchain
* xmake 3.0.0 or newer
* CommonLibSSE-NG

Configure and build:

```powershell
xmake f -m release --toolchain=msvc --skyrim_se=n --skyrim_ae=y --skyrim_vr=n --tests=n --rex_ini=n --rex_json=n --rex_toml=n --skse_xbyak=n --yes
xmake build Mirrorborn
```

The compiled DLL is written to:

```text
build/release/bin/Mirrorborn.dll
```

## Credits

* SKSE Team
* CommonLibSSE-NG contributors
* meh321 - Address Library for SKSE Plugins
* expired6978 - RaceMenu
