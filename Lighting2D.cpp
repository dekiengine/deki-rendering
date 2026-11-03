#include "Lighting2D.h"

#include "DirtyRegion.h"
#include "Light.h"
#include "PixelFormat.h"

#include <deki/LogSystem.h>
#include <deki/Object.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// The pixels of a frame are lit on two threads where there are two cores, on
// a device or a game build: the editor loads this package as a DLL it can
// unload, which a thread kept running would outlive.
#if !defined(DEKI_EDITOR)
#define DEKI_LIGHTING_WORKER 1
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif
#else
#define DEKI_LIGHTING_WORKER 0
#endif

namespace DekiRendering
{
namespace
{
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// A spot's share at offset (dx, dy) from it: 1 inside the cone, fading to 0
// at its edge.
float Cone(const Light2D& l, float dx, float dy, float d2)
{
    if (d2 <= 0.0f)
    {
        return 1.0f;
    }
    const float c = (dx * l.dirX + dy * l.dirY) / std::sqrt(d2);
    if (c <= l.cosOuter)
    {
        return 0.0f;
    }
    if (c >= l.cosInner)
    {
        return 1.0f;
    }
    const float s = (c - l.cosOuter) / (l.cosInner - l.cosOuter);
    return s * s * (3.0f - 2.0f * s);
}

uint8_t ToUnit(float v)
{
    const float scaled = v * static_cast<float>(Lighting2D::kUnit) + 0.5f;
    if (!(scaled > 0.0f))
    {
        return 0;
    }
    return scaled >= static_cast<float>(Lighting2D::kMax) ? static_cast<uint8_t>(Lighting2D::kMax)
                                                          : static_cast<uint8_t>(scaled);
}

// One RGB565 value lit by (lr, lg, lb) in kUnit.
inline uint16_t Light565(uint16_t c, uint32_t lr, uint32_t lg, uint32_t lb)
{
    uint32_t r = ((c >> 11) * lr) >> 5;
    uint32_t g = (((c >> 5) & 0x3F) * lg) >> 5;
    uint32_t b = ((c & 0x1F) * lb) >> 5;
    r = r > 31 ? 31 : r;
    g = g > 63 ? 63 : g;
    b = b > 31 ? 31 : b;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

inline uint8_t Light8(uint8_t v, uint32_t l)
{
    const uint32_t o = (static_cast<uint32_t>(v) * l) >> 5;
    return static_cast<uint8_t>(o > 255 ? 255 : o);
}

// A light as three tables, one per channel of an RGB565 value: each holds the
// channel lit (and kept within it, so a light brighter than the colour clips
// there), already in its place and in the stored byte order. A lit pixel is
// the three ORed, which the byte swap of a stored value distributes over.
struct Tables565
{
    uint32_t key = ~0u;
    uint32_t used = 0;
    uint16_t r[32];
    uint16_t g[64];
    uint16_t b[32];
};

// A few tables kept, by each thread that lights: a frame's light is mostly
// one value (the ambient), and the rest changes from block to block.
struct TableCache
{
    Tables565 tables[8];
    uint32_t clock = 0;
};

bool HaveTables(const TableCache& cache, uint32_t key)
{
    for (const Tables565& t : cache.tables)
    {
        if (t.key == key)
        {
            return true;
        }
    }
    return false;
}

const Tables565& TablesFor(TableCache& cache, uint32_t key, uint32_t lr, uint32_t lg, uint32_t lb)
{
    Tables565* oldest = &cache.tables[0];
    for (Tables565& t : cache.tables)
    {
        if (t.key == key)
        {
            t.used = ++cache.clock;
            return t;
        }
        if (t.used < oldest->used)
        {
            oldest = &t;
        }
    }
    Tables565& t = *oldest;
    t.key = key;
    t.used = ++cache.clock;
    for (uint32_t v = 0; v < 32; v++)
    {
        const uint32_t r = std::min<uint32_t>((v * lr) >> 5, 31);
        const uint32_t b = std::min<uint32_t>((v * lb) >> 5, 31);
        t.r[v] = DekiPixel::ToMemory565(static_cast<uint16_t>(r << 11));
        t.b[v] = DekiPixel::ToMemory565(static_cast<uint16_t>(b));
    }
    for (uint32_t v = 0; v < 64; v++)
    {
        const uint32_t g = std::min<uint32_t>((v * lg) >> 5, 63);
        t.g[v] = DekiPixel::ToMemory565(static_cast<uint16_t>(g << 5));
    }
    return t;
}

// A stored RGB565 pixel lit, stored.
inline uint32_t LightStored565(uint32_t m, const Tables565& t)
{
    const uint32_t v = DekiPixel::FromMemory565(static_cast<uint16_t>(m));
    return static_cast<uint32_t>(t.r[v >> 11] | t.g[(v >> 5) & 0x3Fu] | t.b[v & 0x1Fu]);
}

// `rows` rows of n RGB565 pixels from p, `stride` bytes apart.
[[gnu::noinline]] DEKI_FAST_ATTR void LightRows565(uint8_t* p, int32_t stride, int32_t rows, int32_t n,
                                                   const Tables565& t)
{
    for (int32_t y = 0; y < rows; y++, p += stride)
    {
        uint8_t* q = p;
        int32_t i = 0;
        if ((reinterpret_cast<uintptr_t>(q) & 2) != 0 && n > 0)
        {
            DekiPixel::StoreU16(q, static_cast<uint16_t>(LightStored565(DekiPixel::LoadU16(q), t)));
            q += 2;
            i = 1;
        }
        // Two pixels a load and a store.
        for (; i + 1 < n; i += 2, q += 4)
        {
            const uint32_t w = *reinterpret_cast<const uint32_t*>(q);
            *reinterpret_cast<uint32_t*>(q) = LightStored565(w & 0xFFFFu, t) | (LightStored565(w >> 16, t) << 16);
        }
        if (i < n)
        {
            DekiPixel::StoreU16(q, static_cast<uint16_t>(LightStored565(DekiPixel::LoadU16(q), t)));
        }
    }
}

// The same for RGB565A8, three bytes a pixel; the alpha is left alone.
[[gnu::noinline]] DEKI_FAST_ATTR void LightRows565A8(uint8_t* p, int32_t stride, int32_t rows, int32_t n,
                                                     const Tables565& t)
{
    for (int32_t y = 0; y < rows; y++, p += stride)
    {
        uint8_t* q = p;
        for (int32_t i = 0; i < n; i++, q += 3)
        {
            DekiPixel::StoreU16(q, static_cast<uint16_t>(LightStored565(DekiPixel::LoadU16(q), t)));
        }
    }
}

// By arithmetic, for a few pixels in a light no table is kept for. `step` is
// 2 (RGB565) or 3 (RGB565A8).
void LightRows565Few(uint8_t* p, int32_t stride, int32_t rows, int32_t n, int32_t step, uint32_t lr, uint32_t lg,
                     uint32_t lb)
{
    for (int32_t y = 0; y < rows; y++, p += stride)
    {
        uint8_t* q = p;
        for (int32_t i = 0; i < n; i++, q += step)
        {
            DekiPixel::Store565(q, Light565(DekiPixel::Load565(q), lr, lg, lb));
        }
    }
}

// Lights `rows` rows of n pixels of `format` from p, `stride` bytes apart.
void LightRect(uint8_t* p, int32_t stride, int32_t rows, int32_t n, Deki::ColorFormat format, uint32_t lr,
               uint32_t lg, uint32_t lb, TableCache& cache)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
        case Deki::ColorFormat::RGB565A8:
        {
            // Tables cost 128 entries to make: worth it past a few dozen
            // pixels, or at once when they are kept from before.
            const uint32_t key = (lr << 16) | (lg << 8) | lb;
            if (n * rows >= 48 || HaveTables(cache, key))
            {
                const Tables565& t = TablesFor(cache, key, lr, lg, lb);
                if (format == Deki::ColorFormat::RGB565)
                {
                    LightRows565(p, stride, rows, n, t);
                }
                else
                {
                    LightRows565A8(p, stride, rows, n, t);
                }
            }
            else
            {
                LightRows565Few(p, stride, rows, n, format == Deki::ColorFormat::RGB565 ? 2 : 3, lr, lg, lb);
            }
            break;
        }
        case Deki::ColorFormat::RGB888:
            for (int32_t y = 0; y < rows; y++, p += stride)
            {
                uint8_t* q = p;
                for (int32_t i = 0; i < n; i++, q += 3)
                {
                    q[0] = Light8(q[0], lr);
                    q[1] = Light8(q[1], lg);
                    q[2] = Light8(q[2], lb);
                }
            }
            break;
        case Deki::ColorFormat::ARGB8888:
            for (int32_t y = 0; y < rows; y++, p += stride)
            {
                uint8_t* q = p;
                for (int32_t i = 0; i < n; i++, q += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, q, 4);
                    const uint32_t r = Light8(static_cast<uint8_t>(v >> 16), lr);
                    const uint32_t g = Light8(static_cast<uint8_t>(v >> 8), lg);
                    const uint32_t b = Light8(static_cast<uint8_t>(v), lb);
                    v = (v & 0xFF000000u) | (r << 16) | (g << 8) | b;
                    std::memcpy(q, &v, 4);
                }
            }
            break;
    }
}

