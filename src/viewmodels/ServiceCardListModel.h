#pragma once

#include "models/ServiceCard.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class ServiceCardListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

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
        LastUsedAtRole,
        PrivateModeRole
    };

    explicit ServiceCardListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setCards(std::vector<ServiceCard> cards);
    void clear();
    std::optional<ServiceCard> cardAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<ServiceCard> m_cards;
};
