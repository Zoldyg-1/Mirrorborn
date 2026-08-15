# Mirrorborn

**Nexus Mods:** [Mod page](NEXUS_MOD_PAGE_URL)

## Requirements

- Skyrim SE/AE 1.6.1170
- SKSE64 2.2.6
- Address Library for SKSE Plugins
- RaceMenu 0.4.20+

## Building

Requires Visual Studio 2022, xmake 3.0.0+, and CommonLibSSE-NG at `lib/commonlibsse-ng`.

```powershell
xmake f -m release --toolchain=msvc --skyrim_se=n --skyrim_ae=y --skyrim_vr=n --tests=n --rex_ini=n --rex_json=n --rex_toml=n --skse_xbyak=n --yes
xmake build Mirrorborn
```

Output:

```text
build/release/bin/Mirrorborn.dll
```

## Credits

- SKSE Team
- CommonLibSSE-NG contributors
- meh321 - Address Library for SKSE Plugins
- expired6978 - RaceMenu
