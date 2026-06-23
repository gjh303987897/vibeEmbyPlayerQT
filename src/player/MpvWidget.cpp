#include "player/MpvWidget.h"

#include <QShowEvent>

MpvWidget::MpvWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setUpdatesEnabled(false);
}

PlayerController* MpvWidget::controller()
{
    return &m_controller;
}

void MpvWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_initialized) {
        m_initialized = m_controller.initialize(winId());
    }
}
