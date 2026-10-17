#pragma once
#include "PerformanceTrace.h"
#include "PerformanceMarker.h"
#include <SDL3/SDL.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <freerdp/codec/color.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>

// Test-only hooks for the exact pinned SDL source. No endpoint or input values enter the trace.
namespace sdlPerformance {
inline bool enabled() {
    const auto* value = std::getenv("RM_PERFORMANCE_WORKLOAD");
    return value && value[0] >= '0' && value[0] <= '5' && !value[1];
}
inline PerformanceTrace trace{enabled(), 1};
inline std::mutex mutex;
inline std::deque<FrameStamp> queue;
inline FrameStamp painted;
inline quint64 revision = 0, start = 0, completeRevision = 0, end = 0, inputAt = 0;
inline quint64 uploadBegin = 0, uploadBytes = 0;
inline std::vector<quint32> finalPixels;
inline quint32 width = 0, height = 0, codecs = 0;
inline pBitmapUpdate bitmapUpdate = nullptr;
inline pSurfaceBits surfaceBits = nullptr;
inline std::atomic_bool reportWritten{false}, stopRequested{false}, cleanAbort{false};
inline bool pixelsMatch = false, verificationReady = false;
inline Uint32 finalEvent = 0;
inline Uint32 SDLCALL finalTimer(void*, SDL_TimerID, Uint32) {
    SDL_Event event{}; event.type = finalEvent;
    if (!SDL_PushEvent(&event)) { std::printf("RM_COMPARATOR_VALIDATION_EVENT_FAILED=1\n"); std::fflush(stdout); }
    return 0;
}

inline void beforeAbort(rdpContext* context) {
    if (!enabled() || !reportWritten || !stopRequested || cleanAbort ||
        freerdp_get_last_error(context) != FREERDP_ERROR_SUCCESS || freerdp_error_info(context->instance) != 0)
        return;
    std::printf("RM_COMPARATOR_ABORT_CLEAN=1\n"); std::fflush(stdout);
    cleanAbort = true;
}
inline void cancelled(rdpContext* context) {
    if (enabled() && cleanAbort && freerdp_error_info(context->instance) == 0 &&
        freerdp_get_last_error(context) == FREERDP_ERROR_CONNECT_CANCELLED) {
        std::printf("RM_COMPARATOR_CANCELLED=1\n"); std::fflush(stdout);
    }
}

inline void observed(ReceivedCodec codec, quint32 w, quint32 h) {
    const auto bit = 1u << quint32(codec);
    if (codecs & bit) return;
    codecs |= bit;
    const auto now = PerformanceTrace::now();
    PerformanceSample sample{PerformanceStage::ReceivedCodec, {}, now, now, 0, w, h};
    sample.codec = codec; trace.record(sample);
}
inline BOOL bitmap(rdpContext* context, const BITMAP_UPDATE* update) {
    const BOOL ok = bitmapUpdate(context, update);
    if (ok) for (UINT32 i = 0; i < update->number; ++i) {
        const auto& r = update->rectangles[i];
        observed(!r.compressed ? ReceivedCodec::Uncompressed :
            r.bitsPerPixel >= 32 ? ReceivedCodec::Planar : ReceivedCodec::Interleaved, r.width, r.height);
    }
    return ok;
}
inline BOOL surface(rdpContext* context, const SURFACE_BITS_COMMAND* command) {
    const BOOL ok = surfaceBits(context, command);
    if (ok) {
        auto codec = ReceivedCodec::Unknown;
        switch (command->bmp.codecID) {
        case RDP_CODEC_ID_NONE: codec = ReceivedCodec::Uncompressed; break;
        case RDP_CODEC_ID_REMOTEFX: case RDP_CODEC_ID_IMAGE_REMOTEFX: codec = ReceivedCodec::RemoteFx; break;
        case RDP_CODEC_ID_NSCODEC: codec = ReceivedCodec::NsCodec; break;
        }
        observed(codec, command->bmp.width, command->bmp.height);
    }
    return ok;
}
inline bool prepare(rdpContext* context, Uint32 upstreamLastEvent) {
    if (!enabled()) return true;
    const auto* settings = context->settings;
    const auto* host = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
    const auto* user = freerdp_settings_get_string(settings, FreeRDP_Username);
    const auto* config = std::getenv("RM_COMPARATOR_CONFIG");
    if (!host || std::strcmp(host, "127.0.0.1") || !user || std::strcmp(user, "rdpfixture") || !config)
        return false;
    // Upstream uses fixed, unregistered user IDs. Reserve past them before choosing ours.
    if (upstreamLastEvent < SDL_EVENT_USER || upstreamLastEvent > SDL_EVENT_USER + 64) return false;
    const int count = int(upstreamLastEvent - SDL_EVENT_USER) + 2;
    const auto first = SDL_RegisterEvents(count);
    if (first == Uint32(-1)) return false;
    finalEvent = first + Uint32(count - 1);
    // Explicit config scope, not HOME mutation. Fingerprint trust remains FreeRDP's job.
    return freerdp_settings_set_string(context->settings, FreeRDP_ConfigPath, config) &&
        freerdp_settings_set_uint32(context->settings, FreeRDP_KeyboardLayout, 0x0409) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_DeviceRedirection, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_AudioPlayback, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_NetworkAutoDetect, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_SupportHeartbeatPdu, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_SupportMultitransport, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_SupportGraphicsPipeline, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_RemoteFxCodec, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_NSCodec, FALSE) &&
        freerdp_settings_set_bool(context->settings, FreeRDP_RedirectClipboard, FALSE);
}
inline bool install(rdpContext* context) {
    if (!enabled()) return true;
    bitmapUpdate = context->update->BitmapUpdate; surfaceBits = context->update->SurfaceBits;
    if (!bitmapUpdate || !surfaceBits) return false;
    context->update->BitmapUpdate = bitmap; context->update->SurfaceBits = surface;
    return true;
}
inline void push() {
    if (!enabled()) return;
    const auto now = PerformanceTrace::now();
    if (queue.size() >= PerformanceTrace::Capacity) throw std::runtime_error("SDL probe metadata backlog exceeded");
    queue.push_back({1, ++revision, now, now, 0});
    if (!start) start = now;
}
inline void pop() {
    if (!enabled() || queue.empty()) return;
    painted = queue.front(); queue.pop_front();
    const auto now = PerformanceTrace::now();
    trace.record({PerformanceStage::Mailbox, painted, painted.latestNs, now});
}
inline void capture(rdpGdi* gdi) {
    if (!enabled() || completeRevision || gdi->width != 1024 || gdi->height != 768) return;
    if (performanceMarker(gdi->primary_buffer, gdi->width, gdi->height, gdi->stride) != 436) return;
    // One final owned synthetic snapshot, outside the timed interval. Never a production payload.
    end = PerformanceTrace::now(); completeRevision = revision + 1;
    width = quint32(gdi->width); height = quint32(gdi->height);
    finalPixels.reserve(size_t(width) * height);
    for (quint32 y = 0; y < height; ++y) for (quint32 x = 0; x < width; ++x) {
        const auto* p = gdi->primary_buffer + y * gdi->stride + x * 4;
        finalPixels.push_back((quint32(p[2]) << 16) | (quint32(p[1]) << 8) | p[0]);
    }
    // One post-measurement event, never a full upload or continuous motion repaint.
    if (!SDL_AddTimer(200, finalTimer, nullptr)) throw std::runtime_error("SDL final validation timer failed");
}
inline FrameStamp stamp() {
    std::lock_guard<std::mutex> lock(mutex); return painted;
}
inline void beginUploads() {
    if (!enabled()) return;
    uploadBegin = PerformanceTrace::now(); uploadBytes = 0;
}
inline void uploaded(quint64 bytes) {
    if (enabled()) uploadBytes += bytes;
}
inline void endUploads() {
    if (!enabled() || !uploadBytes) return;
    // One span per dirty batch, including retained-target draw-command preparation.
    trace.record({PerformanceStage::UploadPreparation, stamp(), uploadBegin, PerformanceTrace::now(), uploadBytes});
    uploadBytes = 0;
}
inline quint32 reference(double x, double y, int channel, SDL_ScaleMode mode) {
    const auto pixel = [channel](int px, int py) {
        return double((finalPixels[size_t(std::clamp(py, 0, int(height) - 1)) * width +
            size_t(std::clamp(px, 0, int(width) - 1))] >> channel) & 255);
    };
    if (mode == SDL_SCALEMODE_NEAREST) return quint32(pixel(int(std::floor(x + .5)), int(std::floor(y + .5))));
    const int left = int(std::floor(x)), top = int(std::floor(y));
    const double fx = x - left, fy = y - top;
    return quint32(std::round((pixel(left, top) * (1 - fx) + pixel(left + 1, top) * fx) * (1 - fy) +
        (pixel(left, top + 1) * (1 - fx) + pixel(left + 1, top + 1) * fx) * fy));
}
inline void readback(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (!enabled() || !verificationReady || reportWritten || !completeRevision || painted.revision < completeRevision) return;
    auto* shot = SDL_RenderReadPixels(renderer, nullptr);
    if (!shot) throw std::runtime_error("SDL final readback failed");
    SDL_ScaleMode mode = SDL_SCALEMODE_INVALID;
    bool ok = SDL_GetTextureScaleMode(texture, &mode);
    if (mode != SDL_SCALEMODE_NEAREST && mode != SDL_SCALEMODE_LINEAR) ok = false;
    auto* converted = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_BGRA32);
    if (!converted) ok = false;
    int mismatchCount = 0, maxDifference = 0;
    if (converted) {
        for (int y = 32; y < int(height); y += 32) for (int x = 16; x < int(width); x += 32) {
            const int px = x * converted->w / int(width), py = y * converted->h / int(height);
            const auto* p = static_cast<const quint8*>(converted->pixels) + py * converted->pitch + px * 4;
            const double sx = (px + .5) * width / converted->w - .5;
            const double sy = (py + .5) * height / converted->h - .5;
            for (int c = 0; c < 3; ++c) {
                const int expected = int(reference(sx, sy, c * 8, mode));
                const int difference = std::abs(int(p[c]) - expected);
                if (difference > 1) {
                    ok = false; ++mismatchCount; maxDifference = std::max(maxDifference, difference);
                    if (mismatchCount == 1) std::printf("RM_COMPARATOR_PIXEL_FIRST=%d,%d,%d,%d,%d\n", px, py, c, int(p[c]), expected);
                }
            }
        }
        if (!ok) {
            int cpuLeft = -1, cpuRight = -1, gpuLeft = -1, gpuRight = -1;
            for (quint32 x = 0; x < width; ++x) if (finalPixels[size_t(64) * width + x] == 0xf08020) {
                if (cpuLeft < 0) cpuLeft = int(x); cpuRight = int(x);
            }
            for (int x = 0; x < converted->w; ++x) {
                const auto* p = static_cast<const quint8*>(converted->pixels) + (64 * converted->h / int(height)) * converted->pitch + x * 4;
                if (p[0] == 32 && p[1] == 128 && p[2] == 240) { if (gpuLeft < 0) gpuLeft = x; gpuRight = x; }
            }
            std::printf("RM_COMPARATOR_PIXEL_DIAGNOSTIC=%d,%d,%d,%d,%d\n", converted->w, converted->h, int(mode), mismatchCount, maxDifference);
            std::printf("RM_COMPARATOR_PIXEL_SPAN=%d,%d,%d,%d\n", cpuLeft, cpuRight, gpuLeft, gpuRight);
        }
        SDL_DestroySurface(converted);
    }
    if (!ok) std::fflush(stdout);
    SDL_DestroySurface(shot); pixelsMatch = ok;
}
inline void presented(SDL_Renderer* renderer, SDL_Window* window, quint64 begin) {
    if (!enabled()) return;
    const auto now = PerformanceTrace::now();
    trace.record({PerformanceStage::WindowSubmission, painted, begin, now});
    if (!verificationReady || !completeRevision || painted.revision < completeRevision || reportWritten) return;
    const auto* backend = SDL_GetRendererName(renderer);
    const auto* video = SDL_GetCurrentVideoDriver();
    if (!backend || !std::strcmp(backend, "software") || !video ||
        (!std::strcmp(video, "dummy") || !std::strcmp(video, "offscreen")) || !pixelsMatch)
        throw std::runtime_error("SDL native final pixel oracle failed");
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (!SDL_GetWindowSize(window, &ww, &wh) || !SDL_GetWindowSizeInPixels(window, &pw, &ph))
        throw std::runtime_error("SDL native geometry unavailable");
    QByteArray report = "{\"schema\":1,\"workload\":" + QByteArray(1, std::getenv("RM_PERFORMANCE_WORKLOAD")[0]) +
        ",\"width\":" + QByteArray::number(width) + ",\"height\":" + QByteArray::number(height) +
        ",\"dpr\":" + QByteArray::number(double(SDL_GetWindowPixelDensity(window))) +
        ",\"window_width\":" + QByteArray::number(ww) + ",\"window_height\":" + QByteArray::number(wh) +
        ",\"pixel_width\":" + QByteArray::number(pw) + ",\"pixel_height\":" + QByteArray::number(ph) +
        ",\"trace\":" + trace.summaryJson(start, end) + ",\"pixels\":[";
    bool first = true;
    for (quint32 y = 32; y < height; y += 32) for (quint32 x = 16; x < width; x += 32) {
        if (!first) report += ',';
        first = false; report += QByteArray::number(finalPixels[size_t(y) * width + x]);
    }
    report += "]}"; reportWritten = true;
    std::printf("RM_PERFORMANCE_RESULT=%s\n", report.constData()); std::fflush(stdout);
    SDL_Event quit{}; quit.type = SDL_EVENT_QUIT;
    stopRequested = true;
    std::printf("RM_COMPARATOR_STOP_REQUESTED=1\n"); std::fflush(stdout);
    if (!SDL_PushEvent(&quit)) throw std::runtime_error("SDL completion event rejected");
}
inline void input(SDL_Window* window) {
    if (!enabled() || std::getenv("RM_PERFORMANCE_WORKLOAD")[0] != '5') return;
    const auto now = PerformanceTrace::now();
    if (now - inputAt < 100000000) return;
    inputAt = now;
    SDL_Event event{}; event.key.windowID = SDL_GetWindowID(window);
    event.type = SDL_EVENT_KEY_DOWN; event.key.scancode = SDL_SCANCODE_A; event.key.key = SDLK_A; event.key.down = true;
    if (!SDL_PushEvent(&event)) throw std::runtime_error("SDL synthetic key rejected");
    event.type = SDL_EVENT_KEY_UP; event.key.down = false;
    if (!SDL_PushEvent(&event)) throw std::runtime_error("SDL synthetic key release rejected");
    event = {}; event.button.windowID = SDL_GetWindowID(window); event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 96; event.button.y = 80;
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.down = true;
    if (!SDL_PushEvent(&event)) throw std::runtime_error("SDL synthetic button rejected");
    event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.down = false;
    if (!SDL_PushEvent(&event)) throw std::runtime_error("SDL synthetic button release rejected");
}
}
