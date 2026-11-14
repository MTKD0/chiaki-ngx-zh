// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace chiaki_enhancement
{
// Affine camera motion fitted to the optical-flow grid:
//   vx = a[0] + a[1]*u + a[2]*v,  vy = b[0] + b[1]*u + b[2]*v
// where (u, v) is the grid position normalised to [-0.5, 0.5] and vectors are in
// source-luma pixels. Pan, zoom and roll are covered; perspective is left to the
// local flow. Pure C++11: shared by the MSVC worker and the model test.
struct GlobalMotion
{
    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    bool valid = false;
    unsigned inliers = 0, samples = 0; // inliers: refit set when valid, else the best RANSAC agreement
};
namespace motion_detail
{
struct Sample
{
    double x, y, vx, vy;
};
inline double det3(const double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
inline bool solve3(const double m[3][3], const double r[3], double out[3], double epsilon)
{
    const double det = det3(m);
    if (std::fabs(det) < epsilon)
        return false;
    for (int k = 0; k < 3; ++k)
    {
        double c[3][3];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                c[row][column] = column == k ? r[row] : m[row][column];
        out[k] = det3(c) / det;
    }
    return true;
}
inline double error2(const double a[3], const double b[3], const Sample &s)
{
    const double dx = a[0] + a[1] * s.x + a[2] * s.y - s.vx;
    const double dy = b[0] + b[1] * s.x + b[2] * s.y - s.vy;
    return dx * dx + dy * dy;
}
// One pixel plus 5% of the vector length: fast pans carry proportionally more noise.
inline double tolerance2(const Sample &s)
{
    const double t = 1.0 + 0.05 * std::sqrt(s.vx * s.vx + s.vy * s.vy);
    return t * t;
}
} // namespace motion_detail

// `vectors` is the tightly packed NVOFA output: width*height pairs of int16 in
// S10.5 fixed point. Deterministic RANSAC followed by a least-squares refit.
// The model is reported only when at least 20% of the sampled grid agrees (recorded
// 3D scenes with parallax agree at a median of 16%, rarely above 35%). Only vectors
// that already agree with the model are snapped to it by the shader, so a
// minority model cannot bend the rest of the frame. Below the threshold a
// scene dominated by independent motion keeps its local vectors.
inline GlobalMotion fitGlobalMotion(const uint8_t *vectors, unsigned width, unsigned height)
{
    using namespace motion_detail;
    GlobalMotion result;
    if (!vectors || width < 8 || height < 8)
        return result;
    unsigned stride = static_cast<unsigned>(std::sqrt(double(width) * height / 1500.0));
    if (stride < 1)
        stride = 1;
    std::vector<Sample> samples;
    for (unsigned y = stride / 2; y < height; y += stride)
        for (unsigned x = stride / 2; x < width; x += stride)
        {
            int16_t v[2];
            std::memcpy(v, vectors + (static_cast<size_t>(y) * width + x) * 4, sizeof v);
            Sample s = {(x + 0.5) / width - 0.5, (y + 0.5) / height - 0.5, v[0] / 32.0, v[1] / 32.0};
            samples.push_back(s);
        }
    const unsigned count = static_cast<unsigned>(samples.size());
    result.samples = count;
    if (count < 64)
        return result;
    uint32_t seed = 0x9e3779b9u;
    double bestA[3] = {0, 0, 0}, bestB[3] = {0, 0, 0};
    unsigned best = 0;
    for (int iteration = 0; iteration < 48; ++iteration)
    {
        unsigned pick[3];
        for (int k = 0; k < 3; ++k)
        {
            seed = seed * 1664525u + 1013904223u;
            pick[k] = (seed >> 8) % count;
        }
        if (pick[0] == pick[1] || pick[0] == pick[2] || pick[1] == pick[2])
            continue;
        double m[3][3], rx[3], ry[3], a[3], b[3];
        for (int k = 0; k < 3; ++k)
        {
            const Sample &s = samples[pick[k]];
            m[k][0] = 1.0; m[k][1] = s.x; m[k][2] = s.y;
            rx[k] = s.vx; ry[k] = s.vy;
        }
        // Reject nearly collinear triples: they extrapolate wildly.
        if (!solve3(m, rx, a, 1e-3) || !solve3(m, ry, b, 1e-3))
            continue;
        unsigned agreeing = 0;
        for (unsigned n = 0; n < count; ++n)
            if (error2(a, b, samples[n]) <= tolerance2(samples[n]))
                ++agreeing;
        if (agreeing > best)
        {
            best = agreeing;
            for (int k = 0; k < 3; ++k) { bestA[k] = a[k]; bestB[k] = b[k]; }
        }
    }
    if (best * 100 < count * 20)
    {
        result.inliers = best; // Reported for diagnostics; the model stays invalid.
        return result;
    }
    double normal[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, rx[3] = {0, 0, 0}, ry[3] = {0, 0, 0};
    unsigned inliers = 0;
    for (unsigned n = 0; n < count; ++n)
    {
        const Sample &s = samples[n];
        if (error2(bestA, bestB, s) > tolerance2(s))
            continue;
        const double basis[3] = {1.0, s.x, s.y};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
                normal[row][column] += basis[row] * basis[column];
            rx[row] += basis[row] * s.vx;
            ry[row] += basis[row] * s.vy;
        }
        ++inliers;
    }
    double a[3], b[3];
    if (!solve3(normal, rx, a, 1e-9) || !solve3(normal, ry, b, 1e-9))
        for (int k = 0; k < 3; ++k) { a[k] = bestA[k]; b[k] = bestB[k]; }
    for (int k = 0; k < 3; ++k)
    {
        result.a[k] = static_cast<float>(a[k]);
        result.b[k] = static_cast<float>(b[k]);
    }
    result.inliers = inliers;
    result.valid = true;
    return result;
}
} // namespace chiaki_enhancement
