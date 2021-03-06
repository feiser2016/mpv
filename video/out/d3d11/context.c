/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/msg.h"
#include "options/m_config.h"
#include "osdep/timer.h"
#include "osdep/windows_utils.h"

#include "video/out/gpu/context.h"
#include "video/out/gpu/d3d11_helpers.h"
#include "video/out/gpu/spirv.h"
#include "video/out/w32_common.h"
#include "ra_d3d11.h"

static int d3d11_validate_adapter(struct mp_log *log,
                                  const struct m_option *opt,
                                  struct bstr name, struct bstr param);

struct d3d11_opts {
    int feature_level;
    int warp;
    int flip;
    int sync_interval;
    char *adapter_name;
    int output_format;
};

#define OPT_BASE_STRUCT struct d3d11_opts
const struct m_sub_options d3d11_conf = {
    .opts = (const struct m_option[]) {
        OPT_CHOICE("d3d11-warp", warp, 0,
                   ({"auto", -1},
                    {"no", 0},
                    {"yes", 1})),
        OPT_CHOICE("d3d11-feature-level", feature_level, 0,
                   ({"12_1", D3D_FEATURE_LEVEL_12_1},
                    {"12_0", D3D_FEATURE_LEVEL_12_0},
                    {"11_1", D3D_FEATURE_LEVEL_11_1},
                    {"11_0", D3D_FEATURE_LEVEL_11_0},
                    {"10_1", D3D_FEATURE_LEVEL_10_1},
                    {"10_0", D3D_FEATURE_LEVEL_10_0},
                    {"9_3", D3D_FEATURE_LEVEL_9_3},
                    {"9_2", D3D_FEATURE_LEVEL_9_2},
                    {"9_1", D3D_FEATURE_LEVEL_9_1})),
        OPT_FLAG("d3d11-flip", flip, 0),
        OPT_INTRANGE("d3d11-sync-interval", sync_interval, 0, 0, 4),
        OPT_STRING_VALIDATE("d3d11-adapter", adapter_name, 0,
                            d3d11_validate_adapter),
        OPT_CHOICE("d3d11-output-format", output_format, 0,
                   ({"auto",     DXGI_FORMAT_UNKNOWN},
                    {"rgba8",    DXGI_FORMAT_R8G8B8A8_UNORM},
                    {"bgra8",    DXGI_FORMAT_B8G8R8A8_UNORM},
                    {"rgb10_a2", DXGI_FORMAT_R10G10B10A2_UNORM},
                    {"rgba16f",  DXGI_FORMAT_R16G16B16A16_FLOAT})),
        {0}
    },
    .defaults = &(const struct d3d11_opts) {
        .feature_level = D3D_FEATURE_LEVEL_12_1,
        .warp = -1,
        .flip = 1,
        .sync_interval = 1,
        .adapter_name = NULL,
        .output_format = DXGI_FORMAT_UNKNOWN,
    },
    .size = sizeof(struct d3d11_opts)
};

struct priv {
    struct d3d11_opts *opts;

    struct ra_tex *backbuffer;
    ID3D11Device *device;
    IDXGISwapChain *swapchain;

    int64_t perf_freq;
    unsigned last_sync_refresh_count;
    int64_t last_sync_qpc_time;
    int64_t vsync_duration_qpc;
    int64_t last_submit_qpc;
};

static int d3d11_validate_adapter(struct mp_log *log,
                                  const struct m_option *opt,
                                  struct bstr name, struct bstr param)
{
    bool help = bstr_equals0(param, "help");
    bool adapter_matched = false;
    struct bstr listing = { 0 };

    if (bstr_equals0(param, "")) {
        return 0;
    }

    adapter_matched = mp_d3d11_list_or_verify_adapters(log,
                                                       help ? bstr0(NULL) : param,
                                                       help ? &listing : NULL);

    if (help) {
        mp_info(log, "Available D3D11 adapters:\n%.*s",
                BSTR_P(listing));
        talloc_free(listing.start);
        return M_OPT_EXIT;
    }

    if (!adapter_matched) {
        mp_err(log, "No adapter matching '%.*s'!\n", BSTR_P(param));
    }

    return adapter_matched ? 0 : M_OPT_INVALID;
}

static struct ra_tex *get_backbuffer(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ID3D11Texture2D *backbuffer = NULL;
    struct ra_tex *tex = NULL;
    HRESULT hr;

    hr = IDXGISwapChain_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                  (void**)&backbuffer);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Couldn't get swapchain image\n");
        goto done;
    }

    tex = ra_d3d11_wrap_tex(ctx->ra, (ID3D11Resource *)backbuffer);
done:
    SAFE_RELEASE(backbuffer);
    return tex;
}

