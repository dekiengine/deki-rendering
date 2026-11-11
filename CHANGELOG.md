# Changelog

Notable changes to `deki-rendering`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Added
- **The profiler breaks a frame down by object.** While it is on, each
  renderer, and each render pass on each object, has a row
  (`RenderProfiler::TakeObjects`): its path and type, its share of finding
  what changed, of making and of drawing its pixels, how many times it was
  drawn, and why its area was drawn again (new, changed, still on screen while
  the world scrolled, drawn over a pin). Off, nothing is counted. The
  **Render Profiler** window (Tools > Rendering) shows the rows for the Play
  view, or for a board on a serial port. Render passes name themselves for it
  (`RenderPass::Name`, "Render pass" unless they say otherwise).
- **Redraw Changes moves the last frame when the camera scrolls.** A camera
  that moved by whole pixels, and sits on whole pixels, used to redraw the
  whole frame; now the frame's pixels are moved and only the edge it uncovers
  and what changed besides are drawn. Frames match ones drawn whole. A scene
  with 2D lights, a frame drawn in strips, a camera between pixels, and a
  render pass that reports its own changes but not
  `RenderPass::FollowsScroll` still redraw whole frames.
  `DekiRenderer::ScrolledBy` and `RenderContext::scrollable`, `scrollX` and
  `scrollY` carry it; a custom renderer needs no change. New virtuals on
  `DekiRenderer` and `RenderPass`: packages built against the old headers
  must be rebuilt.
- **What stays on screen keeps its pixels while the camera scrolls.** A
  renderer drawn the same at the same place on screen, such as a HUD panel
  under the camera, keeps the pixels of its opaque middle as the frame moves
  (`DekiRenderer::ScrollPins`); only its see-through edge and the band it
  leaves are drawn again. Anything drawn over it that moved redraws it.
- **A drawing's area is only what it can write.** A sprite, shape or other
  renderer drawn 1:1 and unturned counts only the box of its non-transparent
  pixels as changed: an animation frame's empty margin is not redrawn.
  `QuadBlit::VisibleBounds` and `QuadBlit::OpaqueInterior` work them out;
  they are kept, by pixels and revision, in a small table in external
  memory. Pixels with no `Source::revision` are not trimmed.
- **Changed parts close together are drawn as one.** Each part redrawn walks
  the scene and sets up every render pass again, so parts are merged while a
  merge adds fewer pixels than that costs (`DirtyRegion::MergeCheaperThan`,
  `DekiRenderSystem::SetPartCostPixels`, 4096 by default).
- `DeferredClear::colour` and `QuadBlit::PendingClearColour`: the colour of
  a clear still pending, whatever it covers, for a render pass about to write
  every pixel the clear covers in a frame or a redrawn part of one, which then
  drops the clear (`DeferredClear::pixel` stays for whole frames only).
  `DekiRenderSystem` and the lit clear of 2D lights give it.
- `QuadBlit::SwapClipState` sets the clip stack, the target limit and the
  clip switch aside, for a pass drawing into a buffer of its own in the
  middle of a frame.
- **Finding what changed reuses what it measured.** A renderer can give a
  draw key (`RendererComponent::GetDrawKey`): a value from its own fields
  that changes whenever its content could. While the key, the object's scale
  and its turn stay the same, what was measured of its content (its size,
  pixels, area and the signature they make) is reused, and only where it is
  drawn is worked out again; a renderer with no key is measured in full each
  frame as before. Kept in a table in external memory. A new virtual on
  `RendererComponent`: packages built against the old headers must be
  rebuilt. On a LilyGO T-Deck, finding the changes of the "Your first game"
  farm went from 7.6 to 6.2 ms a frame.

