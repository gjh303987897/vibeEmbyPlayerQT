#pragma once

#include "models/MediaItem.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class MediaItemListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ItemTypeRole,
        ProductionYearRole,
        SeriesIdRole,
        SeriesNameRole,
        SeriesImageUrlRole,
        ChildCountRole,
        ContinueImageUrlRole,
        OverviewRole,
        ImageUrlRole,
        BackdropImageUrlRole,
        CommunityRatingRole,
        OfficialRatingRole,
        RunTimeRole,
        GenresRole,
        PeopleRole,
        SeasonNameRole,
        IndexNumberRole,
        ParentIndexNumberRole,
        PlayedPercentageRole,
        PlayedRole,
        PlaybackPositionTicksRole
    };

    explicit MediaItemListModel(QObject* parent = nullptr);

    int count() const;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<MediaItem> items);
    void appendItems(std::vector<MediaItem> items);
    bool updatePlaybackProgress(const QString& itemId, qint64 playbackPositionTicks, double playedPercentage, bool played);
    void clear();
    std::optional<MediaItem> itemAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<MediaItem> m_items;
};
