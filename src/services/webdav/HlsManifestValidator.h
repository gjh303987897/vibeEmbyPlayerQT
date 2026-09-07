#pragma once

#include <QByteArrayView>
#include <QString>

#include <expected>

namespace HlsManifestValidator {

inline constexpr qsizetype m3u8sIdentifierLength = 4096;
inline constexpr qsizetype maximumEncryptedSourceNameBytes = 16 + 4096 + 16;

// M3U8S metadata carried by a manifest-head window. The encrypted source
// name is empty when the manifest predates v3 packaging (no source-name line).
struct HeadMetadata final {
    QByteArray identifier;
    QByteArray encryptedSourceFileName;
};

std::expected<void, QString> validate(QByteArrayView manifest,
                                      const QString& manifestPath = {});
// Parses only the M3U8S metadata tags from the head of a manifest without
// validating the whole playlist. The packager always writes both tag lines
// directly after #EXTM3U (the identifier is a fixed 4096-char tag and the
// source name follows), so a window that reaches the segment list has already
// seen all metadata. Returns an error when the window is malformed, does not
// hold a complete identifier line, or cuts the source name at the window edge;
// callers treat any error as "fall back to the full-manifest path".
std::expected<HeadMetadata, QString> parseMetadataHead(QByteArrayView head);
std::expected<QByteArray, QString> extractM3u8sIdentifier(QByteArrayView manifest);
std::expected<QByteArray, QString> extractEncryptedSourceFileName(QByteArrayView manifest);
std::expected<QByteArray, QString> insertM3u8sIdentifier(QByteArrayView manifest,
                                                        QByteArrayView identifier);
std::expected<QByteArray, QString> insertEncryptedSourceFileName(QByteArrayView manifest,
                                                                QByteArrayView encryptedSourceFileName);

}