### Changed
- **A frame redrawn in parts costs less with many sprites moving.** On a
  LilyGO T-Deck, the "Your first game" farm with the camera following the
  farmer and villagers walking about went from 18.1 to 16.6 ms a frame with 4
  villagers, 20.6 to 16.7 ms with 8 (both now the display's 60 fps) and 26.3
  to 18.6 ms with 16. The pixels are the same: checked on the board, each
  frame drawn in parts against the same frame drawn whole. What changed:
  - A sprite drawn 1:1 and unturned blits only its box of non-transparent
    pixels, as only that box already counted as its area. An animation
    frame's empty margin is no longer read.
  - What is known of a source's pixels (its visible box, its opaque
    interior) is kept in a few places for each source, not one: sources that
    shared a place pushed each other out every frame, and each time the
    pixels were walked again.
  - Each part passes over the objects outside it from a short list of their
    areas, made as the frame's plan is built, without reading the plan.
  - A renderer's content is kept beside what is reused of it from frame to
    frame, not copied into the plan for every object every frame.
  - `DirtyRegion` keeps the sum of its areas as rectangles are added, and
    merging looks again only at the pairs a merge changed. The pairs merged
    are the same (new tests compare them with comparing every pair again).
  - A scrolled frame's rows find the pins they cross only where a pin
    starts or ends.
- `DirtyRegion::Add` joins a rectangle to one already there when their
  bounding box is no larger than the two together: a sprite that moved a
  pixel is one rectangle, not two. Above the merge threshold the set is
  merged down to a quarter under it, not to it, so the next rectangles are
  added without merging again.
- Parts are drawn as one when closer than 2048 pixels of extra area
  (`kPartCostPixels`, was 4096): a part costs less than it did.
- The frame's plan and its lists are in external memory (`ExternalAllocator`)
  where the target has it. They came from the C library, which on a board
  put them, or the sizes they grew through, in internal RAM.- Redrawn parts cost less. Taking what stays on screen out of what changed
  allocates nothing (1.85 to 0.2 ms a frame on the T-Deck farm), and a part
  passes over an object outside it that has nothing else to draw before
  setting anything up for it.
- Finding what changed no longer allocates every frame. `DirtyRegion::Align`
  folded its rectangles back through a new list each time and threw the old
  one away, so the region grew its list again from nothing: five allocations
  a frame on a T-Deck. It keeps both lists now.

### Breaking
- **Positive rotation now draws counter-clockwise.** `QuadBlit` applied the
  angle in screen space, where Y runs down, so a sprite turned clockwise while
  its children, lights, 3D and physics turned counter-clockwise. A rotated
  sprite now turns the same way as the rest of the engine, and a child stays
  fixed to a rotated parent. A scene or script that set a rotation by eye
  needs the sign of that rotation flipped.
- **A turned sprite no longer moves by a pixel.** `QuadBlit` read each pixel
  of a rotated draw at its corner instead of its centre, so a sprite turned
  by a half turn or a quarter turn landed one pixel right of, or below, where
  it stands unturned. It now covers the same pixels, as Tiled and other
  editors draw it. Rotated sprites placed by eye may sit a pixel off.
- **A scaled sprite reads the source pixel under each drawn pixel's middle.**
  `QuadBlit::BlitScaled` read the one under its left or top edge. A sprite
  drawn at a whole multiple of its size looks the same; one shrunk, or grown
  by a fraction, shows some different edge pixels: a 6-pixel strip drawn 2
  wide now shows its second and fifth pixels, not its first and fourth.
  Exactly between two pixels it takes the first, so halving still takes every
  other pixel from the first. This matches turned sprites and Tiled. The cost
  is the same.

### Fixed
- The profiler's kinds of component kept no name for draws whose content
  was reused, so a board's `DEKI:PROFILE` listed them as "?": `AddBlit` takes
  the name too.

## 0.30.0

### Declares the capabilities it uses
- `package.json` has `"capabilityDefines": ["opengl"]` and
  `"filtersTexturesWith": ["opengl"]`: its GPU code tests
  `DEKI_CAPABILITY_OPENGL`, and its GL sprite sink draws textures filtered.
  The editor reads them instead of knowing `opengl` itself; needs the editor
  from this release.