int32_t FloorDiv(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
}  // namespace

bool Light2D::operator==(const Light2D& o) const
{
    return x == o.x && y == o.y && radius == o.radius && r == o.r && g == o.g && b == o.b && spot == o.spot &&
           dirX == o.dirX && dirY == o.dirY && cosOuter == o.cosOuter && cosInner == o.cosInner && bands == o.bands;
}

Deki::Rect Light2D::Bounds() const
{
    // The blocks whose centres it can reach: centre within radius.
    const int32_t k = Lighting2D::kBlock;
    const int32_t bx0 = static_cast<int32_t>(std::floor((x - radius - k * 0.5f) / k));
    const int32_t bx1 = static_cast<int32_t>(std::ceil((x + radius - k * 0.5f) / k));
    const int32_t by0 = static_cast<int32_t>(std::floor((y - radius - k * 0.5f) / k));
    const int32_t by1 = static_cast<int32_t>(std::ceil((y + radius - k * 0.5f) / k));
    return Deki::Rect{ bx0 * k, by0 * k, (bx1 + 1) * k, (by1 + 1) * k };
}

void Lighting2D::Build(const Deki::Scene* scene, const FrameCamera& cam, int32_t viewWidth, int32_t viewHeight,
                       int32_t litUpToSortOrder)
{
    m_ViewWidth = viewWidth;
    m_ViewHeight = viewHeight;
    m_LitUpTo = litUpToSortOrder;
    m_AmbientR = m_AmbientG = m_AmbientB = 0.0f;
    m_Lights.clear();

    static std::vector<const Light*> s_Found;
    Light::InScene(scene, s_Found);
    m_On = !s_Found.empty();
    if (!m_On)
    {
        return;
    }

    for (const Light* light : s_Found)
    {
        if (!light->Shines())
        {
            continue;
        }
        const float r = light->color.r / 255.0f * light->intensity;
        const float g = light->color.g / 255.0f * light->intensity;
        const float b = light->color.b / 255.0f * light->intensity;
        if (light->type == LightType::Ambient || light->type == LightType::Directional)
        {
            // 2D has no surface to face a light, so a far light lights it
            // evenly, as ambient does.
            m_AmbientR += r;
            m_AmbientG += g;
            m_AmbientB += b;
            continue;
        }
        const Deki::Object* owner = light->GetOwner();
        Light2D l;
        cam.WorldToScreen(owner->GetWorldX(), owner->GetWorldY(), l.x, l.y);
        l.radius = light->range * cam.ppm;
        l.r = r;
        l.g = g;
        l.b = b;
        l.bands = light->bands < 0 ? 0 : light->bands;
        if (light->type == LightType::Spot)
        {
            l.spot = true;
            // World angles turn counter-clockwise with Y up; the screen's Y is down.
            const float a = owner->GetWorldRotation();
            l.dirX = std::cos(a);
            l.dirY = -std::sin(a);
            const float half = std::min(std::max(light->spotAngle, 1.0f), 179.0f) * 0.5f * kDegToRad;
            const float soft = std::min(std::max(light->spotSoftness, 0.0f), 1.0f);
            l.cosOuter = std::cos(half);
            l.cosInner = std::cos(half * (1.0f - soft));
            if (l.cosInner <= l.cosOuter)
            {
                l.cosInner = l.cosOuter + 1e-4f;
            }
        }
        if (l.radius > 0.0f)
        {
            m_Lights.push_back(l);
        }
    }
}