static bool resize(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    HRESULT hr;

    ra_tex_free(ctx->ra, &p->backbuffer);

    hr = IDXGISwapChain_ResizeBuffers(p->swapchain, 0, ctx->vo->dwidth,
        ctx->vo->dheight, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        MP_FATAL(ctx, "Couldn't resize swapchain: %s\n", mp_HRESULT_to_str(hr));
        return false;
    }

    p->backbuffer = get_backbuffer(ctx);

    return true;
}

static bool d3d11_reconfig(struct ra_ctx *ctx)
{
    vo_w32_config(ctx->vo);
    return resize(ctx);
}

static int d3d11_color_depth(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;

    if (!p->backbuffer)
        return 0;

    return p->backbuffer->params.format->component_depth[0];
}

static bool d3d11_start_frame(struct ra_swapchain *sw, struct ra_fbo *out_fbo)
{
    struct priv *p = sw->priv;

    if (!p->backbuffer)
        return false;

    *out_fbo = (struct ra_fbo) {
        .tex = p->backbuffer,
        .flip = false,
    };
    return true;
}

static bool d3d11_submit_frame(struct ra_swapchain *sw,
                               const struct vo_frame *frame)
{
    ra_d3d11_flush(sw->ctx->ra);
    return true;
}

static int64_t qpc_to_us(struct ra_swapchain *sw, int64_t qpc)
{
    struct priv *p = sw->priv;

    // Convert QPC units (1/perf_freq seconds) to microseconds. This will work
    // without overflow because the QPC value is guaranteed not to roll-over
    // within 100 years, so perf_freq must be less than 2.9*10^9.
    return qpc / p->perf_freq * 1000000 +
        qpc % p->perf_freq * 1000000 / p->perf_freq;
}

static int64_t qpc_us_now(struct ra_swapchain *sw)
{
    LARGE_INTEGER perf_count;
    QueryPerformanceCounter(&perf_count);
    return qpc_to_us(sw, perf_count.QuadPart);
}

static void d3d11_swap_buffers(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;

    LARGE_INTEGER perf_count;
    QueryPerformanceCounter(&perf_count);
    p->last_submit_qpc = perf_count.QuadPart;

    IDXGISwapChain_Present(p->swapchain, p->opts->sync_interval, 0);
}

static void d3d11_get_vsync(struct ra_swapchain *sw, struct vo_vsync_info *info)
{
    struct priv *p = sw->priv;
    HRESULT hr;

    // The calculations below are only valid if mpv presents on every vsync
    if (p->opts->sync_interval != 1)
        return;

    // GetLastPresentCount returns a sequential ID for the frame submitted by
    // the last call to IDXGISwapChain::Present()
    UINT submit_count;
    hr = IDXGISwapChain_GetLastPresentCount(p->swapchain, &submit_count);
    if (FAILED(hr))
        return;

    // GetFrameStatistics returns two pairs. The first is (PresentCount,
    // PresentRefreshCount) which relates a present ID (on the same timeline as
    // GetLastPresentCount) to the physical vsync it was displayed on. The
    // second is (SyncRefreshCount, SyncQPCTime), which relates a physical vsync
    // to a timestamp on the same clock as QueryPerformanceCounter.
    DXGI_FRAME_STATISTICS stats;
    hr = IDXGISwapChain_GetFrameStatistics(p->swapchain, &stats);
    if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        p->last_sync_refresh_count = 0;
        p->last_sync_qpc_time = 0;
    }
    if (FAILED(hr))
        return;

    // Detecting skipped vsyncs is possible but not supported yet
    info->skipped_vsyncs = 0;

    // Get the number of physical vsyncs that have passed since the last call.
    // Check for 0 here, since sometimes GetFrameStatistics returns S_OK but
    // with 0s in some (all?) members of DXGI_FRAME_STATISTICS.
    unsigned src_passed = 0;
    if (stats.SyncRefreshCount && p->last_sync_refresh_count)
        src_passed = stats.SyncRefreshCount - p->last_sync_refresh_count;
    p->last_sync_refresh_count = stats.SyncRefreshCount;

    // Get the elapsed time passed between the above vsyncs
    unsigned sqt_passed = 0;
    if (stats.SyncQPCTime.QuadPart && p->last_sync_qpc_time)
        sqt_passed = stats.SyncQPCTime.QuadPart - p->last_sync_qpc_time;
    p->last_sync_qpc_time = stats.SyncQPCTime.QuadPart;

    // If any vsyncs have passed, estimate the physical frame rate
    if (src_passed && sqt_passed)
        p->vsync_duration_qpc = sqt_passed / src_passed;
    if (p->vsync_duration_qpc)
        info->vsync_duration = qpc_to_us(sw, p->vsync_duration_qpc);

    // If the physical frame rate is known and the other members of
    // DXGI_FRAME_STATISTICS are non-0, estimate the timing of the next frame
    if (p->vsync_duration_qpc && stats.PresentCount &&
        stats.PresentRefreshCount && stats.SyncRefreshCount &&
        stats.SyncQPCTime.QuadPart)
    {
        // PresentRefreshCount and SyncRefreshCount might refer to different
        // frames (this can definitely occur in bitblt-mode.) Assuming mpv
        // presents on every frame, guess the present count that relates to
        // SyncRefreshCount.
        unsigned expected_sync_pc = stats.PresentCount +
            (stats.SyncRefreshCount - stats.PresentRefreshCount);

        // Now guess the timestamp of the last submitted frame based on the
        // timestamp of the frame at SyncRefreshCount and the frame rate
        int queued_frames = submit_count - expected_sync_pc;
        int64_t last_queue_display_time_qpc = stats.SyncQPCTime.QuadPart +
            queued_frames * p->vsync_duration_qpc;

        // Only set the estimated display time if it's after the last submission
        // time. It could be before if mpv skips a lot of frames.
        if (last_queue_display_time_qpc >= p->last_submit_qpc) {
            info->last_queue_display_time = mp_time_us() +
                (qpc_to_us(sw, last_queue_display_time_qpc) - qpc_us_now(sw));
        }
    }
}

