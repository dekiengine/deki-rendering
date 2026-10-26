# Changelog

Notable changes to `deki-rendering`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Changed
- **No clear under a full-screen background.** A whole frame was cleared to
  the camera's colour and then drawn over. `Standard2DRenderer` now clears
  just before its first draw, and not at all when that draw is opaque and
  covers the screen: 0.49 ms a frame on a T-Deck Plus. Passes can write
  anywhere, so the clear comes before any of them runs (an Execute pass only
  for an object it `WantsObject`). A renderer takes this on with
  `DekiRenderer::ClearsLazily()` and `RenderContext::clear`.
- **See-through sprite edges draw faster.** An untinted sprite with soft
  edges blended them through the general per-pixel pipeline; it now has its
  own loop, which also skips each row's transparent run without reading it.
  Its blits in the demo's meadow take 1.29 ms instead of 1.97 ms on a T-Deck
  Plus (render 5.9 to 5.1 ms), with the same pixels. Blends onto RGB565 mix
  red and blue in one word: two multiplies fewer a pixel, the same result.

### Added
- `QuadBlit::Source::stretchWidth` / `stretchHeight`: one column or one row
  drawn as the whole picture it stands for, placed, scaled, clipped and rotated
  as that picture would be. A plain opaque one onto RGB565 is filled row by row
  (`StretchRows` in the profiler) instead of sampled pixel by pixel.
  `QuadBlit::DrawnWidth` / `DrawnHeight` give the size a source is drawn as.

## 0.20.0

### Added
- Strips along a display's refresh. A display that wants it offers
  `IScanOrderedDisplay` (`RegisterScanOrderedDisplay`): the order its panel
  refreshes the screen in, rows down or up, or columns left to right or
  right to left. `DekiRenderSystem` then cuts strips along it, in that order,
  and sends each with `PresentStrip`: the whole strip, what changed in it,
  and whether it is the frame's first. A strip of columns is as many pixels
  as `bandRows` rows would be, in whole eights of columns so the ordered
  dither's tile stays where the whole frame has it. `RenderContext` gains
  `viewLeft` and `viewWidth`, the x twins of `viewTop` and `viewHeight`, and
  `DrawsInStrips` means strips of rows or of columns. Every whole-frame
  feature works in either: dirty rectangles, a camera that does not clear,
  screenshots. `Deki::IDisplay` does not change, so no package is rebuilt for
  it.
- Big-endian RGB565 (`DEKI_RGB565_BIG_ENDIAN`, see `Deki::kRGB565BigEndian`):
  `DekiPixel::Load565`, `Store565`, `ToMemory565`, `FromMemory565` and
  `FileToMemory565` read and write 565 values in memory's byte order. The
  blitter, its tint tables and chroma keys, clears and `GetPixel` use them.
  Copies of whole pixels are unchanged. The golden tests run in both byte
  orders and give the same hashes.
- `RenderProfiler` times each of QuadBlit's paths (its fast loops and the
  generic scaled and rotated pipelines): blits, destination pixels and
  microseconds per frame (`AddPath`, `TakePaths`). Entries carry the
  component's short name, which a stripped build's registry cannot give.

### Changed
- `minEngine` 0.20.0: built against engine 0.20.0. A build of this package from before does not load and is rebuilt.
- On the ESP32's Xtensa cores an RGB565 pixel is read and written with one
  16-bit access, aligned or not (`DekiPixel::LoadU16` and `StoreU16`). The
  cores handle unaligned loads and stores in hardware; GCC assumes they
  fault, so the byte-by-byte read it replaced was four instructions. Other
  targets use `memcpy`.

### Removed
- `halfWidthFramebuffer` and `interlaced60hz` from the Rendering settings:
  neither was ever built, though their tooltips described them as if they
  were. A project that still has them in its settings loads as before; they
  are left out when it is saved.

## 0.19.0

### Changed
- `minEngine` 0.19.0: built against engine 0.19.0, whose component metadata records the package each class is in. A build of this package from before does not load and is rebuilt.

### Added
- Japanese, Simplified Chinese and Korean: `editor/lang/ja.json`, `zh.json`
  and `ko.json` translate the names, tooltips and enum values this
  package's components show in the Inspector.
- The package's editor text goes through the editor's translation (`Tr`), so
  it shows in the editor's language. Needs the editor release that adds
  `Localization.h`; raise `minEngine` to it when releasing.

## 0.18.2

### Added
- `RenderProfiler`: once enabled, the draws, content time, blit time and
  pixels per frame of each component type, and the frame's clear. The ESP32's
  `DEKI:PROFILE` console request reports it.

### Changed
- **Blits are several times faster on the ESP32.**
  - A pixel is read from RGB565 sources byte by byte. With `memcpy`, which
    GCC makes a function call on the ESP32's Xtensa when the pixel may be
    unaligned, every pixel cost a call.
  - A 1:1 sprite drawn onto RGB565 with a tint or alpha tint runs in one loop
    per row, with the tint worked out once for each of RGB565's channel
    values. It went through the per-pixel pipeline.
  - So does a sprite drawn onto RGB565A8 scaled or tinted: each particle into
    its emitter's composite. The demo's two fountains took 3.6 ms a frame
    on a LilyGO T-Deck, now 2.4 ms.

  The pixels are the same as before.

## 0.18.0

### Added
- The render system places Screen Anchor objects for its camera and screen
  before each frame, so a HUD stays in the screen's corners on any screen size.

### Changed
- `minEngine` 0.18.0. Reflection ABI 21: the package must be rebuilt.
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