void Lighting2D::Ambient(float& r, float& g, float& b) const
{
    r = m_AmbientR;
    g = m_AmbientG;
    b = m_AmbientB;
}

void Lighting2D::KeepOrForgetGrid()
{
    const int32_t nbx = (m_ViewWidth + kBlock - 1) / kBlock;
    const int32_t nby = (m_ViewHeight + kBlock - 1) / kBlock;
    const bool same = nbx == m_GridWidth && nby == m_GridHeight && m_AmbientR == m_GridAmbientR &&
                      m_AmbientG == m_GridAmbientG && m_AmbientB == m_GridAmbientB && m_Lights == m_GridLights;
    if (same)
    {
        return;
    }
    m_GridWidth = nbx;
    m_GridHeight = nby;
    m_GridAmbientR = m_AmbientR;
    m_GridAmbientG = m_AmbientG;
    m_GridAmbientB = m_AmbientB;
    m_GridLights = m_Lights;
    const size_t cells = static_cast<size_t>(nbx) * static_cast<size_t>(nby);
    m_GridR.resize(cells);
    m_GridG.resize(cells);
    m_GridB.resize(cells);
    m_GridHard.resize(cells);
    m_GridEven.resize(cells);
    m_GridRowDone.assign(static_cast<size_t>(nby), 0);
    m_GridEvenDone.assign(static_cast<size_t>(nby), 0);
}

