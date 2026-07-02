#include "viewmodels/ServiceCardListModel.h"

#include <QUrl>

ServiceCardListModel::ServiceCardListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ServiceCardListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_cards.size());
}

int ServiceCardListModel::count() const
{
    return rowCount();
}

QVariant ServiceCardListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& card = m_cards[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return card.server.id;
    case NameRole:
        return card.server.name;
    case BaseUrlRole:
        return card.server.baseUrl;
    case HostRole:
        return QUrl(card.server.baseUrl).host();
    case UsernameRole:
        return card.server.username;
    case ServiceTypeRole:
        return serviceTypeToString(card.server.serviceType);
    case AutoLoginRole:
        return card.server.autoLogin;
    case HasSessionRole:
        return card.hasSession;
    case LastUsedAtRole:
        return card.lastUsedAt;
    case PrivateModeRole:
        return card.server.privateMode;
    default:
        return {};
    }
}

QHash<int, QByteArray> ServiceCardListModel::roleNames() const
{
    return {
        { IdRole, "cardId" },
        { NameRole, "name" },
        { BaseUrlRole, "baseUrl" },
        { HostRole, "host" },
        { UsernameRole, "username" },
        { ServiceTypeRole, "serviceType" },
        { AutoLoginRole, "autoLogin" },
        { HasSessionRole, "hasSession" },
        { LastUsedAtRole, "lastUsedAt" },
        { PrivateModeRole, "privateMode" },
    };
}

void ServiceCardListModel::setCards(std::vector<ServiceCard> cards)
{
    beginResetModel();
    m_cards = std::move(cards);
    endResetModel();
    emit countChanged();
}

void ServiceCardListModel::clear()
{
    setCards({});
}

std::optional<ServiceCard> ServiceCardListModel::cardAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_cards[static_cast<size_t>(row)];
}
