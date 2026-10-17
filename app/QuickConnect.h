#pragma once
#include <QComboBox>
#include <QToolBar>
#include <QWidget>

inline void sizeQuickConnect(QWidget* controls, QComboBox* address, QToolBar* toolbar) {
    controls->setMinimumWidth(0);
    controls->setMaximumWidth(480);
    controls->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    address->setMinimumWidth(180);
    address->setMaximumWidth(320);
    address->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    toolbar->setMaximumWidth(500);
    toolbar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}
