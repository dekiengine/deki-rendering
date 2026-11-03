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

## 2D on a GPU (`QuadBlit::BlitSink`)

Where the display builds its frames on a GPU (the GL presenter, below, in a
desktop build), the 2D can be drawn there too. The package that has a GPU
registers a sink; `DekiRenderSystem` asks it to take each whole frame of a
camera that clears, and while it does, QuadBlit hands it every blit into the
frame's buffer, placed and clipped exactly as the CPU would draw it, instead
of drawing it. QuadBlit knows nothing of the GPU, and without a sink it
changes nothing.

The frame's buffer then holds only what code writes into it directly, which
the sink composes in its place in the draw order:

- a render pass that writes into `ctx.buffer` itself (pixels by hand, a
  software rasteriser) is followed by `QuadBlit::NoteWritten` after each of
  its hooks; a pass that draws only through QuadBlit says so
  (`WritesBuffer()` false) and costs nothing;
- what a component hands QuadBlit is uploaded once and kept: give its source a
  `revision` (`QuadBlit::NextRevision()`, new each time the pixels are
  written), or mark pixels that never change `cacheScaled` and call
  `ForgetScaled` before freeing them. A source with neither is uploaded again
  every blit, which is right, only slow.

A screenshot of the buffer (`GetFrameBuffer`) misses what the sink drew; the
sink's frame has it (`GLPresenter::ReadFrame`). So does a pass
that reads the buffer back to work on the picture.

## OpenGL

Where the platform provides `opengl` (the desktop; a Linux handheld once its
platform exists), the build defines `DEKI_CAPABILITY_OPENGL` and this package
carries Deki's OpenGL code: the context the program draws in, a presenter
that builds each frame on the GPU, 2D included, and `ScreenEffect`. On a
board without OpenGL the flag is never defined and none of it is compiled.
deki-3d draws its meshes with it, and `deki-sdl3-integration` presents
through it. Its types live in `DekiGL`.

> **Experimental.** Run on Windows (desktop OpenGL 3.3, NVIDIA) in the editor
> and a desktop simulator build, and on Linux (OpenGL ES 3.0 through EGL,
> Mesa) in its tests. Not yet run on a handheld.

### The context

OpenGL is loaded at run time, not linked, so nothing needs GL headers or
libraries to build, and a machine without a driver still runs the game on the
CPU. One context serves the whole program (`SharedContext()`), because what is
made in one context cannot be used in another:

- a **window's**, when a display presents through OpenGL: it makes the context
  and hands it over (`GLContext::Adopt`);
- otherwise an **off-screen** one made on first use: WGL on a hidden window on
  Windows, EGL with a 1x1 pbuffer or no surface at all on Linux.

The calls are the common ground of OpenGL ES 3.0 and desktop OpenGL 3.3 core,
so one code path serves a handheld's Mali and a desktop.

### The presenter

The presenter builds each frame on the GPU and shows it:

1. **2D**: the 2D renderer's blits are drawn straight into the frame (see
   below), unless that is turned off; then the 2D is drawn on the CPU into
   its frame buffer and arrives as a layer.
2. **`PushLayer`**: what a frame buffer holds goes up as a texture and over
   the frame being built: the 2D drawn on the CPU, or what code wrote into the
   buffer directly. Only the rows that hold something are uploaded and drawn.
3. **`BindFrame`**: GPU drawing goes straight into the frame between layers.
   deki-3d does this at the start of each batch of meshes, then clears
   the CPU buffer to transparent for the 2D that sorts after them.
4. **`Present`**: the last layer, then the frame to the window, scaled by
   whole pixels and centred, and the swap.

With the 2D on the GPU, the CPU draws no pixels at all. With it on the CPU, a
frame of 2D, then 3D, then 2D is two uploads and no readback, and with no 3D
one upload, so the presenter is then simply the way the buffer reaches the
screen.

Layers need a buffer that keeps coverage, so a display that presents through
here reports `RGB565A8`. The 2D blitter, drawing into a cleared RGB565A8
buffer, leaves premultiplied colour and the union of coverage, which is
exactly what the presenter blends. Any other format still presents, opaque.

### 2D on the GPU

The presenter is deki-rendering's blit sink (`QuadBlit::BlitSink`): for every
whole frame of a camera that clears, QuadBlit hands it each blit into the
frame buffer, placed and clipped as its CPU code would draw it, and the
presenter draws it as a quad. The quad's shader takes the CPU's integer steps
through the source and runs its pixel pipeline (tint, alpha tint, chroma key,
ordered dither, flips, stretched strips), so it picks the same source pixels
and blends them the same way; the picture differs from the CPU's only where
the CPU rounds each write to 565. Quads are drawn in call order, in batches,
one draw per run of quads from the same texture.