void Lighting2D::LightBlockRow(int32_t by)
{
    const int32_t nbx = m_GridWidth;
    const size_t at = static_cast<size_t>(by) * static_cast<size_t>(nbx);
    uint8_t* cr = &m_GridR[at];
    uint8_t* cg = &m_GridG[at];
    uint8_t* cb = &m_GridB[at];
    // The ambient, everywhere no light reaches.
    std::memset(cr, ToUnit(m_AmbientR), static_cast<size_t>(nbx));
    std::memset(cg, ToUnit(m_AmbientG), static_cast<size_t>(nbx));
    std::memset(cb, ToUnit(m_AmbientB), static_cast<size_t>(nbx));
    uint8_t* hard = &m_GridHard[at];
    std::memset(hard, 0, static_cast<size_t>(nbx));

    m_RowR.resize(static_cast<size_t>(nbx));
    m_RowG.resize(static_cast<size_t>(nbx));
    m_RowB.resize(static_cast<size_t>(nbx));
    m_RowTouched.resize(static_cast<size_t>(nbx));
    m_RowHard.resize(static_cast<size_t>(nbx));
    if (++m_RowGeneration == 0)
    {
        std::fill(m_RowTouched.begin(), m_RowTouched.end(), 0u);
        std::fill(m_RowHard.begin(), m_RowHard.end(), 0u);
        m_RowGeneration = 1;
    }
    const uint32_t gen = m_RowGeneration;
    int32_t lo = nbx, hi = 0;

    const float cy = static_cast<float>(by * kBlock) + kBlock * 0.5f;
    for (const Light2D& l : m_Lights)
    {
        const float dy = cy - l.y;
        const float r2 = l.radius * l.radius;
        const float reach2 = r2 - dy * dy;
        if (reach2 <= 0.0f)
        {
            continue;
        }
        const float reach = std::sqrt(reach2);
        int32_t from = static_cast<int32_t>(std::floor((l.x - reach - kBlock * 0.5f) / kBlock));
        int32_t to = static_cast<int32_t>(std::ceil((l.x + reach - kBlock * 0.5f) / kBlock)) + 1;
        from = std::max<int32_t>(from, 0);
        to = std::min<int32_t>(to, nbx);
        if (from >= to)
        {
            continue;
        }
        lo = std::min(lo, from);
        hi = std::max(hi, to);
        const float invR2 = 1.0f / r2;
        const float bands = static_cast<float>(l.bands);
        const float invBands = l.bands > 0 ? 1.0f / bands : 0.0f;
        const float dy2 = dy * dy;
        float dx = static_cast<float>(from * kBlock) + kBlock * 0.5f - l.x;
        for (int32_t bx = from; bx < to; bx++, dx += kBlock)
        {
            const float d2 = dx * dx + dy2;
            const float t = 1.0f - d2 * invR2;
            if (t <= 0.0f)
            {
                continue;
            }
            float f = l.bands > 0 ? std::ceil(t * bands) * invBands : t * t;
            if (l.spot)
            {
                f *= Cone(l, dx, dy, d2);
                if (f <= 0.0f)
                {
                    continue;
                }
            }
            const size_t i = static_cast<size_t>(bx);
            if (m_RowTouched[i] != gen)
            {
                m_RowTouched[i] = gen;
                m_RowR[i] = m_AmbientR;
                m_RowG[i] = m_AmbientG;
                m_RowB[i] = m_AmbientB;
            }
            m_RowR[i] += l.r * f;
            m_RowG[i] += l.g * f;
            m_RowB[i] += l.b * f;
            if (l.bands > 0)
            {
                m_RowHard[i] = gen;  // its rings stay hard
            }
        }
    }
    for (int32_t bx = lo; bx < hi; bx++)
    {
        const size_t i = static_cast<size_t>(bx);
        if (m_RowTouched[i] == gen)
        {
            cr[i] = ToUnit(m_RowR[i]);
            cg[i] = ToUnit(m_RowG[i]);
            cb[i] = ToUnit(m_RowB[i]);
            hard[i] = m_RowHard[i] == gen ? 1 : 0;
        }
    }
}

