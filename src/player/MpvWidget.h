#pragma once

#include "player/PlayerController.h"

#include <QWidget>

class MpvWidget final : public QWidget {
    Q_OBJECT

public:
    explicit MpvWidget(QWidget* parent = nullptr);

    PlayerController* controller();

protected:
    void showEvent(QShowEvent* event) override;

private:
    PlayerController m_controller;
    bool m_initialized { false };
};