Each source is uploaded as a texture and kept: until its `revision` changes,
or for pixels marked `cacheScaled`, until `QuadBlit::ForgetScaled` says they
go. deki-2d's components, particles and tilemaps set revisions. A source with
neither is uploaded again at every blit, which is always right, only slower.
A texture not drawn for two seconds goes, and past 64 MB the ones drawn
longest ago.

The frame starts from the camera's clear colour and the frame buffer from
nothing, so it collects only what code writes into it directly. That is
composed as a layer before the next quad, so it keeps its place in the draw
order: a render pass that writes into the buffer says so
(`RenderPass::WritesBuffer`), and the renderer tells the presenter after each
of its hooks. Meshes (deki-3d) draw between the quads as they sort.

`SetDraws2D(false)` draws the 2D on the CPU again; `SDL3DisplaySetup`'s
`draw2DOnGpu` turns it off in a project. The log says which way the 2D goes:
`OpenGL: drawing 2D straight into the presented frame`.

The disabled test `GLBlitSink.DISABLED_BenchmarkAgainstTheCpu` times a frame
at 640x480 both ways. On a desktop (RTX 2080, release build), with 20 texts
and a background: 100 sprites, 1.9 ms on the CPU against 0.6 ms; 500, 2.6
against 0.6; 2000, 5.6 against 0.8.

### Screen effects

A `ScreenEffect` component anywhere in the scene draws the finished frame into
the window through a shader: scanlines, a vignette, colour grading. Its
`.shader` defines:

```hlsl
float4 Effect(float2 uv)
{
    float3 colour = Frame(uv).rgb;
    float scan = fmod(floor(uv.y * uResolution.y * 2.0), 2.0) < 1.0 ? 1.0 : 1.0 - uParams.x;
    return float4(colour * scan, 1.0);
}
```

`uv` runs 0 to 1 over the frame, bottom left to top right, at every window
pixel the frame covers, so an effect can work finer than the game's pixels.
`Frame(uv)` reads the frame; `uResolution` is its size in game pixels, `uDst`
where it lands in the window, and `uTime`, `uParams`, `uColor` come from the
component. A shader that does not compile is an error in the editor (below).
Code can also present through an effect of its own
(`GLPresenter::Present` with an `Effect`).

### Limits

- The CPU frame buffer no longer holds the whole picture: with the 2D on the
  GPU it holds only what code wrote into it directly, and when 3D draws into
  the presenter, only the 2D drawn after the last meshes. A pass that reads
  the frame back (a full-screen effect done on the CPU) needs both off
  (`SetDraws2D(false)`, `Mesh3DSettings.composeOnGpu`). Screenshots taken
  from the CPU buffer miss the GPU part too; `GLPresenter::ReadFrame` has the
  whole frame.
- The GPU frame keeps 8 bits a channel, so gradients and blends are a little
  smoother than a device's 565 screen shows them.
- The editor shows its views its own way, so it never presents through here:
  meshes there are drawn off-screen and read back, and screen effects show
  only in a build.

## Shaders

A `.shader` is HLSL. It defines the functions of what draws with it, its
hooks: deki-3d's meshes call `Shade()` and `Displace()`, a `ScreenEffect`
calls `Effect()` (above). What draws with it wraps the file in its own
shader, with the uniforms and types it promises, so a file holds only the
interesting part.

Devices run GLSL, so the editor compiles each file. DXC, Microsoft's HLSL
compiler, makes SPIR-V; SPIRV-Cross writes that as GLSL ES 3.00 (OpenGL ES
3.0, WebGL 2) and GLSL 3.30 (desktop OpenGL). This is done once for each use
whose hooks the file defines (`editor/ShaderUses.h`: "mesh", "effect"), and
`ShaderAsset` is the result: no compiler goes to a device. The Shader
Model 6 language is there, but only what OpenGL ES 3.0 can run compiles; a
wave operation, say, is an error.

Every problem is said, with the file and line, in the Console when the file
changes and in the build's list of problems:

- it does not compile, or uses what OpenGL ES 3.0 cannot run;
- it defines no hook of any use, or only part of what a use needs;
- a scene the build ships uses it, and the target has no OpenGL (a board).
  The build stops: nothing there could draw it.

A package that draws with shaders registers its use from its editor code
(`RegisterShaderUse`), with its hooks and the HLSL around them.

## Dependencies

| Dependency | Type |
|---|---|
| `deki-editor` | Deki editor DLL |

## Community

Questions, ideas and what you made with Deki: [the Deki Discord](https://dekiengine.com/discord/).

## License

Apache 2.0. See [LICENSE](LICENSE).