static int d3d11_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    int ret = vo_w32_control(ctx->vo, events, request, arg);
    if (*events & VO_EVENT_RESIZE) {
        if (!resize(ctx))
            return VO_ERROR;
    }
    return ret;
}

static void d3d11_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (ctx->ra)
        ra_tex_free(ctx->ra, &p->backbuffer);
    SAFE_RELEASE(p->swapchain);
    vo_w32_uninit(ctx->vo);
    SAFE_RELEASE(p->device);

    // Destory the RA last to prevent objects we hold from showing up in D3D's
    // leak checker
    if (ctx->ra)
        ctx->ra->fns->destroy(ctx->ra);
}

static const struct ra_swapchain_fns d3d11_swapchain = {
    .color_depth  = d3d11_color_depth,
    .start_frame  = d3d11_start_frame,
    .submit_frame = d3d11_submit_frame,
    .swap_buffers = d3d11_swap_buffers,
    .get_vsync    = d3d11_get_vsync,
};

static bool d3d11_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    p->opts = mp_get_config_group(ctx, ctx->global, &d3d11_conf);

    LARGE_INTEGER perf_freq;
    QueryPerformanceFrequency(&perf_freq);
    p->perf_freq = perf_freq.QuadPart;

    struct ra_swapchain *sw = ctx->swapchain = talloc_zero(ctx, struct ra_swapchain);
    sw->priv = p;
    sw->ctx = ctx;
    sw->fns = &d3d11_swapchain;

    struct d3d11_device_opts dopts = {
        .debug = ctx->opts.debug,
        .allow_warp = p->opts->warp != 0,
        .force_warp = p->opts->warp == 1,
        .max_feature_level = p->opts->feature_level,
        .max_frame_latency = ctx->vo->opts->swapchain_depth,
        .adapter_name = p->opts->adapter_name,
    };
    if (!mp_d3d11_create_present_device(ctx->log, &dopts, &p->device))
        goto error;

    if (!spirv_compiler_init(ctx))
        goto error;
    ctx->ra = ra_d3d11_create(p->device, ctx->log, ctx->spirv);
    if (!ctx->ra)
        goto error;

    if (!vo_w32_init(ctx->vo))
        goto error;

    struct d3d11_swapchain_opts scopts = {
        .window = vo_w32_hwnd(ctx->vo),
        .width = ctx->vo->dwidth,
        .height = ctx->vo->dheight,
        .format = p->opts->output_format,
        .flip = p->opts->flip,
        // Add one frame for the backbuffer and one frame of "slack" to reduce
        // contention with the window manager when acquiring the backbuffer
        .length = ctx->vo->opts->swapchain_depth + 2,
        .usage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
    };
    if (!mp_d3d11_create_swapchain(p->device, ctx->log, &scopts, &p->swapchain))
        goto error;

    p->backbuffer = get_backbuffer(ctx);
    if (!p->backbuffer)
        goto error;

    return true;

error:
    d3d11_uninit(ctx);
    return false;
}

const struct ra_ctx_fns ra_ctx_d3d11 = {
    .type     = "d3d11",
    .name     = "d3d11",
    .reconfig = d3d11_reconfig,
    .control  = d3d11_control,
    .init     = d3d11_init,
    .uninit   = d3d11_uninit,
};