### Changed
- `minEngine` 0.30.0: built against engine 0.30.0. A build of this package
  from before does not load and is rebuilt.
- **Lights are a feature** (`lights`, `DEKI_FEATURE_LIGHTS`, the Light
  component): a device build whose scenes and code have no Light leaves it
  out. Light.cpp is not compiled, nothing is lit, and the light loops stay in
  flash: such a game spends no internal RAM on lights (a T-Deck build had
  1.4 KB of their code there, and 4.2 KB of tables, below). Code that uses
  Light includes Light.h inside `#ifdef DEKI_FEATURE_LIGHTS`;
  `RenderingFeatures.h` turns it on in a build that does not configure
  features.
- The 2D light's colour tables (about 2 KB for each thread that lights) are
  allocated by the first frame lit, not held static: a game without lights
  keeps that internal RAM (4.2 KB on a T-Deck, where a second core lights
  too). With no memory for them the frame is drawn unlit, and the log says
  so once.
- The frame's clear waits for its first write, wherever that is: the
  renderer leaves it with QuadBlit (`SetPendingClear`), a blit into the
  buffer runs it first, and so must a render pass that writes into the buffer
  itself and says it does not (`RenderPass::WritesBuffer` false): it now
  calls `QuadBlit::BeforeWriting(ctx.buffer)` before it writes, as well as
  `NoteWritten` after. Such a pass that does not would have its pixels
  cleared over. Before, any pass with a hook had the clear made first.
- A frame holding nothing but its clear when the 2D light is due, and lit
  the same all over (`Lighting2D::Uniform`: no point or spot light in view),
  is cleared in the clear colour lit (`DeferredClear::runLit`) instead of
  cleared and lit after, still when the frame is first written. The same
  pixels. A 3D scene lit by an ambient light and the sun saves the 2D light's
  pass over the screen: 6.8 ms a frame on a T-Deck.
