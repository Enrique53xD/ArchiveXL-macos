# ArchiveXL-macos (macOS port)

Apple Silicon port of the upstream project, built on [RED4ext-macos](https://github.com/Enrique53xD/RED4ext-macos). See [cp2077-macos-tools](https://github.com/Enrique53xD/cp2077-macos-tools) for the full install guide.

**Build:** clone next to `RED4ext-macos` and `ArchiveXL-macos`, then `mkdir build && cd build && cmake .. && make -j8`, `codesign -f -s - ArchiveXL.dylib`.
**Install:** copy the dylib to `<game>/red4ext/plugins/ArchiveXL/` with an empty `rtti_experiment` file beside it.
**Active on macOS:** archive mounting (`Bundle/` and `archive/pc/mod`), `.xl` discovery, and dynamic appearance names for items (template and mesh appearance lookups). Other extensions are disabled until their game addresses are located. See `STATUS.md`.
Original README below.

---

# ArchiveXL

ArchiveXL is a modding tool that allows you to load custom resources without touching original game files,
thus allowing multiple mods to expand same resources without conflicts.

With the mod you can:

- Load custom entity factories (necessary for item additions)
- Add localization texts that can be used in scripts, resources and TweakDB
- Edit existing localization texts without overwriting original resources
- Override submeshes visibility of entity parts
- Add visual tags to a clothing item
- Spawn widgets from any library without registering dependencies

## Getting Started

### Compatibility

- Cyberpunk 2077 2.31
- [redscript](https://github.com/jac3km4/redscript) 0.5.31+

### Installation

1. Install requirements:
   - [RED4ext](https://docs.red4ext.com/getting-started/installing-red4ext) 1.29.0+
2. Extract the release archive `ArchiveXL-x.x.x.zip` into the Cyberpunk 2077 directory.

## Documentation

- [Dynamic appearances](https://github.com/psiberx/cp2077-archive-xl/wiki#dynamic-appearances)
- [Body types](https://github.com/psiberx/cp2077-archive-xl/wiki#body-types)
- [Appearance suffixes](https://github.com/psiberx/cp2077-archive-xl/wiki#appearance-suffixes)
- [Components overrides](https://github.com/psiberx/cp2077-archive-xl/wiki#components-overrides)
- [Visual tags](https://github.com/psiberx/cp2077-archive-xl/wiki#visual-tags)
- [Extending resources](https://github.com/psiberx/cp2077-archive-xl/wiki#extending-resources)
