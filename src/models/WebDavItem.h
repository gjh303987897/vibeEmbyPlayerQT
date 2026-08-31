#pragma once

#include <QString>
#include <QUrl>

struct WebDavItem {
    QString name;
    QUrl url;
    QString relativePath;
    QString contentType;
    QString lastModified;
    qint64 size { -1 };
    bool directory { false };
    bool playable { false };
    bool audioPlayable { false };
    bool encryptedHls { false };
    QString identifierPreview;
    QString sourceFileName;
    // Whether the async M3U8S metadata read for this manifest has answered. Listing
    // rows know an encrypted package exists before its manifest has been fetched, so
    // this is what lets the view show a loading indicator instead of an empty row that
    // fills in later. A failed read counts as answered and leaves both fields empty.
    bool m3u8sMetadataResolved { false };
};
