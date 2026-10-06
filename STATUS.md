# ArchiveXL macOS port status (Cyberpunk 2077 2.3.1, Epic, arm64)

## Works (verified in game)
- RED4ext macOS fork, inline hooks (re-signed executable), plugin loading.
- Stages 1 and 2: mounts `Bundle/` and `archive/pc/mod`, discovers and reads `.xl` files.
- Archive-only mods (meshes, textures, refits) load correctly.

## Key facts
- Mac layout: DynArray = {entries, capacity, size}; ResourceDepot groups at +0x10 (0x38 per element).
- `Main(Load)` is called twice by the loader, so there is a guard. The depot pointer is captured in `Red::Mac::g_depot`.
- Only ResourceMeta and ArchiveService are active on Apple.

## Not working yet (needs unlocated functions)
ResourceLink, ResourcePatch, Mesh, FactoryIndex, Localization, Journal, Animation, Transmog, Attachment, Customization, Garment, PuppetState, QuestPhase, WorldStreaming, InkSpawner.

## In progress
External ink widget spawning: the widget library lookup is hooked to inject the dependency and the widget draws, but the script controller is not initialized (`MacCompat.cpp`). Diagnostics log to `/tmp/axl_*.log`.
