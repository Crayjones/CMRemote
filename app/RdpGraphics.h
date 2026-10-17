#pragma once
#include "PerformanceTrace.h"
#include <QRegion>
#include "RdpGraphicsPolicy.h"
#include <freerdp/freerdp.h>
#include <freerdp/client/rdpgfx.h>
#include <map>

bool rdpDesktopSizeValid(quint32 width, quint32 height);
QRegion rdpDesktopDamage(const rdpGdi* gdi);
bool resizeRdpDesktop(rdpContext* context, quint64& geometry);
bool rdpSoftwareAvcAvailable();
ReceivedCodec rdpGfxReceivedCodec(quint16 codecId);
bool applyRdpGraphicsPolicy(rdpSettings* settings, RdpGraphicsPolicy policy, bool softwareAvc);

class RdpGfxPipeline;
// The worker's custom context and unit fixture share this explicit prefix.
struct RdpGraphicsContext {
    rdpContext base;
    RdpGfxPipeline* pipeline;
};

class RdpGfxPipeline final {
public:
    struct Hooks {
        void* user = nullptr;
        bool (*publish)(void*, const QRegion&) = nullptr;
        void (*codec)(void*, ReceivedCodec, quint32, quint32) = nullptr;
    };
    struct Operations {
        BOOL (*init)(rdpGdi*, RdpgfxClientContext*);
        void (*uninit)(rdpGdi*, RdpgfxClientContext*);
    };
    static Operations systemOperations();
    explicit RdpGfxPipeline(Operations operations = systemOperations());
    ~RdpGfxPipeline();
    RdpGfxPipeline(const RdpGfxPipeline&) = delete;
    RdpGfxPipeline& operator=(const RdpGfxPipeline&) = delete;
    bool initialize(RdpGraphicsContext* context, RdpgfxClientContext* gfx, Hooks hooks);
    UINT close();
    bool active() const { return gfx_ != nullptr; }
    bool collectPaint();
private:
    static RdpGfxPipeline* from(RdpgfxClientContext* gfx);
    static UINT startFrame(RdpgfxClientContext*, const RDPGFX_START_FRAME_PDU*);
    static UINT endFrame(RdpgfxClientContext*, const RDPGFX_END_FRAME_PDU*);
    static UINT updateSurfaces(RdpgfxClientContext*);
    static UINT surfaceCommand(RdpgfxClientContext*, const RDPGFX_SURFACE_COMMAND*);
    static UINT createSurface(RdpgfxClientContext*, const RDPGFX_CREATE_SURFACE_PDU*);
    static UINT deleteSurface(RdpgfxClientContext*, const RDPGFX_DELETE_SURFACE_PDU*);
    static UINT resetGraphics(RdpgfxClientContext*, const RDPGFX_RESET_GRAPHICS_PDU*);
    bool publish();
    void restore();
    Operations operations_;
    RdpGraphicsContext* context_ = nullptr;
    RdpgfxClientContext* gfx_ = nullptr;
    RdpgfxClientContext previous_{}, installed_{};
    Hooks hooks_;
    bool collecting_ = false, pending_ = false, inFrame_ = false, ending_ = false;
    quint32 frameId_ = 0;
    QRegion damage_;
    std::map<quint16, quint64> surfacePixels_;
};
