// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include "../pipeline.h"
#include "backend.h"
extern "C"
{
#include <libplacebo/log.h>
}

namespace videolab
{
// libplacebo Vulkan device shared by the offline backend and realtime playback.
struct VulkanDevice
{
    pl_log log = nullptr;
    pl_vk_inst instance = nullptr;
    pl_vulkan vk = nullptr;
    pl_renderer renderer = nullptr;
    pl_tex source = nullptr, readback = nullptr;
    // `presentation` adds the Win32 surface extensions needed for a swapchain.
    bool create(bool presentation);
    ~VulkanDevice();
    // Uploads RGBA8 and draws it 1:1 into the pipeline's shared input texture.
    bool renderInput(const uint8_t *rgba, int width, int height, pl_tex target);
    // Downloads a pipeline output as width x height RGBA8, scaling it down first if it is larger.
    bool readOutput(const pl_frame &frame, int width, int height, uint8_t *rgba);
};
EnhancementOptions enhancementOptions(const LabOptions &options);
// The pipeline reports failures only as status text; these are the phrases it uses.
bool pipelineFailure(const QString &message);
} // namespace videolab
