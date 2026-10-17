#pragma once
#include <QImage>
#include <QSet>
#include <QRhiWidget>
#include <QWidget>
#include <array>
#include <memory>
#include <functional>
#include "PerformanceTrace.h"
#include "DesktopUpdate.h"
#include "RemoteCursor.h"
#include <QCursor>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiRenderPassDescriptor;
class QLabel;
class PointerCapture;

class DesktopView : public QRhiWidget {
    Q_OBJECT
public:
    explicit DesktopView(bool mock, QWidget* parent = nullptr);
    ~DesktopView() override;
    void setFrame(QImage frame);
    void setFrame(QImage frame, FrameStamp stamp);
    bool applyUpdate(const DesktopUpdate& update);
    void setPerformanceTrace(std::shared_ptr<PerformanceTrace> trace);
    void report(const QString& text);
    void setRemoteCursor(RemoteCursor cursor);
    void resetRemoteCursor();
    QImage frame() const { return frame_; }
    quint64 frameRevision() const { return frameRevision_; }
    quint64 presentedRevision() const { return presentedRevision_; }
    bool rendererAvailable() const { return rendererError_.isEmpty(); }
    static quint32 scanCode(int qtKey);
    // Mouse capture (relative mouse for games). Toggle chord: Control+Option on
    // macOS, Ctrl+Alt elsewhere, pressed and released without another key.
    void setPointerCaptureAllowed(bool allowed);
    bool pointerCaptureAllowed() const { return captureAllowed_; }
    void setPointerCaptured(bool captured);
    bool pointerCaptured() const;
    // Server advertised relative mouse input (INPUT_FLAG_MOUSE_RELATIVE).
    void setRelativePointerSupported(bool supported) { relativeSupported_ = supported; }
    // Server-initiated cursor moves (applications re-centring the pointer).
    void setRemotePointerPosition(QPoint position);
    static QString captureChordText();
    static std::array<float, 16> letterboxVertices(QSize widgetSize, QSize frameSize);
signals:
    void keyInput(quint32 scan, bool down, bool repeat);
    void textInput(QString text);
    void pointerInput(quint16 flags, quint16 x, quint16 y);
    void relativePointerInput(quint16 flags, qint16 dx, qint16 dy);
    void pointerCaptureChanged(bool captured);
    void viewportChanged(QSize size);
    void mockInput(QString description);
    void framePresented(quint64 revision);
    void renderFailed(QString reason);
protected:
    void paintEvent(QPaintEvent* event) override;
    // A narrow fault-injection seam for the complete GPU readiness contract.
    virtual bool allowResourceCreation(const char* resource) const;
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    // Shared fit mapping/cursor presentation, not protocol input state.
    QRect presentationRect() const;
    bool mapPointer(QPointF position, QPoint& remote, bool clamp = false) const;
    void trackPointer(QPointF position);
    void setPointerDragActive(bool active);
    void setDesktopLabel(QString label);
private:
    void key(QKeyEvent* event, bool down);
    bool captureChord(QKeyEvent* event, bool down);
    void captureMotion(double dx, double dy);
    void pointer(QMouseEvent* event, bool down, bool button);
    QString placeholderText() const;
    bool mock_;
    QString report_, desktopLabel_ = QStringLiteral("RDP desktop");
    QImage frame_;
    QSet<quint32> held_;
    QSet<quint16> heldButtons_;
    QPoint lastPointer_, cursorPosition_;
    RemoteCursor remoteCursor_;
    QCursor nativeCursor_;
    bool cursorInside_ = false, pointerDragActive_ = false;
    std::unique_ptr<PointerCapture> capture_;
    bool captureAllowed_ = false, relativeSupported_ = false, chordArmed_ = false, chordClean_ = false;
    QSet<int> chordDown_;
    double fractionX_ = 0, fractionY_ = 0;
    void refreshRemoteCursor(bool rebuild = false);
    QLabel* placeholder_ = nullptr;
    bool ensureResources();
    void clearResources(bool deferred);
    void rendererUnavailable(const QString& reason);
    void refreshPlaceholder();
    // GUI-thread presentation; frame_ remains authoritative across GPU recreation.
    QSize submittedSize_;
    QRegion gpuDamage_;
    bool fullUpload_ = false;
    std::shared_ptr<PerformanceTrace> performance_, submittedTrace_;
    FrameStamp frameStamp_, submittedStamp_;
    quint64 submittedAtNs_ = 0, resourceEpoch_ = 0, submittedResourceEpoch_ = 0;
    QString rendererError_;
    bool uploadPending_ = false, geometryDirty_ = true, windowPaint_ = false, submissionPending_ = false;
    quint64 frameRevision_ = 0, renderedRevision_ = 0, presentedRevision_ = 0;
    QRhi* resourceRhi_ = nullptr;
    QRhiRenderPassDescriptor* renderPass_ = nullptr;
    int targetSamples_ = 0;
    QRhiTexture* texture_ = nullptr;
    QRhiSampler* sampler_ = nullptr;
    QRhiBuffer* vertexBuf_ = nullptr;
    QRhiShaderResourceBindings* bindings_ = nullptr;
    QRhiGraphicsPipeline* pipeline_ = nullptr;
    QSize textureSize_;
};

