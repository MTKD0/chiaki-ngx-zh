// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "vulkan_device.h"

namespace videolab
{
EnhancementOptions enhancementOptions(const LabOptions &o)
{
    EnhancementOptions options;
    // Keeps real frames flowing while FG warms up or is switched off, as the bridge test does.
    options.bridgeTest = true;
    options.frameGeneration = o.frameGeneration;
    options.fgMultiplier = o.multiplier;
    options.neural = o.neural;
    options.fsr = o.fsr;
    options.percent = o.percent;
    options.sharpness = o.sharpness;
    if (o.vsr)
    {
        options.vsrQuality = o.vsr;
        options.inputWidth = unsigned(o.width);
        options.inputHeight = unsigned(o.height);
    }
    return options;
}

bool pipelineFailure(const QString &message)
{
    return message.contains("crashed") || message.contains("timed out") || message.startsWith("NVOFA") ||
           message.startsWith("NGX") || message.contains("could not", Qt::CaseInsensitive) ||
           message.contains("failed", Qt::CaseInsensitive) || message.contains("not installed") ||
           message.contains("unavailable") || message.contains("manifest") || message.contains("package changed") ||
           message.contains("protocol") || message.contains("require") || message.contains("exited") ||
           message.contains("invalid") || message.contains("stale") || message.contains("Unknown") ||
           message.contains("LUID") || message.contains("Cannot");
}

bool VulkanDevice::create(bool presentation)
{
    pl_log_params logParams{};
    logParams.log_cb = pl_log_simple;
    logParams.log_level = PL_LOG_ERR;
    log = pl_log_create(PL_API_VER, &logParams);
    if (presentation)
    {
        const char *instanceExtensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        pl_vk_inst_params params{};
        params.extensions = instanceExtensions;
        params.num_extensions = 2;
        instance = pl_vk_inst_create(log, &params);
        if (!instance)
            return false;
    }
    const char *extensions[] = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
    pl_vulkan_params p{};
    p.async_compute = true;
    p.async_transfer = true;
    p.opt_extensions = extensions;
    p.num_opt_extensions = 2;
    if (instance)
    {
        p.instance = instance->instance;
        p.get_proc_addr = instance->get_proc_addr;
    }
    vk = pl_vulkan_create(log, &p);
    if (!vk)
        return false;
    renderer = pl_renderer_create(log, vk->gpu);
    return renderer;
}

VulkanDevice::~VulkanDevice()
{
    if (source)
        pl_tex_destroy(vk->gpu, &source);
    if (readback)
        pl_tex_destroy(vk->gpu, &readback);
    if (renderer)
        pl_renderer_destroy(&renderer);
    if (vk)
        pl_vulkan_destroy(&vk);
    if (instance)
        pl_vk_inst_destroy(&instance);
    if (log)
        pl_log_destroy(&log);
}

bool VulkanDevice::renderInput(const uint8_t *rgba, int width, int height, pl_tex target)
{
    pl_tex_params textureParams{};
    textureParams.w = width;
    textureParams.h = height;
    textureParams.format = pl_find_named_fmt(vk->gpu, "rgba8");
    textureParams.sampleable = true;
    textureParams.host_writable = true;
    if (!textureParams.format || !pl_tex_recreate(vk->gpu, &source, &textureParams))
        return false;
    pl_tex_transfer_params transfer{};
    transfer.tex = source;
    transfer.ptr = const_cast<uint8_t *>(rgba);
    if (!pl_tex_upload(vk->gpu, &transfer))
        return false;
    pl_frame image{}, native{};
    image.num_planes = native.num_planes = 1;
    image.planes[0].texture = source;
    native.planes[0].texture = target;
    image.planes[0].components = native.planes[0].components = 4;
    for (int c = 0; c < 4; ++c)
        image.planes[0].component_mapping[c] = native.planes[0].component_mapping[c] = c;
    image.repr = native.repr = pl_color_repr_rgb;
    image.repr.alpha = native.repr.alpha = PL_ALPHA_NONE;
    image.color = native.color = pl_color_space_srgb;
    image.crop = {0, 0, float(width), float(height)};
    native.crop = {0, 0, float(target->params.w), float(target->params.h)};
    return pl_render_image(renderer, &image, &native, &pl_render_default_params);
}

bool VulkanDevice::readOutput(const pl_frame &frame, int width, int height, uint8_t *rgba)
{
    pl_tex texture = frame.planes[0].texture;
    if (texture->params.w != width || texture->params.h != height)
    {
        // Upscaled output is scored against source-resolution ground truth.
        pl_tex_params params{};
        params.w = width;
        params.h = height;
        params.format = pl_find_named_fmt(vk->gpu, "rgba8");
        params.renderable = true;
        params.host_readable = true;
        if (!params.format || !pl_tex_recreate(vk->gpu, &readback, &params))
            return false;
        pl_frame target{};
        target.num_planes = 1;
        target.planes[0].texture = readback;
        target.planes[0].components = 4;
        for (int c = 0; c < 4; ++c)
            target.planes[0].component_mapping[c] = c;
        target.repr = pl_color_repr_rgb;
        target.repr.alpha = PL_ALPHA_NONE;
        target.color = frame.color;
        target.crop = {0, 0, float(width), float(height)};
        pl_render_params render = pl_render_default_params;
        render.dither_params = nullptr; // Dither noise would lower every score.
        if (!pl_render_image(renderer, &frame, &target, &render))
            return false;
        texture = readback;
    }
    pl_tex_transfer_params transfer{};
    transfer.tex = texture;
    transfer.ptr = rgba;
    return pl_tex_download(vk->gpu, &transfer);
}

namespace
{
class PipelineBackend : public Backend
{
  public:
    explicit PipelineBackend(const LabOptions &o)
        : width(o.width), height(o.height), outputWidth(o.vsr ? o.outputWidth : o.width),
          outputHeight(o.vsr ? o.outputHeight : o.height), options(enhancementOptions(o)), scratch(size_t(o.width) * o.height * 4)
    {
    }
    ~PipelineBackend() override { stop(); }
    QString name() const override { return QStringLiteral("enhancement-pipeline"); }
    void start() override
    {
        if (!device.create(false))
        {
            fail(QStringLiteral("Cannot create a Vulkan device"));
            return;
        }
        pipeline.reset(new EnhancementPipeline);
        QObject::connect(pipeline.get(), &EnhancementPipeline::statusChanged, &context,
                         [this](QString message, int, int, int, int, bool, bool, bool) {
                             if (message != lastStatus && status)
                                 status(message);
                             lastStatus = message;
                             if (stopping)
                                 return;
                             const bool featureMissing = message.startsWith("GPU bridge ready") &&
                                 ((options.frameGeneration && message.contains("; FG:")) ||
                                  (options.neural && message.contains("; NR:")));
                             // After a failure the pipeline has already stopped its worker.
                             if (pipelineFailure(message) || featureMissing ||
                                 (configured && !pipeline->ownsPresentation()))
                                 fail(message);
                             else if (message.startsWith("GPU bridge ready") && !configured)
                             {
                                 configured = true;
                                 if (ready)
                                     ready();
                             }
                         });
        QObject::connect(pipeline.get(), &EnhancementPipeline::timingsChanged, &context,
                         [this](double nr, double, double fg, double vsr, double flow, double, bool vsrActive) {
                             vsrSeen |= vsrActive;
                             if (vsr > 0)
                                 vsrMs.add(vsr);
                             if (fg > 0)
                                 fgMs.add(fg);
                             if (flow > 0)
                                 flowMs.add(flow);
                             if (nr > 0)
                                 nrMs.add(nr);
                         });
        QObject::connect(pipeline.get(), &EnhancementPipeline::frameAvailable, &context, [this] { drain(); });
        pipeline->prepare(device.vk, {uint32_t(outputWidth), uint32_t(outputHeight)}, options, false);
    }
    bool submit(uint64_t frameId, uint64_t timestampUs, const uint8_t *rgba) override
    {
        if (!pipeline || broken || !configured || pending)
            return false;
        pl_tex input = pipeline->beginFrame(frameId, timestampUs);
        if (!input)
            return false;
        if (!device.renderInput(rgba, width, height, input) || !pipeline->submitFrame())
        {
            fail(QStringLiteral("Cannot hand the frame to the enhancement pipeline"));
            return false;
        }
        pending = frameId;
        return true;
    }
    void stop() override
    {
        if (pipeline)
        {
            stopping = true;
            pipeline->reset();
            pipeline.reset();
        }
    }

