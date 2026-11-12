// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "metrics.h"
#include <cstdio>
#include <cstdlib>

using namespace videolab;

#define CHECK(condition) \
    do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

static bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// A bright square on a dark gradient, displaced by `offset` pixels.
static std::vector<uint8_t> scene(int width, int height, int offset)
{
    std::vector<uint8_t> frame(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            uint8_t *p = &frame[(size_t(y) * width + x) * 4];
            const bool square = x >= 16 + offset && x < 48 + offset && y >= 16 && y < 48;
            p[0] = square ? 230 : uint8_t(x / 2);
            p[1] = square ? 200 : uint8_t(y / 2);
            p[2] = square ? 40 : 60;
            p[3] = 255;
        }
    return frame;
}

int main()
{
    const int width = 128, height = 96;
    const size_t pixels = size_t(width) * height;
    const auto a = scene(width, height, 0), b = scene(width, height, 8), middle = scene(width, height, 4);

    CHECK(mseRgb(a.data(), a.data(), pixels) == 0);
    CHECK(psnr(0) == IdenticalPsnr);
    CHECK(near(psnr(1.0), 48.13, 0.01));
    CHECK(near(ssimRgba(a.data(), a.data(), width, height), 1.0, 1e-9));

    // Uniform error of 2 on every colour channel; alpha is ignored.
    auto shifted = a;
    for (size_t n = 0; n < shifted.size(); ++n)
        shifted[n] = uint8_t(n % 4 == 3 ? 0 : (shifted[n] > 250 ? shifted[n] - 2 : shifted[n] + 2));
    CHECK(near(mseRgb(a.data(), shifted.data(), pixels), 4.0, 1e-9));

    // Cross-fade end points and midpoint.
    std::vector<uint8_t> out(a.size());
    blendRgba(a.data(), b.data(), 0.0, out.data(), pixels);
    CHECK(mseRgb(out.data(), a.data(), pixels) == 0);
    blendRgba(a.data(), b.data(), 1.0, out.data(), pixels);
    CHECK(mseRgb(out.data(), b.data(), pixels) == 0);
    blendRgba(a.data(), b.data(), 0.5, out.data(), pixels);
    CHECK(out[(size_t(20) * width + 20) * 4] == uint8_t((230 + 10 + 1) / 2)); // square leaves this pixel in b
    CHECK(out[3] == 255);

    // A correctly placed square beats the ghosted cross-fade, which beats repeating the old frame.
    const double perfect = psnr(mseRgb(middle.data(), middle.data(), pixels));
    const double blended = psnr(mseRgb(out.data(), middle.data(), pixels));
    const double repeated = psnr(mseRgb(a.data(), middle.data(), pixels));
    CHECK(perfect > blended && blended > repeated);
    CHECK(ssimRgba(out.data(), middle.data(), width, height) < 0.999);

    // Damage confined to one tile is found there even though the frame average hardly moves.
    auto damaged = a;
    for (int y = 64; y < 96; ++y)
        for (int x = 64; x < 128; ++x)
            damaged[(size_t(y) * width + x) * 4 + 1] ^= 0x80;
    const TileResult tile = worstTile(a.data(), damaged.data(), width, height);
    CHECK(tile.x == 64 && tile.y == 64);
    CHECK(tile.worstPsnr < psnr(mseRgb(a.data(), damaged.data(), pixels)));
    CHECK(worstTile(a.data(), a.data(), width, height).worstPsnr == IdenticalPsnr);

    // Ground-truth alignment between generated phases and skipped source frames.
    CHECK(truthIndex(1, 2, 2) == 0);                                                    // 60 -> 30, 2x
    CHECK(truthIndex(1, 3, 3) == 0 && truthIndex(2, 3, 3) == 1);                         // 90 -> 30, 3x
    CHECK(truthIndex(1, 4, 4) == 0 && truthIndex(2, 4, 4) == 1 && truthIndex(3, 4, 4) == 2);
    CHECK(truthIndex(1, 2, 4) == 1);                                                    // 120 -> 30, 2x: the middle frame
    CHECK(truthIndex(1, 4, 2) == -1 && truthIndex(2, 4, 2) == 0 && truthIndex(3, 4, 2) == -1);
    CHECK(truthIndex(1, 2, 1) == -1 && truthIndex(0, 2, 2) == -1 && truthIndex(2, 2, 2) == -1);
    CHECK(truthIndex(1, 2, 3) == -1);                                                   // t = 0.5 falls between frames

    Series series;
    for (int n = 1; n <= 100; ++n)
        series.add(n);
    CHECK(series.mean() == 50.5 && series.min() == 1 && series.max() == 100);
    CHECK(series.percentile(5) == 5 && series.percentile(50) == 50 && series.percentile(100) == 100);
    CHECK(near(series.stddev(), 29.0115, 1e-3));
    CHECK(Series().mean() == 0 && Series().percentile(50) == 0);
    std::puts("videolab metrics: PASS");
    return 0;
}
