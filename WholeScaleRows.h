#pragma once

#include <cstdint>

#include "QuadBlit.h"

namespace QuadBlit
{

/// A plain blit (no tint, key, dither or flips) onto an ARGB8888 target, at a
/// whole-number scale on each axis (1x, 2x, 3x...): each source pixel is read
/// once and fills the block of target pixels it covers, where the generic
/// pipeline samples every target pixel on its own. The pixels are the
/// pipeline's: the same source pixel lands on each target pixel, and a
/// translucent one blends with the same arithmetic.
///
/// Draws inside [startX, endX) x [startY, endY), the blit's rectangle already
/// clipped. False, drawing nothing, when the blit is not at a whole-number
/// scale.
bool WholeScaleRows_ARGB8888(const Source& source, uint32_t* target, int32_t targetWidth, int32_t destX, int32_t destY,
                             int32_t destWidth, int32_t destHeight, int32_t startX, int32_t startY, int32_t endX,
                             int32_t endY);

/// The scale WholeScaleRows_ARGB8888 draws `size` source pixels at to cover
/// `destSize`: destSize / size when that is whole, else 0.
inline int32_t WholeScale(int32_t size, int32_t destSize)
{
    return size > 0 && destSize >= size && destSize % size == 0 ? destSize / size : 0;
}

}  // namespace QuadBlit