#include "TerminalAppearance.h"
class TerminalScreen;
class ClipboardEndpoint;
class QScrollBar;
class QTimer;
class QContextMenuEvent;
// Explicit mocks retain their report surface; real SSH uses GUI-owned libvterm.
class TerminalView : public QWidget {
    Q_OBJECT
public:
    explicit TerminalView(QWidget* parent = nullptr);
    void report(const QString& text);
    void initializeTerminal(std::shared_ptr<ClipboardEndpoint> clipboard = {});
    bool setAppearance(const TerminalAppearance& appearance);
    const TerminalAppearance& appearance() const { return appearance_; }
    TerminalScreen* screen() const { return screen_; }
    bool append(const QByteArray& bytes);
    void setInputEnabled(bool enabled);
    void setSessionShortcutsEnabled(bool enabled) { sessionShortcuts_ = enabled; }
    void setInputSink(std::function<bool(const QByteArray&)> sink) { inputSink_ = std::move(sink); }
    // Typed input from outside the view (View > Multi SSH toolbar).
    bool sendText(const QString& text) { return send(text.toUtf8()); }
    void selectCells(QPoint start, QPoint end);
    bool copySelection();
    bool pasteClipboard();
signals:
    void mockInput(QString description);
    void viewportChanged(QSize size);
    void cellsChanged(quint16 rows, quint16 columns);
    void inputError(QString error);
    void appearanceRequested();
protected:
    bool event(QEvent*) override;
    void changeEvent(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    enum class Shortcut { None, Copy, Paste, SelectAll, PageUp, PageDown, First, Last, ZoomIn, ZoomOut, ZoomReset };
    Shortcut localShortcut(const QKeyEvent& event) const;
    void runShortcut(Shortcut shortcut);
    void applyAppearance();
    void updateCells();
    void refreshScreen();
    bool send(const QByteArray& bytes);
    void pumpPaste();
    struct SelectionCell {
        int column = 0;
        quint64 line = 0;
        bool operator<(const SelectionCell& other) const { return line < other.line || (line == other.line && column < other.column); }
    };
    QPoint cellAt(QPoint position) const;
    SelectionCell selectionCell(QPoint cell) const;
    quint64 lineOrigin() const;
    void updateDrag();
    int dragScrollStep() const;
    void stopDrag();
    QString selectionText() const;
    bool selected(int line, int column) const;
    QString report_;
    TerminalScreen* screen_ = nullptr;
    QScrollBar* scroll_ = nullptr;
    QTimer* pasteTimer_ = nullptr;
    QTimer* blinkTimer_ = nullptr;
    bool cursorOn_ = true;
    TerminalAppearance appearance_, baseAppearance_;
    int zoomOffset_ = 0;
    bool sessionShortcuts_ = true;
    void updateBlink();
    std::shared_ptr<ClipboardEndpoint> endpoint_;
    std::function<bool(const QByteArray&)> inputSink_;
    QSize cellSize_;
    SelectionCell selectionStart_, selectionEnd_;
    QPoint dragPosition_;
    QTimer* selectionTimer_ = nullptr;
    quint64 lineOrigin_ = 0, historyEnd_ = 0, contentRevision_ = 0, selectionEpoch_ = 0;
    qint64 pixelRemainder_ = 0, angleRemainder_ = 0;
    bool inputEnabled_ = false, selection_ = false, dragging_ = false;
    int pasteOffset_ = 0;
    QByteArray pendingPaste_;
};
