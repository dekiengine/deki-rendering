# Deki Rendering

Core rendering pipeline for the Deki Engine: camera, render system, standard 2D renderer, render passes, and sorting callbacks.

Part of the [Deki Engine](https://github.com/dekiengine/deki-engine) package ecosystem.

## Installation

Install via the Package Manager inside the Deki Editor.

## Partial present (dirty rectangles)

Off by default. With the project's Rendering setting `dirtyTileTracking` on,
the render system records the rectangles each frame draws (QuadBlit reports
every clipped blit; a pass that writes the framebuffer directly calls
`QuadBlit::MarkDirty`), clears only the previous frame's rectangles instead of
the whole framebuffer, and hands the display the rectangles that changed on
screen (this frame's and the previous frame's, aligned to `dirtyTileSize`)
through `IDekiDisplay::PresentRegions`. Displays that cannot present partial
frames keep receiving whole frames. Double-buffered displays are covered: the
history is kept per buffer pointer. A first frame, a resize, a clear-colour
change, a swapped display or `IDekiRenderSystem::MarkAllDirty()` all fall back
to a full clear and a full present.

Editor and SDL3 displays present partial frames. The LovyanGFX display's
partial present (row bands through a small DMA staging buffer, which also
stops the byte swap for big-endian panels from mutating the engine's
framebuffer) is implemented but has not yet been run on hardware.

## Dependencies

| Dependency | Type |
|---|---|
| `deki-editor` | Deki editor DLL |

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
