// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "../protocol.h"
#include "../runtime_route.h"
#include "../motion_model.h"
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace chiaki_enhancement;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main()
{
    CHECK(maxFgMultiplier(L"NVIDIA GeForce RTX 5070 Ti Laptop GPU") == 4);
    CHECK(maxFgMultiplier(L"NVIDIA GeForce RTX 4090") == 2);
    CHECK(maxFgMultiplier(L"NVIDIA GeForce RTX 3060") == 2);
    CHECK(maxFgMultiplier(L"NVIDIA RTX 5000 Ada Generation") == 2);
    CHECK(maxFgMultiplier(L"Unknown GPU") == 2);
    auto e = nrExtent(2560, 1440, 50);
    CHECK(e.width == 1280 && e.height == 720);
    e = nrExtent(1920, 1080, 65);
    CHECK(e.width == 1248 && e.height == 702);
    e = nrExtent(1920, 1080, 100);
    CHECK(e.width == 1920 && e.height == 1080);
    e = nrExtent(3840, 2160, 100);
    CHECK(e.width == 2560 && e.height == 1440);
    e = nrExtent(1080, 2560, 100);
    CHECK(e.height == 1440 && e.width <= 608);
    CHECK(!canUpscale(e, e));
    CHECK(canUpscale(nrExtent(1920, 1080, 100), nrExtent(1920, 1080, 50)));
    CHECK(nrExtent(0, 1080, 65).width == 0);
    CHECK(std::abs(presentationInterval(1.0 / 60, 1.0 / 120, true, true, false) - 0.0) <
          1e-9);
    CHECK(std::abs(presentationInterval(1.0 / 60, 1.0 / 60, true, true, false) - 0.0) < 1e-9);
    CHECK(std::abs(presentationInterval(1.0 / 30, 1.0 / 120, true, true, false) - 0.0) < 1e-9);
    CHECK(std::abs(presentationInterval(1.0 / 60, 1.0 / 120, true, false, false) - 1.0 / 60) <
          1e-9);
    Command c;
    c.width = 1920;
    c.height = 1080;
    CHECK(valid(c));
    c.fgMultiplier = 1; CHECK(!valid(c));
    c.fgMultiplier = 5; CHECK(!valid(c));
    c.fgMultiplier = 3; CHECK(valid(c));
    c.fgMultiplier = 4; CHECK(valid(c));
    c.fgMultiplier = 2;
    c.percent = 0;
    CHECK(!valid(c));
    c.percent = 65;
    c.operation = Operation::Process;
    c.fenceValue = 1;
    CHECK(valid(c));
    c.slot = SlotCount;
    CHECK(!valid(c));
    c.slot = 0;
    c.fenceValue = 2;
    CHECK(!valid(c));
    c.fenceValue = 1;
    c.flags = 0x8000;
    CHECK(!valid(c));
    c.flags = Neural | Fsr;
    c.flags |= HdrLinear;
    CHECK(valid(c));
    c.flags &= ~HdrLinear;
    c.nrPassCount = 3;
    c.additionalPasses[0].percent = 75;
    c.additionalPasses[0].fsr = 0;
    c.additionalPasses[1].percent = 100;
    c.additionalPasses[1].sharpness = 85;
    CHECK(valid(c));
    CHECK(passConfig(c, 0).percent == 65 && passConfig(c, 0).fsr == 1);
    CHECK(passConfig(c, 1).percent == 75 && passConfig(c, 1).fsr == 0);
    CHECK(passConfig(c, 2).percent == 100 && passConfig(c, 2).sharpness == 85);
    Command changed = c;
    changed.additionalPasses[1].sharpness = 0;
    CHECK(sameNrResources(c, changed));
    changed.additionalPasses[0].percent = 70;
    CHECK(!sameNrResources(c, changed));
    changed = c;
    changed.nrPassCount = 2;
    CHECK(!sameNrResources(c, changed));
    c.nrPassCount = 0; CHECK(!valid(c));
    c.nrPassCount = 4; CHECK(!valid(c));
    c.nrPassCount = 3;
    c.additionalPasses[0].percent = 49; CHECK(!valid(c));
    c.additionalPasses[0].percent = 65;
    c.additionalPasses[1].fsr = 2; CHECK(!valid(c));
    c.additionalPasses[1].fsr = 1;
    c.additionalPasses[1].sharpness = 101; CHECK(!valid(c));
    c.additionalPasses[1].sharpness = 20;
    c.version = 3; CHECK(!valid(c));
    c.version = Version;
    c.nrTuning.localStructure = 2.0f;
    c.nrTuning.localTone = 0.25f;
    c.nrTuning.skinStructure = 2.5f;
    c.nrTuning.style = 2;
    c.nrTuning.autoMask = 1;
    CHECK(valid(c));
    CHECK(passConfig(c, 0).tuning == c.nrTuning);
    CHECK(passConfig(c, 1).tuning.style == 0);
    changed = c; changed.nrTuning.style = 1;
    changed.additionalPasses[1].tuning.localTone = 0;
    CHECK(sameNrResources(c, changed));
    changed.nrTuning.style = 3; CHECK(!valid(changed));
    changed = c; changed.additionalPasses[0].tuning.autoMask = 2; CHECK(!valid(changed));
    changed = c; changed.nrTuning.localTone = -0.1f; CHECK(!valid(changed));
    changed = c; changed.additionalPasses[1].tuning.skinStructure = 2.6f; CHECK(!valid(changed));
    changed = c; changed.nrTuning.localStructure = std::nanf(""); CHECK(!valid(changed));
    changed = c; changed.additionalPasses[0].tuning.localTone = INFINITY; CHECK(!valid(changed));
    changed = c; changed.nrTuning.intensity = -0.1f; CHECK(!valid(changed));
    changed = c; changed.additionalPasses[0].tuning.intensity = 1.1f; CHECK(!valid(changed));
    changed = c; changed.nrTuning.intensity = std::nanf(""); CHECK(!valid(changed));
    changed = c; changed.additionalPasses[1].tuning.intensity = INFINITY; CHECK(!valid(changed));
    changed = c; changed.nrTuning.intensity = 0; CHECK(valid(changed)); CHECK(sameNrResources(c, changed));
    CHECK(passConfig(changed, 0).tuning.intensity == 0 && passConfig(changed, 1).tuning.intensity == 1);
    changed = c; changed.version = 4; CHECK(!valid(changed));
    c.version = 2; CHECK(!valid(c));
    c.version = Version;
    changed = c; changed.vsrQuality = 5; CHECK(!valid(changed));
    changed = c; changed.flags |= Vsr; CHECK(!valid(changed));
    changed.vsrQuality = 3; changed.inputWidth = 640; changed.inputHeight = 360;
    changed.width = 1280; changed.height = 720; CHECK(valid(changed));
    CHECK(inputExtent(changed).width == 640);
    CHECK(nrBaseExtent(changed).width == 640);
    changed.flags |= VsrFirst; CHECK(valid(changed)); CHECK(nrBaseExtent(changed).width == 1280);
    changed.flags |= HdrLinear; CHECK(nrBaseExtent(changed).width == 640); changed.flags &= ~uint32_t(HdrLinear);
    changed.flags &= ~uint32_t(Vsr); CHECK(!valid(changed)); changed.flags |= Vsr;
    changed.flags &= ~uint32_t(VsrFirst); CHECK(valid(changed));
    changed.inputHeight = 0; CHECK(!valid(changed));
    uint64_t lastGenerated = 0, counted = 0;
    for (unsigned frame = 1; frame <= 30; ++frame)
        for (unsigned phase = 1; phase <= 3; ++phase) {
            auto key = presentationKey(frame, phase);
            if (key != lastGenerated) { ++counted; lastGenerated = key; }
            CHECK(key == presentationKey(frame, phase)); // A repeated render must not add a phase.
        }
    CHECK(counted == 90); // 30 real + 90 generated = 120, rather than the former 60.
    {
        // Camera pan + slight zoom over a 120x68 flow grid, with a quarter of the
        // grid moving independently: the model must follow the camera only.
        const unsigned gw = 120, gh = 68;
        std::vector<uint8_t> grid(size_t(gw) * gh * 4);
        auto put = [&](unsigned x, unsigned y, double vx, double vy) {
            int16_t v[2] = {int16_t(std::lround(vx * 32.0)), int16_t(std::lround(vy * 32.0))};
            std::memcpy(grid.data() + (size_t(y) * gw + x) * 4, v, 4);
        };
        for (unsigned y = 0; y < gh; ++y)
            for (unsigned x = 0; x < gw; ++x) {
                const double u = (x + 0.5) / gw - 0.5, v = (y + 0.5) / gh - 0.5;
                if (x < gw / 2 && y < gh / 2) put(x, y, -20.0, 9.0);
                else put(x, y, 6.5 + 4.0 * u, -2.25 + 4.0 * v);
            }
        const auto model = fitGlobalMotion(grid.data(), gw, gh);
        CHECK(model.valid);
        CHECK(std::fabs(model.a[0] - 6.5f) < 0.1f && std::fabs(model.b[0] + 2.25f) < 0.1f);
        CHECK(std::fabs(model.a[1] - 4.0f) < 0.2f && std::fabs(model.b[2] - 4.0f) < 0.2f);
        CHECK(std::fabs(model.a[2]) < 0.2f && std::fabs(model.b[1]) < 0.2f);
        CHECK(model.inliers * 100 >= model.samples * 70 && model.inliers * 100 <= model.samples * 80);
        // No dominant motion: every cell differs, so no model may be reported.
        uint32_t seed = 12345;
        for (unsigned y = 0; y < gh; ++y)
            for (unsigned x = 0; x < gw; ++x) {
                seed = seed * 1664525u + 1013904223u; const double vx = int((seed >> 8) % 801) / 10.0 - 40.0;
                seed = seed * 1664525u + 1013904223u; const double vy = int((seed >> 8) % 801) / 10.0 - 40.0;
                put(x, y, vx, vy);
            }
        const auto rejected = fitGlobalMotion(grid.data(), gw, gh);
        CHECK(!rejected.valid && rejected.samples > 0 && rejected.inliers * 100 < rejected.samples * 20);
        CHECK(!fitGlobalMotion(nullptr, gw, gh).valid);
    }
    std::puts("Enhancement geometry, VSR input/output, 4x phase identity, global motion fit and IPC validation passed");
}
