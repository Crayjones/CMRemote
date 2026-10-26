#include "Views.h"
#include <QCoreApplication>
#include <atomic>
#include <QFile>
#include <QEnterEvent>
#include <QPixmap>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include "PointerCapture.h"
#include <cmath>
#include <QKeyEvent>
#include <QLabel>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <memory>
#include <QPainter>
#include <QResizeEvent>
#include <QVector4D>
#include <QThread>
#include <QWheelEvent>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>

DesktopView::DesktopView(bool mock, QWidget* parent) : QRhiWidget(parent), mock_(mock) {
    setFocusPolicy(Qt::StrongFocus);
    // RDP sends physical scancodes; the remote layout composes text. Without an
    // input method macOS does not show the press-and-hold accent picker.
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setMouseTracking(true);
    setMinimumSize(200, 150);
    setObjectName("desktopView");
    placeholder_ = new QLabel(this);
    placeholder_->setAlignment(Qt::AlignCenter); placeholder_->setWordWrap(true);
    placeholder_->setAttribute(Qt::WA_TransparentForMouseEvents);
    placeholder_->setObjectName("desktopPlaceholder");
    placeholder_->setGeometry(rect());
    placeholder_->setText(placeholderText());
    placeholder_->setVisible(frame_.isNull());
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        rendererUnavailable("The window's graphics backend could not be initialized.");
    });
    connect(this, &QRhiWidget::frameSubmitted, this, [this] {
        if (!submissionPending_) return;
        submissionPending_ = false;
        if (submittedTrace_ && submittedStamp_.revision) {
            PerformanceSample sample{PerformanceStage::WindowSubmission, submittedStamp_, submittedAtNs_,
                PerformanceTrace::now(), 0, quint32(submittedSize_.width()), quint32(submittedSize_.height())};
            sample.resourceEpoch = submittedResourceEpoch_; submittedTrace_->record(sample);
        }
        submittedSize_ = {}; submittedTrace_.reset();
        presentedRevision_ = renderedRevision_;
        emit framePresented(presentedRevision_);
    });
}
DesktopView::~DesktopView() { if (capture_) capture_->stop(); releaseResources(); }
void DesktopView::paintEvent(QPaintEvent* event) {
    windowPaint_ = true;
    QRhiWidget::paintEvent(event);
    windowPaint_ = false;
}
void DesktopView::refreshPlaceholder() {
    placeholder_->setText(rendererError_.isEmpty() ? placeholderText() :
        "Renderer unavailable\n" + rendererError_ + "\nThe remote connection is unchanged. Retry on the next desktop update.");
    placeholder_->setVisible(frame_.isNull() || !rendererError_.isEmpty());
}
void DesktopView::rendererUnavailable(const QString& reason) {
    submissionPending_ = false;
    if (rendererError_ == reason) return;
    rendererError_ = reason; refreshPlaceholder();
    // Rendering/resource callers continue using this widget after this function.
    // Defer observers so a failure handler can safely retire/delete the view.
    QMetaObject::invokeMethod(this, [this, reason] { emit renderFailed(reason); }, Qt::QueuedConnection);
}
bool DesktopView::event(QEvent* event) {
    // Never leave the local pointer locked when this view stops receiving input.
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide || event->type() == QEvent::ApplicationDeactivate) setPointerCaptured(false);
    if (event->type() == QEvent::DevicePixelRatioChange) { geometryDirty_ = true; refreshRemoteCursor(true); update(); }
    if (event->type() == QEvent::Enter) {
        cursorPosition_ = static_cast<QEnterEvent*>(event)->position().toPoint(); cursorInside_ = true; refreshRemoteCursor();
    }
    if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) { cursorInside_ = false; refreshRemoteCursor(); }
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto* input = static_cast<QKeyEvent*>(event);
        if (input->key() == Qt::Key_Tab || input->key() == Qt::Key_Backtab) {
            if (event->type() == QEvent::KeyPress) keyPressEvent(input);
            else keyReleaseEvent(input);
            return true;
        }
    }
    return QRhiWidget::event(event);
}
QString DesktopView::placeholderText() const {
    return (mock_ ? QStringLiteral("MOCK GRAPHICAL SESSION\nNo remote connection is made.\n\n") :
        desktopLabel_ + QStringLiteral("\nWaiting for decoded frames.\n\n")) + report_;
}
void DesktopView::setPerformanceTrace(std::shared_ptr<PerformanceTrace> trace) {
    Q_ASSERT(QThread::currentThread() == thread());
    submissionPending_ = false; submittedTrace_.reset();
    performance_ = trace && trace->enabled() ? std::move(trace) : nullptr;
}
void DesktopView::setFrame(QImage frame) { setFrame(std::move(frame), {}); }
void DesktopView::setFrame(QImage frame, FrameStamp stamp) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto begin = performance_ ? PerformanceTrace::now() : 0;
    if (!stamp.revision) stamp = {0, frameRevision_ + 1, begin, begin, 0};
    if (uploadPending_ && frame.size() == frame_.size() && stamp.geometry == frameStamp_.geometry) {
        stamp.oldestNs = frameStamp_.oldestNs;
        stamp.replacements += frameStamp_.replacements + 1;
    }
    frame_ = frame.isNull() || frame.format() == QImage::Format_RGB32 ? std::move(frame) : frame.convertToFormat(QImage::Format_RGB32);
    frameStamp_ = frame_.isNull() ? FrameStamp{} : stamp;
    ++frameRevision_; uploadPending_ = fullUpload_ = !frame_.isNull(); geometryDirty_ = true;
    gpuDamage_ = {};
    refreshPlaceholder(); refreshRemoteCursor(true); update();
    if (performance_ && !frame_.isNull()) performance_->record({PerformanceStage::GuiApply, frameStamp_,
        begin, PerformanceTrace::now(), 0, quint32(frame_.width()), quint32(frame_.height())});
}
bool DesktopView::applyUpdate(const DesktopUpdate& desktop) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!desktop.stamp.geometry || !desktop.stamp.revision || desktop.stamp.geometry < frameStamp_.geometry ||
        (desktop.stamp.geometry == frameStamp_.geometry &&
            (desktop.stamp.revision <= frameStamp_.revision || desktop.size != frame_.size())) ||
        ((frame_.isNull() || desktop.stamp.geometry != frameStamp_.geometry) && !desktop.full)) return false;
    const auto begin = performance_ ? PerformanceTrace::now() : 0;
    const auto previousSize = frame_.size();
    if (!applyDesktopUpdate(frame_, desktop)) return false;
    auto stamp = desktop.stamp;
    if (uploadPending_ && previousSize == desktop.size && stamp.geometry == frameStamp_.geometry) {
        stamp.oldestNs = frameStamp_.oldestNs;
        stamp.replacements += frameStamp_.replacements + 1;
    }
    fullUpload_ |= desktop.full;
    if (!fullUpload_) {
        for (const auto& patch : desktop.patches) gpuDamage_ |= patch.rectangle;
        if (gpuDamage_.rectCount() > DesktopUpdateAccumulator::MaxRegions) { fullUpload_ = true; gpuDamage_ = {}; }
    }
    frameStamp_ = stamp; ++frameRevision_; uploadPending_ = true;
    geometryDirty_ |= previousSize != desktop.size;
    refreshPlaceholder(); if (previousSize != desktop.size) refreshRemoteCursor(true); update();
    if (performance_) performance_->record({PerformanceStage::GuiApply, frameStamp_, begin, PerformanceTrace::now(),
        0, quint32(frame_.width()), quint32(frame_.height())});
    return true;
}
void DesktopView::report(const QString& text) {
    report_ = text; refreshPlaceholder(); update();
}
void DesktopView::setRemoteCursor(RemoteCursor cursor) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!cursor.valid()) return;
    remoteCursor_ = std::move(cursor); refreshRemoteCursor(true);
}
void DesktopView::resetRemoteCursor() { setRemoteCursor({}); }
void DesktopView::refreshRemoteCursor(bool rebuild) {
    if (rebuild) {
        nativeCursor_ = QCursor(Qt::ArrowCursor);
        if (remoteCursor_.kind == RemoteCursor::Kind::Hidden) nativeCursor_ = QCursor(Qt::BlankCursor);
        else if (remoteCursor_.kind == RemoteCursor::Kind::Bitmap && !frame_.isNull()) {
            const auto scaled = scaleRemoteCursor(remoteCursor_, frame_.size(), presentationRect().size(), devicePixelRatioF());
            if (!scaled.image.isNull()) nativeCursor_ = QCursor(QPixmap::fromImage(scaled.image), scaled.hotspot.x(), scaled.hotspot.y());
        }
    }
    const bool remote = !frame_.isNull() && ((cursorInside_ && presentationRect().contains(cursorPosition_)) ||
        !heldButtons_.isEmpty() || pointerDragActive_);
    if (pointerCaptured()) setCursor(Qt::BlankCursor);
    else if (remote) setCursor(nativeCursor_); else unsetCursor();
}
QRect DesktopView::presentationRect() const {
    if (frame_.isNull()) return rect();
    QSize size = frame_.size(); size.scale(this->size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}
bool DesktopView::mapPointer(QPointF position, QPoint& remote, bool clamp) const {
    const auto target = presentationRect();
    if (frame_.isNull() || target.isEmpty() || (!clamp &&
        (position.x() < target.x() || position.y() < target.y() ||
         position.x() >= target.x() + target.width() || position.y() >= target.y() + target.height()))) return false;
    remote = QPoint(std::clamp(int((position.x() - target.x()) * frame_.width() / target.width()), 0, frame_.width() - 1),
        std::clamp(int((position.y() - target.y()) * frame_.height() / target.height()), 0, frame_.height() - 1));
    return true;
}
void DesktopView::trackPointer(QPointF position) {
    cursorPosition_ = position.toPoint(); cursorInside_ = rect().contains(cursorPosition_); refreshRemoteCursor();
}
void DesktopView::setPointerDragActive(bool active) { pointerDragActive_ = active; refreshRemoteCursor(); }
void DesktopView::setDesktopLabel(QString label) { desktopLabel_ = std::move(label); refreshPlaceholder(); }
std::array<float, 16> DesktopView::letterboxVertices(QSize widgetSize, QSize frameSize) {
    // TriangleStrip order: top-left, bottom-left, top-right, bottom-right. pos.xy then uv.xy.
    QSize scaled = frameSize; scaled.scale(widgetSize, Qt::KeepAspectRatio);
    const float left = float((widgetSize.width() - scaled.width()) / 2) / qMax(1, widgetSize.width());
    const float top = float((widgetSize.height() - scaled.height()) / 2) / qMax(1, widgetSize.height());
    const float right = left + float(scaled.width()) / qMax(1, widgetSize.width());
    const float bottom = top + float(scaled.height()) / qMax(1, widgetSize.height());
    const float x0 = 2 * left - 1, x1 = 2 * right - 1, y0 = 1 - 2 * top, y1 = 1 - 2 * bottom;
    return {x0, y0, 0, 0, x0, y1, 0, 1, x1, y0, 1, 0, x1, y1, 1, 1};
}
bool DesktopView::allowResourceCreation(const char*) const { return true; }
void DesktopView::initialize(QRhiCommandBuffer*) { ensureResources(); }
bool DesktopView::ensureResources() {
    if (!rhi() || !renderTarget()) { rendererUnavailable("No graphics render target is available."); return false; }
    const QSize desired = frame_.isNull() ? QSize(1, 1) : frame_.size();
    const int samples = renderTarget()->sampleCount();
    auto* targetPass = renderTarget()->renderPassDescriptor();
    if (resourceRhi_ == rhi() && pipeline_ && bindings_ && texture_ && sampler_ && vertexBuf_ &&
        textureSize_ == desired && targetSamples_ == samples && renderPass_ && renderPass_->isCompatible(targetPass)) {
        if (!rendererError_.isEmpty()) { rendererError_.clear(); refreshPlaceholder(); }
        return true;
    }
    auto failed = [this](const char* stage) {
        rendererUnavailable(QString("Graphics resource creation failed (%1).").arg(QString::fromLatin1(stage)));
        uploadPending_ = !frame_.isNull(); return false;
    };
    auto load = [](const char* path) {
        QFile file(QString::fromLatin1(path));
        return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
    };
    const QShader vert = load(":/rm/shaders/desktop.vert.qsb"), frag = load(":/rm/shaders/desktop.frag.qsb");
    if (!allowResourceCreation("shaders") || !vert.isValid() || !frag.isValid()) return failed("shaders");
    std::unique_ptr<QRhiSampler> sampler(rhi()->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!allowResourceCreation("sampler") || !sampler || !sampler->create()) return failed("sampler");
    std::unique_ptr<QRhiBuffer> vertex(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 16 * sizeof(float)));
    if (!allowResourceCreation("buffer") || !vertex || !vertex->create()) return failed("buffer");
    std::unique_ptr<QRhiTexture> texture(rhi()->newTexture(QRhiTexture::BGRA8, desired));
    if (!allowResourceCreation("texture") || !texture || !texture->create()) return failed("texture");
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi()->newShaderResourceBindings());
    if (!bindings) return failed("bindings");
    bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, texture.get(), sampler.get())});
    if (!allowResourceCreation("bindings") || !bindings->create()) return failed("bindings");
    std::unique_ptr<QRhiRenderPassDescriptor> pass(targetPass->newCompatibleRenderPassDescriptor());
    if (!pass) return failed("render pass");
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
    layout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                          QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi()->newGraphicsPipeline());
    if (!pipeline) return failed("pipeline");
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip); pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(false); pipeline->setDepthWrite(false); pipeline->setVertexInputLayout(layout);
    pipeline->setShaderStages({QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
    pipeline->setShaderResourceBindings(bindings.get()); pipeline->setRenderPassDescriptor(pass.get());
    pipeline->setSampleCount(samples);
    if (!allowResourceCreation("pipeline") || !pipeline->create()) return failed("pipeline");
    // Commit a complete cache only. The old frame's GPU work may still be in flight.
    clearResources(true);
    sampler_ = sampler.release(); vertexBuf_ = vertex.release(); texture_ = texture.release();
    bindings_ = bindings.release(); pipeline_ = pipeline.release(); renderPass_ = pass.release();
    resourceRhi_ = rhi(); textureSize_ = desired; targetSamples_ = samples; ++resourceEpoch_;
    uploadPending_ = fullUpload_ = !frame_.isNull(); gpuDamage_ = {}; geometryDirty_ = true;
    rendererError_.clear(); refreshPlaceholder(); return true;
}
void DesktopView::render(QRhiCommandBuffer* cb) {
    // A grab can overwrite the backing texture before composition. Invalidate any
    // earlier pending window-paint evidence rather than attributing that grab to it.
    if (!windowPaint_) submissionPending_ = false;
    if (!rhi() || !renderTarget()) return;
    const bool ready = ensureResources();
    const bool drawable = ready && !frame_.isNull();
    QRhiResourceUpdateBatch* batch = nullptr;
    if (drawable && (uploadPending_ || geometryDirty_)) {
        auto vertices = letterboxVertices(size(), frame_.size());
        const QMatrix4x4 corr = rhi()->clipSpaceCorrMatrix();
        for (int i = 0; i < 4; ++i) {
            const QVector4D p = corr * QVector4D(vertices[i * 4], vertices[i * 4 + 1], 0.0f, 1.0f);
            vertices[i * 4] = p.x(); vertices[i * 4 + 1] = p.y();
        }
        batch = rhi()->nextResourceUpdateBatch();
        if (uploadPending_) {
            const auto begin = performance_ ? PerformanceTrace::now() : 0;
            std::vector<QRhiTextureUploadEntry> entries;
            quint64 uploaded = 0;
            if (fullUpload_) {
                // Own the padded BGRA bytes without retaining a shared CPU desktop.
                QRhiTextureSubresourceUploadDescription sub(frame_.constBits(), quint32(frame_.sizeInBytes()));
                sub.setDataStride(quint32(frame_.bytesPerLine()));
                entries.emplace_back(0, 0, sub); uploaded = quint64(frame_.sizeInBytes());
            } else {
                for (const auto& rectangle : gpuDamage_) {
                    auto pixels = frame_.copy(rectangle);
                    if (pixels.isNull()) {
                        batch->release(); rendererUnavailable("Desktop upload allocation failed."); return;
                    }
                    // Each compact patch is owned by QRhi's upload description.
                    // No upload borrows mutable GDI or the authoritative CPU image.
                    QRhiTextureSubresourceUploadDescription sub(pixels);
                    sub.setDestinationTopLeft(rectangle.topLeft()); sub.setSourceSize(rectangle.size());
                    entries.emplace_back(0, 0, sub); uploaded += quint64(pixels.sizeInBytes());
                }
            }
            QRhiTextureUploadDescription upload; upload.setEntries(entries.cbegin(), entries.cend());
            batch->uploadTexture(texture_, upload);
            if (performance_ && windowPaint_) performance_->record({PerformanceStage::UploadPreparation, frameStamp_,
                begin, PerformanceTrace::now(), uploaded, quint32(frame_.width()), quint32(frame_.height())});
            gpuDamage_ = {}; fullUpload_ = false;
        }
        batch->updateDynamicBuffer(vertexBuf_, 0, quint32(vertices.size() * sizeof(float)), vertices.data());
        uploadPending_ = false; geometryDirty_ = false;
    }
    // Letterbox bars around a remote desktop stay black, like other RDP clients.
    cb->beginPass(renderTarget(), drawable ? QColor(Qt::black) : palette().color(QPalette::Base), {1.0f, 0}, batch);
    if (drawable) {
        cb->setGraphicsPipeline(pipeline_);
        const QSize pixels = renderTarget()->pixelSize();
        cb->setViewport(QRhiViewport(0, 0, float(pixels.width()), float(pixels.height())));
        cb->setShaderResources(bindings_);
        const QRhiCommandBuffer::VertexInput input(vertexBuf_, 0);
        cb->setVertexInput(0, 1, &input); cb->draw(4);
    }
    cb->endPass();
    // A framebuffer grab invokes render too, but is not evidence of window presentation.
    if (windowPaint_ && ready) {
        submittedSize_ = frame_.size(); renderedRevision_ = frameRevision_; submissionPending_ = true;
        submittedStamp_ = frameStamp_; submittedTrace_ = performance_;
        submittedAtNs_ = performance_ ? PerformanceTrace::now() : 0;
        submittedResourceEpoch_ = resourceEpoch_;
    }
}
void DesktopView::clearResources(bool deferred) {
    auto destroy = [deferred](auto*& resource) {
        if (resource && deferred) resource->deleteLater(); else delete resource;
        resource = nullptr;
    };
    destroy(pipeline_); destroy(bindings_); destroy(vertexBuf_); destroy(sampler_); destroy(texture_); destroy(renderPass_);
    resourceRhi_ = nullptr; textureSize_ = {}; targetSamples_ = 0;
}
void DesktopView::releaseResources() {
    clearResources(false);
    uploadPending_ = fullUpload_ = !frame_.isNull(); gpuDamage_ = {}; geometryDirty_ = true;
    submissionPending_ = false; submittedSize_ = {}; presentedRevision_ = 0;
}
void DesktopView::resizeEvent(QResizeEvent* e) {
    QRhiWidget::resizeEvent(e); placeholder_->setGeometry(rect()); geometryDirty_ = true;
    refreshRemoteCursor(true); update(); emit viewportChanged(e->size());
}
quint32 DesktopView::scanCode(int key) {
    static const quint32 letters[] = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return letters[key - Qt::Key_A];
    if (key >= Qt::Key_1 && key <= Qt::Key_9) return 0x02 + key - Qt::Key_1;
    if (key >= Qt::Key_F1 && key <= Qt::Key_F10) return 0x3b + key - Qt::Key_F1;
    switch (key) {
    case Qt::Key_0:return 0x0b; case Qt::Key_Escape:return 0x01; case Qt::Key_Backspace:return 0x0e;
    case Qt::Key_Tab:case Qt::Key_Backtab:return 0x0f; case Qt::Key_Return:return 0x1c; case Qt::Key_Enter:return 0x11c;
    case Qt::Key_Control:return 0x1d; case Qt::Key_Shift:return 0x2a; case Qt::Key_Alt:return 0x38;
    case Qt::Key_AltGr:return 0x138; case Qt::Key_Meta:return 0x15b; case Qt::Key_Space:return 0x39;
    case Qt::Key_CapsLock:return 0x3a; case Qt::Key_F11:return 0x57; case Qt::Key_F12:return 0x58;
    case Qt::Key_Home:return 0x147; case Qt::Key_Up:return 0x148; case Qt::Key_PageUp:return 0x149;
    case Qt::Key_Left:return 0x14b; case Qt::Key_Right:return 0x14d; case Qt::Key_End:return 0x14f;
    case Qt::Key_Down:return 0x150; case Qt::Key_PageDown:return 0x151; case Qt::Key_Insert:return 0x152;
    case Qt::Key_Delete:return 0x153; case Qt::Key_Minus:return 0x0c; case Qt::Key_Equal:return 0x0d;
    case Qt::Key_BracketLeft:return 0x1a; case Qt::Key_BracketRight:return 0x1b; case Qt::Key_Backslash:return 0x2b;
    case Qt::Key_Semicolon:return 0x27; case Qt::Key_Apostrophe:return 0x28; case Qt::Key_QuoteLeft:return 0x29;
    case Qt::Key_Comma:return 0x33; case Qt::Key_Period:return 0x34; case Qt::Key_Slash:return 0x35;
    default:return 0;
    }
}
namespace { std::atomic<bool> commandSendsControl{false}; }
void DesktopView::setCommandAsControl(bool enabled) { commandSendsControl = enabled; }
bool DesktopView::commandAsControl() { return commandSendsControl; }
QString DesktopView::captureChordText() {
#ifdef Q_OS_MACOS
    return QStringLiteral("Control+Option");
#else
    return QStringLiteral("Ctrl+Alt");
#endif
}
void DesktopView::setPointerCaptureAllowed(bool allowed) { captureAllowed_ = allowed; if (!allowed) setPointerCaptured(false); }
bool DesktopView::pointerCaptured() const { return capture_ && capture_->active(); }
void DesktopView::setPointerCaptured(bool captured) {
    if (captured == pointerCaptured()) return;
    if (captured) {
        if (!captureAllowed_ || frame_.isNull() || !isVisible() || !window()->isActiveWindow()) return;
        if (!capture_) capture_ = std::make_unique<PointerCapture>();
        fractionX_ = fractionY_ = 0;
        if (!capture_->start(this, [this](double dx, double dy) { captureMotion(dx, dy); })) return;
        setFocus();
    } else capture_->stop();
    refreshRemoteCursor(); emit pointerCaptureChanged(captured);
}
void DesktopView::setRemotePointerPosition(QPoint position) {
    if (frame_.isNull()) return;
    lastPointer_ = QPoint(std::clamp(position.x(), 0, frame_.width() - 1), std::clamp(position.y(), 0, frame_.height() - 1));
}
void DesktopView::captureMotion(double dx, double dy) {
    if (!pointerCaptured() || frame_.isNull()) return;
    // Local points → remote pixels at the current presentation scale.
    const auto target = presentationRect(); if (target.isEmpty()) return;
    fractionX_ += dx * frame_.width() / target.width(); fractionY_ += dy * frame_.height() / target.height();
    const int rx = int(std::trunc(fractionX_)), ry = int(std::trunc(fractionY_));
    if (!rx && !ry) return;
    fractionX_ -= rx; fractionY_ -= ry;
    if (relativeSupported_) { emit relativePointerInput(0x0800, qint16(std::clamp(rx, -32768, 32767)), qint16(std::clamp(ry, -32768, 32767))); return; }
    // Absolute fallback: a virtual pointer that follows server re-centring.
    lastPointer_ = QPoint(std::clamp(lastPointer_.x() + rx, 0, frame_.width() - 1), std::clamp(lastPointer_.y() + ry, 0, frame_.height() - 1));
    emit pointerInput(0x0800, quint16(lastPointer_.x()), quint16(lastPointer_.y()));
}
bool DesktopView::captureChord(QKeyEvent* e, bool down) {
#ifdef Q_OS_MACOS
    const int first = Qt::Key_Meta; // physical Control
#else
    const int first = Qt::Key_Control;
#endif
    if (!captureAllowed_ || e->isAutoRepeat()) return false;
    const bool chordKey = e->key() == first || e->key() == Qt::Key_Alt;
    if (!chordKey) { if (down) chordClean_ = false; return false; }
    if (down) {
        if (chordDown_.isEmpty()) { chordClean_ = true; chordArmed_ = false; }
        chordDown_.insert(e->key()); if (chordDown_.size() == 2) chordArmed_ = true;
        return false;
    }
    chordDown_.remove(e->key());
    if (!chordDown_.isEmpty() || !chordArmed_) return false;
    chordArmed_ = false;
    if (!chordClean_) return false;
    setPointerCaptured(!pointerCaptured()); return true;
}
void DesktopView::key(QKeyEvent* e, bool down) {
    captureChord(e, down);
    if (mock_) emit mockInput(QString("Key %1 %2").arg(e->key()).arg(down ? "down" : "up"));
    else {
        auto scan = scanCode(e->key());
#ifdef Q_OS_MACOS
        // Qt reports Command as Key_Control and physical Control as Key_Meta. By
        // default each sends the key it is labeled with: Control → Ctrl, Command → Windows.
        if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) && !commandSendsControl) {
            if (e->key() == Qt::Key_Control) scan = 0x15b; else if (e->key() == Qt::Key_Meta) scan = 0x1d;
        }
#endif
        if (scan) {
            if (e->isAutoRepeat() && !down) { e->accept(); return; }
            if (down) held_.insert(scan); else held_.remove(scan);
            emit keyInput(scan, down, e->isAutoRepeat());
        } else if (down && !e->text().isEmpty()) emit textInput(e->text());
    }
    e->accept();
}
void DesktopView::keyPressEvent(QKeyEvent* e) { key(e, true); }
void DesktopView::keyReleaseEvent(QKeyEvent* e) { key(e, false); }
void DesktopView::inputMethodEvent(QInputMethodEvent* e) { if (!mock_) emit textInput(e->commitString()); e->accept(); }
void DesktopView::focusOutEvent(QFocusEvent* e) {
    for (auto scan : held_) emit keyInput(scan, false, false);
    held_.clear();
    for (auto flag : heldButtons_) emit pointerInput(flag, quint16(lastPointer_.x()), quint16(lastPointer_.y()));
    heldButtons_.clear(); chordDown_.clear(); chordArmed_ = false; setPointerCaptured(false); refreshRemoteCursor(); QWidget::focusOutEvent(e);
}
void DesktopView::pointer(QMouseEvent* e, bool down, bool button) {
    cursorPosition_ = e->position().toPoint(); cursorInside_ = rect().contains(cursorPosition_); refreshRemoteCursor();
    if (mock_) { emit mockInput(QString("Pointer %1,%2").arg(int(e->position().x())).arg(int(e->position().y()))); return; }
    if (pointerCaptured()) {
        // Captured: motion comes from raw deltas; buttons act at the remote pointer.
        if (!button) { capture_->moved(e->position().toPoint()); return; }
        if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton && e->button() != Qt::MiddleButton) return;
        quint16 flags = e->button() == Qt::LeftButton ? 0x1000 : e->button() == Qt::RightButton ? 0x2000 : 0x4000;
        if (down) { heldButtons_.insert(flags); flags |= 0x8000; } else heldButtons_.remove(flags);
        if (relativeSupported_) emit relativePointerInput(flags, 0, 0);
        else emit pointerInput(flags, quint16(lastPointer_.x()), quint16(lastPointer_.y()));
        return;
    }
    const QRect r = presentationRect();
    if (frame_.isNull() || (!r.contains(e->position().toPoint()) && heldButtons_.isEmpty())) return;
    if (button && e->button() != Qt::LeftButton && e->button() != Qt::RightButton && e->button() != Qt::MiddleButton) return;
    const auto x = quint16(std::clamp(int((e->position().x() - r.x()) * frame_.width() / r.width()), 0, frame_.width() - 1));
    const auto y = quint16(std::clamp(int((e->position().y() - r.y()) * frame_.height() / r.height()), 0, frame_.height() - 1));
    quint16 flags = 0x0800; // PTR_FLAGS_MOVE
    if (button) {
        flags = e->button() == Qt::LeftButton ? 0x1000 : e->button() == Qt::RightButton ? 0x2000 : 0x4000;
        if (down) { heldButtons_.insert(flags); flags |= 0x8000; }
        else heldButtons_.remove(flags);
    }
    lastPointer_ = QPoint(x, y); refreshRemoteCursor();
    emit pointerInput(flags, x, y);
}
void DesktopView::mouseMoveEvent(QMouseEvent* e) { pointer(e, false, false); }
void DesktopView::mousePressEvent(QMouseEvent* e) { setFocus(); pointer(e, true, true); }
void DesktopView::mouseReleaseEvent(QMouseEvent* e) { pointer(e, false, true); }
void DesktopView::wheelEvent(QWheelEvent* e) {
    if (mock_) emit mockInput(QString("Wheel %1").arg(e->angleDelta().y()));
    else if (const int delta = std::clamp(e->angleDelta().y(), -255, 255)) {
        // Zero deltas (trackpad phase/momentum ends, horizontal-only motion) must not be sent:
        // xrdp ignores the rotation value and treats any non-negative wheel PDU as scroll up.
        emit pointerInput(quint16(0x0200 | (delta < 0 ? 0x0100 : 0) | (quint16(delta) & 0x00ff)), quint16(lastPointer_.x()), quint16(lastPointer_.y()));
    }
    e->accept();
}
