#include "viewmodels/EncryptedHlsRestoreViewModel.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

int EncryptedHlsRestoreItems::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant EncryptedHlsRestoreItems::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto& item = m_items.at(index.row());
    switch (role) {
    case NameRole: return QFileInfo(item.source).fileName();
    case SourceRole: return item.source;
    case StateRole: return item.state;
    case OutputRole: return item.output;
    case ErrorRole: return item.error;
    case LegacyNameRole: return item.legacyName;
    default: return {};
    }
}

QHash<int, QByteArray> EncryptedHlsRestoreItems::roleNames() const
{
    return {{NameRole, "fileName"}, {SourceRole, "sourcePath"}, {StateRole, "itemState"},
            {OutputRole, "outputPath"}, {ErrorRole, "itemError"}, {LegacyNameRole, "legacyName"}};
}

void EncryptedHlsRestoreItems::add(const QString& source)
{
    // Inputs have already been canonicalized by the picker continuation. Avoid
    // probing the disk for every existing queue row when adding a large batch.
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    if (std::ranges::any_of(m_items, [&source](const Item& item) { return item.source.compare(source, pathCase) == 0; })) return;
    beginInsertRows({}, m_items.size(), m_items.size());
    m_items.append(Item { .source = source });
    ++m_counts[QStringLiteral("queued")];
    endInsertRows();
}

void EncryptedHlsRestoreItems::remove(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    beginRemoveRows({}, row, row);
    --m_counts[m_items.at(row).state];
    m_items.removeAt(row);
    endRemoveRows();
}

void EncryptedHlsRestoreItems::resetStates()
{
    for (auto& item : m_items) { item.state = QStringLiteral("queued"); item.output.clear(); item.error.clear(); item.legacyName = false; }
    m_counts.clear();
    m_counts[QStringLiteral("queued")] = m_items.size();
    if (!m_items.isEmpty()) emit dataChanged(index(0), index(m_items.size() - 1));
}

void EncryptedHlsRestoreItems::clear()
{
    beginResetModel(); m_items.clear(); m_counts.clear(); endResetModel();
}

void EncryptedHlsRestoreItems::update(int row, const QString& state, const QString& output, const QString& error, bool legacyName)
{
    if (row < 0 || row >= m_items.size()) return;
    auto& item = m_items[row];
    --m_counts[item.state];
    ++m_counts[state];
    item.state = state; item.output = output; item.error = error; item.legacyName = legacyName;
    emit dataChanged(index(row), index(row));
}

QStringList EncryptedHlsRestoreItems::sources() const
{
    QStringList sources;
    for (const auto& item : m_items) sources.append(item.source);
    return sources;
}

int EncryptedHlsRestoreItems::countState(const QString& state) const
{
    return m_counts.value(state);
}

EncryptedHlsRestoreViewModel::EncryptedHlsRestoreViewModel(TsslStore& store, FileDialogController& dialogs,
    SessionRepository& repository, Translator translator, QObject* parent)
    : QObject(parent), m_dialogs(dialogs), m_repository(repository), m_translate(std::move(translator)),
      m_restorer(store, this), m_items(this), m_outputDirectory(repository.encryptedHlsRestoreOutputDirectory()),
      m_format(repository.encryptedHlsRestoreFormat())
{
    connect(&m_restorer, &EncryptedHlsRestorer::stateChanged, this, &EncryptedHlsRestoreViewModel::stateChanged);
    connect(&m_restorer, &EncryptedHlsRestorer::itemChanged, this,
            [this](int row, const QString& state, const QString& output, const QString& error, bool legacyName) {
        m_items.update(row, state, output, translatedError(error), legacyName);
        emit stateChanged();
    });
}

void EncryptedHlsRestoreViewModel::setFormat(const QString& format)
{
    if (running() || (format != QStringLiteral("original") && format != QStringLiteral("mkv") && format != QStringLiteral("mp4"))) return;
    if (m_format == format) return;
    m_format = format;
    m_repository.setEncryptedHlsRestoreFormat(format);
    emit settingsChanged();
}