void Lighting2D::EvenRow(int32_t by)
{
    // A block is lit the same all over when it and the eight round it have
    // one light (the grid's edge repeating), or when it is in or beside a
    // light with hard rings, whose edges stay hard. Elsewhere its pixels are
    // blended between the block centres (LightBlockSoft).
    const int32_t w = m_GridWidth;
    for (int32_t bx = 0; bx < w; bx++)
    {
        const size_t at = static_cast<size_t>(by) * w + bx;
        const uint8_t r = m_GridR[at], g = m_GridG[at], b = m_GridB[at];
        bool alike = true, hard = false;
        for (int32_t dy = -1; dy <= 1 && !hard; dy++)
        {
            const int32_t y = std::min<int32_t>(std::max<int32_t>(by + dy, 0), m_GridHeight - 1);
            for (int32_t dx = -1; dx <= 1; dx++)
            {
                const int32_t x = std::min<int32_t>(std::max<int32_t>(bx + dx, 0), w - 1);
                const size_t i = static_cast<size_t>(y) * w + x;
                hard = hard || m_GridHard[i] != 0;
                alike = alike && m_GridR[i] == r && m_GridG[i] == g && m_GridB[i] == b;
            }
        }
        m_GridEven[at] = (alike || hard) ? 1 : 0;
    }
}

void Lighting2D::BlockLight(int32_t bx, int32_t by, uint32_t& r, uint32_t& g, uint32_t& b)
{
    KeepOrForgetGrid();
    if (bx < 0 || by < 0 || bx >= m_GridWidth || by >= m_GridHeight)
    {
        r = g = b = 0;
        return;
    }
    if (!m_GridRowDone[static_cast<size_t>(by)])
    {
        LightBlockRow(by);
        m_GridRowDone[static_cast<size_t>(by)] = 1;
    }
    const size_t i = static_cast<size_t>(by) * static_cast<size_t>(m_GridWidth) + static_cast<size_t>(bx);
    r = m_GridR[i];
    g = m_GridG[i];
    b = m_GridB[i];
}

namespace
{
// What one thread lights: every `step`-th block row of the area from `first`.
struct ApplyJob
{
    const Lighting2D::Grid* grid = nullptr;
    uint8_t* buffer = nullptr;
    int32_t width = 0;
    int32_t bpp = 0;
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    Deki::Rect area = {};
    int32_t viewLeft = 0;
    int32_t viewTop = 0;
    int32_t bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
};

// One pixel of `format` at p lit by (lr, lg, lb) in kUnit.
inline void LightPixel(uint8_t* p, Deki::ColorFormat format, uint32_t lr, uint32_t lg, uint32_t lb)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
        case Deki::ColorFormat::RGB565A8:
            DekiPixel::Store565(p, Light565(DekiPixel::Load565(p), lr, lg, lb));
            break;
        case Deki::ColorFormat::RGB888:
            p[0] = Light8(p[0], lr);
            p[1] = Light8(p[1], lg);
            p[2] = Light8(p[2], lb);
            break;
        case Deki::ColorFormat::ARGB8888:
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            const uint32_t r = Light8(static_cast<uint8_t>(v >> 16), lr);
            const uint32_t g = Light8(static_cast<uint8_t>(v >> 8), lg);
            const uint32_t b = Light8(static_cast<uint8_t>(v), lb);
            v = (v & 0xFF000000u) | (r << 16) | (g << 8) | b;
            std::memcpy(p, &v, 4);
            break;
        }
    }
}

