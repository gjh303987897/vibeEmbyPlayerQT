#pragma once

#include "models/ServiceCard.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class ServiceCardListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        BaseUrlRole,
        HostRole,
        UsernameRole,
        ServiceTypeRole,
        AutoLoginRole,
        HasSessionRole,
        LastUsedAtRole
    };

    explicit ServiceCardListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setCards(std::vector<ServiceCard> cards);
    void clear();
    std::optional<ServiceCard> cardAt(int row) const;

private:
    std::vector<ServiceCard> m_cards;
};
