// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Objective comparisons for tightly packed RGBA8 frames. No Qt or GPU types: the same code
// scores DLSS FG output on Windows and the CPU baseline anywhere.
namespace videolab
{
constexpr double IdenticalPsnr = 100.0;

inline double psnr(double mse)
{
    return mse < 1e-9 ? IdenticalPsnr : std::min(IdenticalPsnr, 10.0 * std::log10(255.0 * 255.0 / mse));
}

inline double mseRgb(const uint8_t *a, const uint8_t *b, size_t pixels)
{
    if (!pixels)
        return 0;
    uint64_t sum = 0;
    for (size_t n = 0; n < pixels * 4; n += 4)
        for (size_t k = 0; k < 3; ++k)
        {
            const int d = int(a[n + k]) - int(b[n + k]);
            sum += uint64_t(d * d);
        }
    return double(sum) / double(pixels * 3);
}

// Linear cross-fade at t in [0, 1]: what a display without motion estimation could show.
inline void blendRgba(const uint8_t *previous, const uint8_t *current, double t, uint8_t *out, size_t pixels)
{
    const int w = int(std::lround(std::min(1.0, std::max(0.0, t)) * 256.0));
    for (size_t n = 0; n < pixels * 4; n += 4)
    {
        for (size_t k = 0; k < 3; ++k)
            out[n + k] = uint8_t((int(previous[n + k]) * (256 - w) + int(current[n + k]) * w + 128) >> 8);
        out[n + 3] = 255;
    }
}

struct TileResult
{
    double worstPsnr = IdenticalPsnr;
    int x = 0, y = 0;
};

// A broken HUD element or a torn edge barely moves the whole-frame PSNR. The worst tile finds it.
inline TileResult worstTile(const uint8_t *a, const uint8_t *b, int width, int height, int tile = 64)
{
    TileResult result;
    double worstMse = -1;
    for (int ty = 0; ty < height; ty += tile)
        for (int tx = 0; tx < width; tx += tile)
        {
            const int x1 = std::min(width, tx + tile), y1 = std::min(height, ty + tile);
            uint64_t sum = 0;
            for (int y = ty; y < y1; ++y)
            {
                const uint8_t *pa = a + (size_t(y) * width + tx) * 4, *pb = b + (size_t(y) * width + tx) * 4;
                for (int x = tx; x < x1; ++x, pa += 4, pb += 4)
                    for (int k = 0; k < 3; ++k)
                    {
                        const int d = int(pa[k]) - int(pb[k]);
                        sum += uint64_t(d * d);
                    }
            }
            const double mse = double(sum) / double(size_t(x1 - tx) * (y1 - ty) * 3);
            if (mse > worstMse)
            {
                worstMse = mse;
                result.x = tx;
                result.y = ty;
            }
        }
    result.worstPsnr = psnr(std::max(0.0, worstMse));
    return result;
}

inline void lumaPlane(const uint8_t *rgba, size_t pixels, std::vector<uint8_t> &out)
{
    out.resize(pixels);
    for (size_t n = 0; n < pixels; ++n) // BT.709 weights in 8-bit fixed point
        out[n] = uint8_t((54 * rgba[n * 4] + 183 * rgba[n * 4 + 1] + 19 * rgba[n * 4 + 2] + 128) >> 8);
}

// Mean SSIM over 8x8 luma windows, stride 4.
inline double ssimLuma(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int width, int height)
{
    if (width < 8 || height < 8 || a.size() != b.size() || a.size() != size_t(width) * height)
        return 0;
    const double c1 = 6.5025, c2 = 58.5225;
    double total = 0;
    size_t windows = 0;
    for (int y = 0; y + 8 <= height; y += 4)
        for (int x = 0; x + 8 <= width; x += 4)
        {
            unsigned sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int j = 0; j < 8; ++j)
            {
                const uint8_t *pa = a.data() + size_t(y + j) * width + x, *pb = b.data() + size_t(y + j) * width + x;
                for (int i = 0; i < 8; ++i)
                {
                    const unsigned va = pa[i], vb = pb[i];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            }
            const double ma = sa / 64.0, mb = sb / 64.0;
            const double va = saa / 64.0 - ma * ma, vb = sbb / 64.0 - mb * mb, cov = sab / 64.0 - ma * mb;
            total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++windows;
        }
    return windows ? total / double(windows) : 0;
}

inline double ssimRgba(const uint8_t *a, const uint8_t *b, int width, int height)
{
    std::vector<uint8_t> la, lb;
    lumaPlane(a, size_t(width) * height, la);
    lumaPlane(b, size_t(width) * height, lb);
    return ssimLuma(la, lb, width, height);
}

struct Series
{
    std::vector<double> values;
    void add(double v) { values.push_back(v); }
    size_t count() const { return values.size(); }
    double mean() const
    {
        if (values.empty())
            return 0;
        double sum = 0;
        for (double v : values)
            sum += v;
        return sum / double(values.size());
    }
    double stddev() const
    {
        if (values.size() < 2)
            return 0;
        const double m = mean();
        double sum = 0;
        for (double v : values)
            sum += (v - m) * (v - m);
        return std::sqrt(sum / double(values.size() - 1));
    }
    double min() const { return values.empty() ? 0 : *std::min_element(values.begin(), values.end()); }
    double max() const { return values.empty() ? 0 : *std::max_element(values.begin(), values.end()); }
    // Nearest-rank percentile, p in [0, 100].
    double percentile(double p) const
    {
        if (values.empty())
            return 0;
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const double rank = std::ceil(std::min(100.0, std::max(0.0, p)) / 100.0 * double(sorted.size()));
        return sorted[size_t(std::max(1.0, rank)) - 1];
    }
};

// Which skipped source frame is the ground truth for a generated phase.
// `decimate` source frames separate two real frames; generated phase p of `multiplier - 1`
// sits at p / multiplier of that gap. Returns the 0-based index among the decimate - 1 skipped
// frames, or -1 when that instant was never captured.
inline int truthIndex(unsigned phase, unsigned multiplier, unsigned decimate)
{
    if (!phase || phase >= multiplier || decimate < 2 || (phase * decimate) % multiplier)
        return -1;
    return int(phase * decimate / multiplier) - 1;
}
} // namespace videolab