// Lights the pixels [x0, x1) x [y0, y1) of block (bx, by), each by the light
// of the four block centres round it, blended by how near each is, so a light
// fades out pixel by pixel rather than in steps of a block. Every block's
// pixels sit at the same places between the centres: a pixel 0 to 3 into the
// block is 5, 7, 1 or 3 eighths past the centre before it (the block's own for
// the last two, the one before for the first two), across and down alike.
// The blend keeps six bits below the 1/32 the pixels are lit in, laid down in
// an ordered dither.
[[gnu::noinline]] DEKI_FAST_ATTR void LightBlockSoft(const ApplyJob& job, int32_t bx, int32_t by, int32_t x0,
                                                     int32_t x1, int32_t y0, int32_t y1)
{
    static constexpr uint32_t kToNext[4] = { 5, 7, 1, 3 };
    static constexpr int32_t kFrom[4] = { 0, 0, 1, 1 };  // the centre before, in the 3x3 round the block
    const Lighting2D::Grid& g = *job.grid;
    // The light of the 3x3 blocks round this one, the grid's edge repeating,
    // in kUnit (at most 64): red and blue packed in one word, 16 bits each,
    // room for the 64 x 8 x 8 of the blend; green in another.
    uint32_t rb[3][3], gg[3][3];
    for (int32_t dy = 0; dy < 3; dy++)
    {
        const int32_t y = std::min<int32_t>(std::max<int32_t>(by + dy - 1, 0), g.height - 1);
        for (int32_t dx = 0; dx < 3; dx++)
        {
            const int32_t x = std::min<int32_t>(std::max<int32_t>(bx + dx - 1, 0), g.width - 1);
            const size_t i = static_cast<size_t>(y) * g.width + x;
            rb[dy][dx] = (static_cast<uint32_t>(g.r[i]) << 16) | g.b[i];
            gg[dy][dx] = g.g[i];
        }
    }
    const int32_t k = Lighting2D::kBlock;
    const int32_t blockX = bx * k - job.viewLeft;  // the block's first pixel in the buffer
    const int32_t blockY = by * k - job.viewTop;
    const bool is565 = job.format == Deki::ColorFormat::RGB565 || job.format == Deki::ColorFormat::RGB565A8;
    for (int32_t y = y0; y < y1; y++)
    {
        const int32_t oy = y - blockY;
        const uint32_t wy = kToNext[oy];
        const int32_t ty = kFrom[oy];
        const int32_t vy = y + job.viewTop;
        const uint8_t* bayerRow = &DekiPixel::kBayer8x8[(vy & 7) << 3];
        // Down first: the column of light at each centre across.
        const uint32_t rb0 = rb[ty][0] * (8 - wy) + rb[ty + 1][0] * wy;
        const uint32_t rb1 = rb[ty][1] * (8 - wy) + rb[ty + 1][1] * wy;
        const uint32_t rb2 = rb[ty][2] * (8 - wy) + rb[ty + 1][2] * wy;
        const uint32_t g0 = gg[ty][0] * (8 - wy) + gg[ty + 1][0] * wy;
        const uint32_t g1 = gg[ty][1] * (8 - wy) + gg[ty + 1][1] * wy;
        const uint32_t g2 = gg[ty][2] * (8 - wy) + gg[ty + 1][2] * wy;
        // Then across, for the block's four pixels.
        const uint32_t acrossRB[4] = { rb0 * 3 + rb1 * 5, rb0 + rb1 * 7, rb1 * 7 + rb2, rb1 * 5 + rb2 * 3 };
        const uint32_t acrossG[4] = { g0 * 3 + g1 * 5, g0 + g1 * 7, g1 * 7 + g2, g1 * 5 + g2 * 3 };
        uint8_t* p = job.buffer + (static_cast<size_t>(y) * job.width + x0) * job.bpp;
        for (int32_t x = x0; x < x1; x++, p += job.bpp)
        {
            const uint32_t d = bayerRow[(x + job.viewLeft) & 7] >> 2;  // 0 to 63
            const uint32_t l = acrossRB[x - blockX];
            const uint32_t lr = ((l >> 16) + d) >> 6;
            const uint32_t lg = (acrossG[x - blockX] + d) >> 6;
            const uint32_t lb = ((l & 0xFFFFu) + d) >> 6;
            if (is565)
            {
                DekiPixel::Store565(p, Light565(DekiPixel::Load565(p), lr, lg, lb));
            }
            else
            {
                LightPixel(p, job.format, lr, lg, lb);
            }
        }
    }
}

