# Changelog

Notable changes to `deki-rendering`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

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
