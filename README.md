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

Scenes saved before 0.16.0 used bare names and still load; saving writes the current one.

## Install

Package Manager in the Deki Editor, or `DekiEditor --packages-add deki-rendering <project>`.

## Partial present (dirty rectangles)

Off by default. Turn on the project's Rendering setting `dirtyTileTracking`
and the render system tracks what each frame actually touched:

- It records every rectangle drawn. QuadBlit reports each clipped blit; a pass
  writing the framebuffer directly calls `QuadBlit::MarkDirty`.
- It clears only last frame's rectangles instead of the whole framebuffer.
- It sends the display this frame's and last frame's rectangles, aligned to
  `dirtyTileSize`, through `Deki::IDisplay::PresentRegions`.

Displays that cannot do partial frames keep getting whole ones. Double
buffering works: history is kept per buffer pointer. A first frame, a resize,
a clear-colour change, a swapped display or
`DekiRendering::DekiRenderSystem::MarkAllDirty()` fall back to a full clear and
a full present.

Editor and SDL3 displays present partial frames. The LovyanGFX display's
partial present (row bands through a small DMA staging buffer, which also
stops the byte swap for big-endian panels from mutating the engine's
framebuffer) is implemented but has not yet been run on hardware.

## Dependencies

| Dependency | Type |
|---|---|
| `deki-editor` | Deki editor DLL |

## License

Apache 2.0. See [LICENSE](LICENSE).