void ApplyRows(const ApplyJob& job, int32_t first, int32_t step, TableCache& tables)
{
    const Lighting2D::Grid& grid = *job.grid;
    const int32_t k = Lighting2D::kBlock;
    const Deki::Rect& a = job.area;
    for (int32_t by = job.by0 + first; by < job.by1; by += step)
    {
        const int32_t y0 = std::max<int32_t>(by * k - job.viewTop, a.top);
        const int32_t y1 = std::min<int32_t>((by + 1) * k - job.viewTop, a.bottom);
        if (y1 <= y0)
        {
            continue;
        }
        const size_t row = static_cast<size_t>(by) * static_cast<size_t>(grid.width);
        const uint8_t* cr = grid.r + row;
        const uint8_t* cg = grid.g + row;
        const uint8_t* cb = grid.b + row;
        const uint8_t* even = grid.even + row;
        // Runs of evenly lit blocks with the same light, each lit (or
        // skipped, at 1x) in one go; a block where the light changes, pixel
        // by pixel.
        int32_t i = job.bx0;
        while (i < job.bx1)
        {
            const int32_t bx0 = std::max<int32_t>(i * k - job.viewLeft, a.left);
            if (!even[i])
            {
                const int32_t bx1 = std::min<int32_t>((i + 1) * k - job.viewLeft, a.right);
                if (bx1 > bx0)
                {
                    LightBlockSoft(job, i, by, bx0, bx1, y0, y1);
                }
                i++;
                continue;
            }
            const uint8_t lr = cr[i], lg = cg[i], lb = cb[i];
            int32_t j = i + 1;
            while (j < job.bx1 && cr[j] == lr && cg[j] == lg && cb[j] == lb && even[j])
            {
                j++;
            }
            if (lr != Lighting2D::kUnit || lg != Lighting2D::kUnit || lb != Lighting2D::kUnit)
            {
                const int32_t x1 = std::min<int32_t>(j * k - job.viewLeft, a.right);
                if (x1 > bx0)
                {
                    uint8_t* corner = job.buffer + (static_cast<size_t>(y0) * job.width + bx0) * job.bpp;
                    LightRect(corner, job.width * job.bpp, y1 - y0, x1 - bx0, job.format, lr, lg, lb, tables);
                }
            }
            i = j;
        }
    }
}

#if DEKI_LIGHTING_WORKER
// The other core's half of the pixels: one worker, started with the first
// frame lit, kept. Started per frame it would cost more than it saves.
#if defined(ESP_PLATFORM)
// A FreeRTOS task pinned to the core the engine is not on.
class ApplyWorker
{
public:
    ApplyWorker()
    {
        m_Start = xSemaphoreCreateBinary();
        m_Done = xSemaphoreCreateBinary();
        const BaseType_t core = xPortGetCoreID() == 0 ? 1 : 0;
        if (!m_Start || !m_Done ||
            xTaskCreatePinnedToCore(&Main, "deki_light", 3072, this, uxTaskPriorityGet(nullptr), nullptr, core) !=
                pdPASS)
        {
            DEKI_LOG_ERROR("Lights: no task for the second core; the frame is lit on one");
            m_Running = false;
            return;
        }
        DEKI_LOG_INFO("[Rendering] lights: each frame lit on two cores");
        m_Running = true;
    }

    bool Running() const { return m_Running; }

    // Lights every other block row of `job` from the second, while the
    // caller lights the rest; returns once both halves are done.
    void Run(const ApplyJob& job, TableCache& mine)
    {
        m_Job = job;
        xSemaphoreGive(m_Start);
        ApplyRows(job, 0, 2, mine);
        xSemaphoreTake(m_Done, portMAX_DELAY);
    }

private:
    static void Main(void* self)
    {
        auto* w = static_cast<ApplyWorker*>(self);
        while (true)
        {
            xSemaphoreTake(w->m_Start, portMAX_DELAY);
            ApplyRows(w->m_Job, 1, 2, w->m_Tables);
            xSemaphoreGive(w->m_Done);
        }
    }

    SemaphoreHandle_t m_Start = nullptr;
    SemaphoreHandle_t m_Done = nullptr;
    ApplyJob m_Job;
    TableCache m_Tables;
    bool m_Running = false;
};

ApplyWorker* Worker()
{
    if (configNUMBER_OF_CORES < 2)
    {
        return nullptr;
    }
    static ApplyWorker s_Worker;
    return s_Worker.Running() ? &s_Worker : nullptr;
}
#else
class ApplyWorker
{
public:
    ApplyWorker()
        : m_Thread([this] { Loop(); })
    {
    }
    ~ApplyWorker()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stop = true;
        }
        m_Ready.notify_one();
        m_Thread.join();
    }

    // Lights every other block row of `job` from the second, while the
    // caller lights the rest; returns once both halves are done.
    void Run(const ApplyJob& job, TableCache& mine)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Job = job;
            m_HasWork = true;
        }
        m_Ready.notify_one();
        ApplyRows(job, 0, 2, mine);
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Done.wait(lock, [this] { return !m_HasWork; });
    }

private:
    void Loop()
    {
        while (true)
        {
            ApplyJob job;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Ready.wait(lock, [this] { return m_Stop || m_HasWork; });
                if (m_Stop)
                {
                    return;
                }
                job = m_Job;
            }
            ApplyRows(job, 1, 2, m_Tables);
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_HasWork = false;
            }
            m_Done.notify_one();
        }
    }

    std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::condition_variable m_Done;
    ApplyJob m_Job;
    bool m_HasWork = false;
    bool m_Stop = false;
    TableCache m_Tables;
    std::thread m_Thread;  // last: it starts running in the constructor
};

