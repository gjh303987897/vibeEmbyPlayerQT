#pragma once

#include "models/MediaLibrary.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class MediaLibraryListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CollectionTypeRole,
        ItemTypeRole,
        ImageUrlRole,
        ChildCountRole
    };

    explicit MediaLibraryListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setLibraries(std::vector<MediaLibrary> libraries);
    void clear();
    std::optional<MediaLibrary> libraryAt(int row) const;

private:
    std::vector<MediaLibrary> m_libraries;
};
