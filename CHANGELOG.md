# Changelog

Notable changes to `deki-rendering`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Changed
- **Names follow the code style** (deki-engine/docs/codestyle): types, functions and enum values are PascalCase, constants kPascalCase, members m_PascalCase, locals and parameters camelCase. The code is formatted with clang-format 22.
- The functions the editor finds by name are PascalCase: DekiRenderingRegisterComponents, DekiRenderingGetAutoComponentCount, DekiRenderingEnsureRegistered and the rest. Built against engine ABI 21; a build of this package from before does not load and is rebuilt.
- Renamed: `kInterfaceID`, `kRendererTypeID`, and the QuadBlit row functions (`RGB565A8BlendRow`, `RGB565CopyRow`, ...).

### Removed
- The camera migration from `pixelsPerMeter`, `zoom` and `pixelSnap` to
  `orthoHeight` and `pixelPerfect`.
- The former names from before 0.16.0 (bare class names, and deki-gpio's
  `DekiEsp32::ESP32PinSetup`). A scene that old is upgraded with 0.17 first.

## 0.17.0

### Added
- `DekiRendering::CurrentDrawView()`: the view being drawn (its pixels per
  meter and size), set by the renderer around a frame, so a component that
  bakes pixels can bake them at the density they are drawn at.

### Changed
- `minEngine` 0.17.0. Reflection ABI 20: the package must be rebuilt.
- The per-format pixel reads and writes (`SrcKind`, `ReadSrcPixel`,
  `ReadDstPixel`, `WriteDstPixel`) moved from QuadBlit.cpp into the public
  `PixelFormat.h`, so deki-3d shares them. Output is unchanged.
- **The camera frames the world, on every screen.** `CameraComponent` shows
  `orthoHeight` meters top to bottom (Orthographic) or a vertical
  `fieldOfView` (Perspective); the width follows the screen. A bigger screen
  of the same shape shows the same world, drawn bigger. `pixelPerfect` scales
  by whole numbers and puts the camera and every draw position on the
  art-pixel grid.
- The camera has a `projection`: Orthographic, or Perspective with
  `fieldOfView`, `nearPlane` and `farPlane` (these moved here from deki-3d).
- Scenes convert on load. A 0.16 camera's `pixelsPerMeter` p shows
  `p / project pixels per meter` times as close; `orthoHeight` is the
  project's old Framebuffer height (in meters, `targetHeight / pixels per
  meter` for a 0.16 project) divided by that, so the picture is the same on a
  screen of the old shape. `pixelSnap`, or the old project's Pixel Perfect,
  becomes the camera's `pixelPerfect`.
- The camera's inspector says what it shows on the previewed screen, and its
  gizmo draws that.

### Fixed
- `QuadBlit::RegisterKernel`/`GetKernel` checked an unsigned id for being
  below zero, an always-false comparison that ESP-IDF 6's GCC 15 build rejects.

## 0.16.0

### Fixed
- `DekiRendering_InitSystem` and `DekiRendering_ShutdownSystem` stay at global
  scope, for the same reason as the equivalent pair in deki-input: a generated
  translation unit names them and cannot know a package's namespace.
  `DekiRendering_DetachPass` is a normal API and stays in the namespace.

### Changed
- **Moved into the `DekiRendering` namespace.** Every component was declared at global
  scope, which made its identity a bare class name — the name a scene file
  stores and the name the registry keys on — so two packages defining one name
  collided there with nothing to tell them apart. Each component carries
  `DEKI_FORMER_NAME` with the name it was saved under before, so existing
  scenes load unchanged and are written back qualified on the next save.
  Code naming these types needs the namespace: `using namespace DekiRendering;` or a
  qualified name.
- Enum properties are stored by name rather than by number, so appending to an
  enum or reordering one no longer changes what a saved scene means. Files
  written before this still read.
- `minEngine` 0.16.0. Reflection ABI 17: the package must be rebuilt.

## 0.15.0

### Changed
- The display format is a `Deki::ColorFormat` rather than an `int`.
- Blit sources are built from a named `PixelLayout` with factory functions per
  format instead of positional booleans; 49 call sites were migrated and the
  positional form is gone.
- A transferred pixel buffer is released through `Deki::Memory`, and sources
  borrow their pixels by default rather than taking ownership implicitly.

### Removed
- The `clip2d` warning, which no pass had provided for a long time.
