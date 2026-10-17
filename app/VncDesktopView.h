#pragma once
#include "Views.h"
#include <QMap>
#include <QVector>

// RFB input specialization only. DesktopView owns CPU/GPU presentation and cursor.
class VncDesktopView final : public DesktopView {
    Q_OBJECT
public:
    explicit VncDesktopView(bool mock, QWidget* parent = nullptr);
    ~VncDesktopView() override;
    void setInputEnabled(bool enabled);
    void releaseInput();
signals:
    void vncKeyInput(quint32 keysym, bool down);
    void vncPointerInput(quint8 mask, quint16 x, quint16 y);
    void inputError(QString error);
protected:
    bool event(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    static quint64 identity(const QKeyEvent& event);
    static quint32 namedKeysym(const QKeyEvent& event);
    static bool textKeysyms(const QString& text, QVector<quint32>& symbols);
    void key(QKeyEvent* event, bool down);
    void pointer(QMouseEvent* event, bool down, bool button);
    void sendPointer(quint8 mask);
    void wheelAxis(qint64& remainder, int delta, int unit, quint8 positive, quint8 negative);
    const bool mock_;
    bool inputEnabled_ = false, composing_ = false;
    QMap<quint64, QVector<quint32>> heldKeys_;
    QMap<quint32, int> symbolHolds_;
    quint64 inputEpoch_ = 0;
    quint8 heldButtons_ = 0;
    QPoint lastPointer_;
    qint64 angleX_ = 0, angleY_ = 0, pixelX_ = 0, pixelY_ = 0;
};
