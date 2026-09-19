# Changelog

Notable changes to `deki-rendering`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

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