  private:
    void fail(const QString &message)
    {
        if (broken)
            return;
        broken = true;
        if (failed)
            failed(message);
    }
    void drain()
    {
        if (broken || !pending || !pipeline)
            return;
        if (pipeline->hasUnpresentedOutput())
        {
            pl_frame frame{};
            if (!pipeline->outputFrame(frame))
                return;
            if (!device.readOutput(frame, width, height, scratch.data()))
            {
                fail(QStringLiteral("Output download failed"));
                return;
            }
            const unsigned phase = pipeline->outputPhase();
            output(pending, phase, scratch.data());
            if (broken || !pipeline)
                return;
            // Marks this phase shown; the pipeline then schedules the next one itself.
            pipeline->notePresented(0);
        }
        if (pipeline->completedFrameId() == pending && !pipeline->generatedOutput() &&
            !pipeline->hasUnpresentedOutput())
        {
            const uint64_t finished = pending;
            pending = 0;
            frameDone(finished);
        }
    }

    QObject context;
    VulkanDevice device;
    std::unique_ptr<EnhancementPipeline> pipeline;
    int width, height, outputWidth, outputHeight;
    EnhancementOptions options;
    std::vector<uint8_t> scratch;
    QString lastStatus;
    uint64_t pending = 0;
    bool configured = false, broken = false, stopping = false;
};
} // namespace

std::unique_ptr<Backend> createPipelineBackend(const LabOptions &options)
{
    return std::unique_ptr<Backend>(new PipelineBackend(options));
}
} // namespace videolab