- A frame that cannot be drawn in strips (a 3D scene's) is drawn in a frame
  the display lends (`IScanOrderedDisplay::LendFrame`) when every pixel of
  it is drawn anew (the camera clears every frame, changes are not redrawn
  on their own, no screenshot is wanted), and goes to the display as it is.
  On a T-Deck that is the 5 ms copy of each frame gone: a 3D corridor of
  2,000 triangles at 19.5 frames a second, from 17.
- A render pass about to write every pixel of the frame itself can take the
  pending clear as one pixel (`QuadBlit::TakePendingClear`,
  `DeferredClear::pixel`), lit when the light came first, and draw it where
  it draws nothing: deki-3d's software rasteriser does, which saves the
  clear's own pass over the screen.
- The shader loader is registered once, as "Shader"
  (`ShaderAsset::kAssetTypeName`). Its second name, "ShaderAsset", was there
  because the reflection named an `AssetRef<ShaderAsset>` by its class; it no
  longer does (engine changelog).

### Added
- `GL::GenerateMipmap`, `gl::LINEAR_MIPMAP_LINEAR` and
  `gl::NEAREST_MIPMAP_NEAREST`, for filtered textures.
- A source's filter (`QuadBlit::Source::filter`: Point, Bilinear,
  Trilinear, the image's setting). `GLBlitSink` draws Bilinear blended
  between the four nearest texels, and Trilinear also between mipmaps by how
  small it is drawn, kept within each source so a frame or tile never takes
  in its neighbour's texels; a texture gets its mipmaps when a Trilinear
  source first draws from it. The CPU draws Point, as before.
- Lights: the `Light` component (Ambient, Directional, Point, Spot; colour,
  intensity, range, a spot's angle and softness, a sun's yaw and pitch, 2D
  bands, shadows). Every active Light in the scene lights deki-3d's meshes and
  the 2D world; a scene with none is lit as before, not at all.
- The 2D light, `Lighting2D`: worked out per 4x4 block, blended and dithered
  between blocks where it changes, up to twice a colour, laid over the frame
  by `Standard2DRenderer` before the first object above the camera's
  `litUpToSortOrder` (new; the default lights everything) or before a
  renderer that `LightsItself()` (new, on `RendererComponent`). Strips and
  redraws of part of the frame are lit as the whole frame, and a changed
  light is a changed region. On a device both cores light the pixels.
- On the GPU the 2D light is a full-screen pass, per pixel
  (`QuadBlit::BlitSink::Light`, `QuadBlit::LightSinkFrame`, the presenter's
  2D light program, HLSL in `editor/Light2DShader.cpp`, compiled into
  `GLLight2DShader.inl`). A sink that cannot light says so, once.
- In the editor: the Light inspector and its gizmos (a point's reach, a
  spot's cone), and a check of the scenes a build ships that lists each
  shadow-casting light a board without OpenGL draws without its shadows.
- OpenGL, where the platform provides `opengl` (its build then defines
  `DEKI_CAPABILITY_OPENGL`; a board never does and compiles none of it): the
  GL loader and shared context (`GLApi.h`), a presenter that builds each frame
  on the GPU (`GLPresenter`), the 2D drawn there too (`GLBlitSink`), and
  `ScreenEffect`, a full-screen shader over the finished frame. Types live in
  `DekiGL`.
- `ShaderAsset` (`.shader`): a GPU shader, written in HLSL. The editor
  compiles it (Slang, then SPIRV-Cross) to GLSL ES 3.00 and GLSL 3.30 for each
  use whose hooks it defines (`editor/ShaderUses.h`: deki-3d's meshes, screen
  effects), and the asset is that GLSL. Errors name the file and line, in the
  Console and in a build's problems; a build for a target without OpenGL that
  ships a scene using a shader stops. The editor lists it as the Shader asset
  type.
- A blit sink (`QuadBlit::BlitSink`), so a GPU that builds the frame
  (`GLPresenter`) can draw its 2D. While a sink frame is on
  (`QuadBlit::BeginSinkFrame`), every `Blit` and `BlitScaled` into that
  frame's buffer goes to the sink, worked out as the CPU would draw it
  (`SinkBlit`: the clipped rectangle and the 16.16 steps it samples with), and
  nothing reaches the buffer. Without a sink a blit tests one pointer more and
  nothing else changes.
- `DekiRenderSystem` draws the whole frames of a camera that clears into a
  sink that takes them: the clear is the sink's, and the buffer starts empty
  (transparent) and collects only what code writes into it directly.
  Redrawing only what changed is skipped then, as the sink's frame starts
  over each time. Strips and a camera that keeps its frame stay on the CPU.
- `RenderPass::WritesBuffer`: whether a pass writes into the buffer other than
  through QuadBlit. On a sink frame the renderer tells the sink after each
  hook of a pass that does (`QuadBlit::NoteWritten`), so what it wrote is
  composed in its place in the draw order. Yes by default; a pass that draws
  only through QuadBlit says no and saves a look through the buffer.
- `QuadBlit::Source::revision` and `QuadBlit::NextRevision()`: a number for a
  source's pixels, new whenever they are written, so a sink keeps its upload
  until it changes. `ForgetScaled` tells the sink too.

### Changed
- QuadBlit's row loops stay where they are meant to on an ESP32. Each one
  `Blit` or `BlitScaled` calls is in IRAM and never inlined into those
  callers, which run from flash; before, the compiler inlined some of them and
  dropped their IRAM copy, and which ones changed with any edit nearby (the
  1:1 alpha blend of RGB565A8 sprites ran from flash). The general per-pixel
  path (`BlitRows`) is in flash, as it was in every build: in IRAM it took
  12 KB and drew Wyrdmoor on a T-Deck no faster. IRAM on a T-Deck build:
  2.4 KB more.

## 0.22.0

### Changed
- The frame buffer and the render strip are counted in the engine's memory
  figures. They came from `Memory::AllocateInternal`, which counts nothing, so
  the largest buffer on a board was missing from every total and from the
  editor's Memory panel. They now come from `Memory::Allocate` in the Internal
  region, as before in internal RAM.
- `minEngine` 0.22.0: built against engine 0.22.0. A build of this package
  from before does not load and is rebuilt.

### Fixed
- A Linux desktop game no longer crashes as it exits. A package's render pass
  (deki-particles) was detached from a pass list that static destruction had
  already freed.

## 0.21.0

### Changed
- `minEngine` 0.21.0: built against engine 0.21.0. A build of this package
  from before does not load and is rebuilt.
- **Redraw only what changed** (project setting `redrawChanges`, off by
  default; `DekiRenderSystem::SetRedrawChanges`). Before drawing, the
  renderer compares each object with the last frame: where it lands and what
  decides its pixels (content and its version, size, pivot, tint, scale,
  rotation, clip, place in the draw order). Only the parts that differ are
  cleared, drawn and sent; a frame where nothing moved costs only the
  comparison. Past 75% of the screen, or when something cannot say what it
  draws, the frame is drawn whole as before. Whole frames and strips both do
  it, and a display that alternates buffers gets every change since each was
  drawn. On a T-Deck Plus the demo's menu went from 7.6 ms render and 3.4 ms
  send to 2.0 ms and 0.1 ms; a fully animated scene costs what it did. The
  editor's Play view does it too.
  - `DekiRenderer::FindChanges` and `RenderContext::redraw` (Standard2DRenderer
    implements them). `RendererComponent::GetContentVersion`: a component
    whose pixels change in place says so; one that does not counts as
    changed every frame it draws. `RenderPass::ReportsChanges`,
    `AddChanges` and `FinishChanges`: a pass that cannot say where it draws
    makes every frame whole, as does a pass with hooks other than Execute or
    a camera that does not clear.
  - `QuadBlit::SetTargetLimit`: a rect every blit stays inside, even with
    clipping off (`ignoreClip`), so nothing writes outside the part redrawn.
  - The profiler's `Changes` entry times the comparison.
  - **Packages built against an older deki-rendering must be rebuilt**:
    `RendererComponent` and `RenderPass` have new virtual functions.
- Drawing in strips keeps each object's content with its place in the
  frame's plan instead of in a map rebuilt every frame.
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

- **Turned sprites draw faster.** A rotated blit visits only the pixels
  of its box that land inside the sprite, worked out once a row, instead of
  testing every pixel of the box. An RGB565 or RGB565A8 sprite turned onto
  an RGB565 frame, tinted or faded, has its own loop (`TintRotatedRows` in
  the profiler) instead of the per-pixel pipeline: the demo's spinning gear
  takes 0.67 ms instead of 0.87 ms on a T-Display S3, with the same pixels.
  Other rotated blits (a colour key, dithering, flips, other formats) run
  from flash, so they take no internal RAM.
- Scaled sprites and particles onto RGB565 find each source pixel with a
  shift instead of a multiply (`TintScaledRows`): 5% faster.

### Added
- `Fingerprint.h`: a 64-bit fingerprint over 32-bit words with 32-bit
  multiplies only, for telling whether something changed since the last
  frame. A 32-bit microcontroller has no 64-bit multiply.
- `QuadBlit::Source::stretchWidth` / `stretchHeight`: one column or one row
  drawn as the whole picture it stands for, placed, scaled, clipped and rotated
  as that picture would be. A plain opaque one onto RGB565 is filled row by row
  (`StretchRows` in the profiler) instead of sampled pixel by pixel.
  `QuadBlit::DrawnWidth` / `DrawnHeight` give the size a source is drawn as.

### Removed
- **Dirty-rect tracking** (project settings `dirtyTileTracking` and
  `dirtyTileSize`, `QuadBlit::MarkDirty`, `SetDirtyTracking`,
  `DekiRenderer::trackDirty`, `GetLastFrameDirty`). `redrawChanges` does the
  same job and also skips the drawing. A project's old keys are ignored; turn
  on `redrawChanges` instead.

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