void EncryptedHlsRestoreViewModel::chooseSources()
{
    if (running()) return;
    const auto sources = m_items.sources();
    m_dialogs.openFiles(m_translate(QStringLiteral("restore.addFiles")),
        sources.isEmpty() ? QDir::homePath() : QFileInfo(sources.last()).absolutePath(),
        QStringLiteral("M3U8S / M3U8SP (*.m3u8s *.m3u8sp)"), [this](const QStringList& selected) {
        if (running() || selected.isEmpty()) return;
        m_error.clear();
        for (const auto& path : selected) {
            const QFileInfo file(path);
            if (!file.isFile() || !file.isReadable() ||
                (file.suffix().compare(QStringLiteral("m3u8s"), Qt::CaseInsensitive) != 0 &&
                 file.suffix().compare(QStringLiteral("m3u8sp"), Qt::CaseInsensitive) != 0)) {
                m_error = m_translate(QStringLiteral("restore.invalidSource"));
                continue;
            }
            m_items.add(file.canonicalFilePath());
        }
        emit stateChanged();
    });
}

void EncryptedHlsRestoreViewModel::chooseOutputDirectory()
{
    if (running()) return;
    m_dialogs.selectDirectory(m_translate(QStringLiteral("restore.output")),
        m_outputDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) : m_outputDirectory,
        [this](const QString& directory) {
        if (running() || directory.isEmpty()) return;
        m_outputDirectory = directory;
        m_repository.setEncryptedHlsRestoreOutputDirectory(directory);
        emit settingsChanged();
    });
}

void EncryptedHlsRestoreViewModel::remove(int row)
{
    if (running()) return;
    m_items.remove(row); emit stateChanged();
}

void EncryptedHlsRestoreViewModel::clear()
{
    if (running()) return;
    m_items.clear(); m_error.clear(); emit stateChanged();
}

void EncryptedHlsRestoreViewModel::start()
{
    if (running()) return;
    m_error.clear();
    auto result = m_restorer.start({m_items.sources(), m_outputDirectory, m_format});
    if (!result) m_error = translatedError(result.error());
    else m_items.resetStates();
    emit stateChanged();
}

void EncryptedHlsRestoreViewModel::cancel() { m_restorer.cancel(); }

QString EncryptedHlsRestoreViewModel::translatedError(const QString& error) const
{
    const auto text = error.toLower();
    QString key;
    if (text.contains(QStringLiteral("no matching local tssl"))) key = QStringLiteral("missingKey");
    else if (text.contains(QStringLiteral("original container format is unsupported"))) key = QStringLiteral("container");
    else if (text.contains(QStringLiteral("disk is full"))) key = QStringLiteral("diskFull");
    else if (text.contains(QStringLiteral("ffmpeg is unavailable")) || text.contains(QStringLiteral("ffmpeg could not start"))) key = QStringLiteral("ffmpeg");
    else if (text.contains(QStringLiteral("ffmpeg could not restore"))) key = QStringLiteral("restore");
    else if (text.contains(QStringLiteral("writable output folder"))) key = QStringLiteral("output");
    else if (text.contains(QStringLiteral("stopped responding"))) key = QStringLiteral("timeout");
    else if (text.contains(QStringLiteral("segment")) || text.contains(QStringLiteral("integrity")) ||
             text.contains(QStringLiteral("hls resource")) || text.contains(QStringLiteral("metadata does not match"))) key = QStringLiteral("integrity");
    else if (text.contains(QStringLiteral("unable to save")) || text.contains(QStringLiteral("temporary output directory"))) key = QStringLiteral("save");
    return key.isEmpty() ? error : m_translate(QStringLiteral("restore.error.") + key);
}

void EncryptedHlsRestoreViewModel::openOutputDirectory()
{
    if (!QFileInfo(m_outputDirectory).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_outputDirectory))) {
        m_error = m_translate(QStringLiteral("m3u8s.openFolderFailed")); emit stateChanged();
    }
}
