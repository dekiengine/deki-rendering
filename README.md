# Deki Rendering

Docs: https://dekiengine.github.io/deki-rendering/ (components and properties, generated from the code)

Core rendering pipeline for the Deki Engine: camera, render system, standard 2D renderer, render passes, and sorting callbacks.

Part of [Deki Engine](https://github.com/dekiengine/deki-engine).

## Namespace

Types live in `DekiRendering`. Scene files store the qualified name, and so does code:

```cpp
using namespace DekiRendering;
obj->AddComponent<SomeComponent>();
```

## Install

Package Manager in the Deki Editor, or `DekiEditor --packages-add deki-rendering <project>`.

## Camera

`CameraComponent` decides how much world the player sees. Orthographic, it
shows `orthoHeight` meters top to bottom and the width follows the screen: a
bigger screen of the same shape shows the same world drawn bigger, a wider one
shows more at the sides. Perspective, it shows a vertical `fieldOfView` for 3D
meshes. `pixelPerfect` scales by whole numbers for pixel art. Every screen
renders at its own resolution.

## Redrawing only what changed (`redrawChanges`)

Off by default. Turn on the project's Rendering setting `redrawChanges` and
each frame draws and sends only the parts of the screen that changed:

- Before drawing, the renderer walks the scene once without drawing
  (`DekiRenderer::FindChanges`) and compares where each object draws, and
  with what, with the last frame. An object that moved, changed, appeared or
  went away marks its old and its new place.
- Those parts, rounded out to 8 pixels, are cleared and drawn again, and no
  blit writes outside them. The display gets them through
  `Deki::IDisplay::PresentRegions`. In strips, a strip with no change is
  neither drawn nor sent.
- When the parts cover most of the screen (a scrolling camera, a full-screen
  animation), the frame is drawn whole as usual.

A render pass says where it draws with `RenderPass::ReportsChanges`; one that
does not makes every frame whole, as do a camera that does not clear, a first
frame, a resize, a clear-colour change, a swapped display and
`DekiRendering::DekiRenderSystem::MarkAllDirty()`. A component whose pixels
change in place says when with `RendererComponent::GetContentVersion`; one
that does not is drawn again every frame. With double buffering, each buffer
keeps what it still owes. Displays that cannot do partial frames keep getting
whole ones.

Editor and SDL3 displays present partial frames. The LovyanGFX display's
partial present (row bands through a small DMA staging buffer, which also
stops the byte swap for big-endian panels from mutating the engine's
framebuffer) runs on hardware, on the LilyGO T-Deck Plus.

## Whole frames or strips (`frameMode`)

The Rendering setting `frameMode` says how each frame reaches the screen on a
device. Leave it at **Auto**: when the game starts, it picks whole frames if
the framebuffer fits in internal RAM with 64 KB to spare for the game, and
strips if not. A display that cannot take part of a frame always gets whole
frames. The editor always draws whole frames.

| | Whole frames | Strips |
|---|---|---|
| How | Each frame drawn once into one framebuffer, then sent while the next is drawn | Each frame drawn a strip of `bandRows` rows at a time into a small buffer in internal RAM, each strip sent while the next is drawn |
| Faster when | The framebuffer fits in internal RAM | It does not, so it would live in PSRAM, where every blend is several times slower |
| Measured (ESP32-S3) | T-Deck Plus, 320x240, 150 KB: 61 fps in the demo's effects scene against 59 in strips | T-Display S3 AMOLED, 536x240, 251 KB: 61 fps against 22 drawing whole frames in PSRAM |
| Internal RAM | The framebuffer | A strip and two copies of it for sending: 126 KB at 536x240 and 40 rows |

Both give the same pixels. The boot log says which a board uses and why, for
example `[Rendering] whole frames: the 150 KB framebuffer fits in internal
RAM (259 KB free)`, and `DEKI:STATS` (the editor's `device_stats`) reports it
as `frameMode=whole` or `frameMode=strips bandRows=40`. Choose
**WholeFrames** or **Strips** only to override that, to measure both, say.

### Strips

There is no framebuffer in strips, which is the point when a whole one only
fits in PSRAM. Strips keep every feature whole frames have:

- Dirty rectangles: every strip is still drawn, but only the rows this frame
  or the last drew in are sent, and a strip with none is not sent. The strip
  buffer is cleared only where the strip drawn before it drew.
- A camera with `clearEveryFrame` off: the frame is kept in PSRAM, and each
  strip is read from it before drawing and written back after. The first
  frame the camera stops clearing starts from the clear colour.
- Screenshots: `DekiRenderSystem::RequestFrameCapture()` copies the next
  frame's strips into a PSRAM frame, which `GetFrameBuffer()` returns until
  the frame after. The device's `DEKI:SCREENSHOT` asks for one.
- A renderer or pass that cannot draw in strips (`DrawsInStrips()`, false by
  default for a renderer and for a pass with `BeginFrame` or `EndFrame`; the
  3D mesh pass while the scene has a mesh) gets whole frames, drawn in PSRAM,
  for as long as it cannot, and the log says so.

## Dependencies

| Dependency | Type |
|---|---|
| `deki-editor` | Deki editor DLL |

## Community

Questions, ideas and what you made with Deki: [the Deki Discord](https://dekiengine.com/discord/).

## License

Apache 2.0. See [LICENSE](LICENSE).
