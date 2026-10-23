// KernelOp::RGB565A8BlendRow: a platform's blend kernel must write the scalar
// blend's pixels, and QuadBlit must hand it the right runs. A kernel written
// to the contract in plain C++ is registered here, and every blit that may
// use it must come out the same with it as without.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

namespace
{

using DekiPixel::Div255;

int s_KernelCalls = 0;

// The contract, pixel by pixel.
void ContractBlendRow(const uint8_t* src, uint8_t* dst8, int32_t n, uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    ++s_KernelCalls;
    for (int32_t i = 0; i < n; i++, src += 3, dst8 += 2)
    {
        uint32_t a = src[2];
        if (ta != 255)
        {
            a = Div255(a * ta);
        }
        if (a == 0)
        {
            continue;
        }
        const uint16_t v = DekiPixel::Load565(src);
        const uint32_t r = Div255(((v >> 11) << 3) * tr);
        const uint32_t g = Div255((((v >> 5) & 63) << 2) * tg);
        const uint32_t b = Div255(((v & 31) << 3) * tb);
        if (a == 255)
        {
            DekiPixel::Store565(dst8, DekiPixel::PackRGB565(r, g, b));
            continue;
        }
        const uint16_t bg = DekiPixel::Load565(dst8);
        const uint32_t inv = 255 - a;
        DekiPixel::Store565(dst8, DekiPixel::PackRGB565(Div255(r * a + ((bg >> 11) << 3) * inv),
                                                        Div255(g * a + (((bg >> 5) & 63) << 2) * inv),
                                                        Div255(b * a + ((bg & 31) << 3) * inv)));
    }
}

constexpr int kSrcW = 21, kSrcH = 9;
constexpr int kDstW = 40, kDstH = 24;

// Alpha with clear, opaque and partial runs, so the span paths have work.
std::vector<uint8_t> MakePixels()
{
    std::vector<uint8_t> pixels(static_cast<size_t>(kSrcW) * kSrcH * 3);
    for (int y = 0; y < kSrcH; ++y)
    {
        for (int x = 0; x < kSrcW; ++x)
        {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * kSrcW + x) * 3];
            DekiPixel::Store565(p, static_cast<uint16_t>(0x2C51u * (x + 1) + 0x1717u * (y + 2)));
            const int edge = (x < 3 || x >= kSrcW - 3) ? 0 : (x < 6 || x >= kSrcW - 6) ? (x * 37 + y * 11) % 256 : 255;
            p[2] = static_cast<uint8_t>(y == 4 ? (x * 53) % 256 : edge);
        }
    }
    return pixels;
}

// Each row's opaque run and longest clear run, as the bakes give them.
void Spans(const std::vector<uint8_t>& px, std::vector<int16_t>& opaque, std::vector<int16_t>& clear)
{
    opaque.assign(kSrcH * 2, 0);
    clear.assign(kSrcH * 2, 0);
    for (int y = 0; y < kSrcH; ++y)
    {
        const uint8_t* row = &px[static_cast<size_t>(y) * kSrcW * 3];
        int s = 0;
        while (s < kSrcW && row[s * 3 + 2] != 255) s++;
        int e = s;
        while (e < kSrcW && row[e * 3 + 2] == 255) e++;
        if (s >= kSrcW) s = e = 0;
        opaque[y * 2] = static_cast<int16_t>(s);
        opaque[y * 2 + 1] = static_cast<int16_t>(e);
        int bs = 0, be = 0;
        for (int x = 0; x < kSrcW;)
        {
            if (row[x * 3 + 2] != 0) { x++; continue; }
            const int rs = x;
            while (x < kSrcW && row[x * 3 + 2] == 0) x++;
            if (x - rs > be - bs) { bs = rs; be = x; }
        }
        clear[y * 2] = static_cast<int16_t>(bs);
        clear[y * 2 + 1] = static_cast<int16_t>(be);
    }
}

std::vector<uint8_t> Draw(const QuadBlit::Source& s, int x, int y, uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, x, y, kSrcW, kSrcH, tr, tg, tb,
                         ta);
    return target;
}

}  // namespace

TEST(BlendKernel, GivesTheScalarPixelsOnEveryPathThatUsesIt)
{
    const std::vector<uint8_t> pixels = MakePixels();
    std::vector<int16_t> opaque, clear;
    Spans(pixels, opaque, clear);

    struct Tint
    {
        uint8_t r, g, b, a;
    };
    const Tint tints[] = { { 255, 255, 255, 255 }, { 255, 255, 255, 42 }, { 52, 211, 153, 255 }, { 52, 211, 153, 42 } };
    const int places[][2] = { { 5, 4 }, { -7, 2 }, { 30, 20 }, { 6, -3 } };
    for (int spans = 0; spans < 3; ++spans)
    {
        QuadBlit::Source s = QuadBlit::MakeSource(pixels.data(), kSrcW, kSrcH, QuadBlit::PixelLayout::RGB565A8(),
                                                  false, spans >= 1 ? opaque.data() : nullptr);
        s.clearRowSpans = spans == 2 ? clear.data() : nullptr;
        for (const auto& at : places)
        {
            for (const Tint& t : tints)
            {
                QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, nullptr);
                const std::vector<uint8_t> want = Draw(s, at[0], at[1], t.r, t.g, t.b, t.a);
                QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, &ContractBlendRow);
                s_KernelCalls = 0;
                const std::vector<uint8_t> got = Draw(s, at[0], at[1], t.r, t.g, t.b, t.a);
                QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, nullptr);
                EXPECT_GT(s_KernelCalls, 0) << "the kernel was not used";
                EXPECT_EQ(got, want) << "spans " << spans << " at " << at[0] << "," << at[1] << " tint " << int(t.r)
                                     << "," << int(t.g) << "," << int(t.b) << "," << int(t.a);
            }
        }
    }
}