ApplyWorker* Worker()
{
    if (std::thread::hardware_concurrency() < 2)
    {
        return nullptr;
    }
    static ApplyWorker s_Worker;
    return &s_Worker;
}
#endif
#endif

TableCache s_MainTables;
}  // namespace

void Lighting2D::Apply(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                       const Deki::Rect& area, int32_t viewLeft, int32_t viewTop)
{
    if (!m_On || !buffer || width <= 0 || height <= 0)
    {
        return;
    }
    Deki::Rect a = area.Empty() ? Deki::Rect{ 0, 0, width, height } : area;
    a.left = std::max<int32_t>(a.left, 0);
    a.top = std::max<int32_t>(a.top, 0);
    a.right = std::min<int32_t>(a.right, width);
    a.bottom = std::min<int32_t>(a.bottom, height);
    // Nothing outside the view is lit: the grid ends there.
    a.right = std::min<int32_t>(a.right, m_ViewWidth - viewLeft);
    a.bottom = std::min<int32_t>(a.bottom, m_ViewHeight - viewTop);
    if (a.Empty())
    {
        return;
    }

    KeepOrForgetGrid();
    ApplyJob job;
    job.buffer = buffer;
    job.width = width;
    job.bpp = static_cast<int32_t>(Deki::FrameBufferBytes(format, 1, 1));
    job.format = format;
    job.area = a;
    job.viewLeft = viewLeft;
    job.viewTop = viewTop;
    job.bx0 = FloorDiv(a.left + viewLeft, kBlock);
    job.bx1 = FloorDiv(a.right - 1 + viewLeft, kBlock) + 1;
    job.by0 = FloorDiv(a.top + viewTop, kBlock);
    job.by1 = FloorDiv(a.bottom - 1 + viewTop, kBlock) + 1;

    // The light of the blocks first, for the rows not worked out already in
    // this frame or an earlier one with the same lights: the area's, and one
    // more above and below, which its edge blends with.
    const int32_t from = std::max<int32_t>(job.by0 - 1, 0);
    const int32_t to = std::min<int32_t>(job.by1 + 1, m_GridHeight);
    for (int32_t by = from; by < to; by++)
    {
        if (!m_GridRowDone[static_cast<size_t>(by)])
        {
            LightBlockRow(by);
            m_GridRowDone[static_cast<size_t>(by)] = 1;
        }
    }
    for (int32_t by = job.by0; by < job.by1; by++)
    {
        if (!m_GridEvenDone[static_cast<size_t>(by)])
        {
            EvenRow(by);
            m_GridEvenDone[static_cast<size_t>(by)] = 1;
        }
    }
    const Grid grid{ m_GridR.data(), m_GridG.data(), m_GridB.data(), m_GridEven.data(), m_GridWidth, m_GridHeight };
    job.grid = &grid;

#if DEKI_LIGHTING_WORKER
    if (ApplyWorker* worker = job.by1 - job.by0 > 1 ? Worker() : nullptr)
    {
        worker->Run(job, s_MainTables);
        return;
    }
#endif
    ApplyRows(job, 0, 1, s_MainTables);
}

void Lighting2D::AddChanges(Lighting2D& last, DirtyRegion& changed) const
{
    const bool same = m_On == last.m_On && m_LitUpTo == last.m_LitUpTo && m_ViewWidth == last.m_ViewWidth &&
                      m_ViewHeight == last.m_ViewHeight && m_AmbientR == last.m_AmbientR &&
                      m_AmbientG == last.m_AmbientG && m_AmbientB == last.m_AmbientB &&
                      m_Lights.size() == last.m_Lights.size();
    if (!same)
    {
        // The whole view changes (the ambient light, or lighting on or off),
        // or lights came or went: everything is lit again.
        if (m_On || last.m_On)
        {
            changed.SetFull();
        }
    }
    else
    {
        for (size_t i = 0; i < m_Lights.size(); i++)
        {
            if (m_Lights[i] != last.m_Lights[i])
            {
                changed.Add(last.m_Lights[i].Bounds());
                changed.Add(m_Lights[i].Bounds());
            }
        }
    }
    last.m_On = m_On;
    last.m_LitUpTo = m_LitUpTo;
    last.m_ViewWidth = m_ViewWidth;
    last.m_ViewHeight = m_ViewHeight;
    last.m_AmbientR = m_AmbientR;
    last.m_AmbientG = m_AmbientG;
    last.m_AmbientB = m_AmbientB;
    last.m_Lights = m_Lights;
}

}  // namespace DekiRendering
