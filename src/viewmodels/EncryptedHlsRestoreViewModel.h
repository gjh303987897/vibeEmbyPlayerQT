#pragma once

#include "app/FileDialogController.h"
#include "database/SessionRepository.h"
#include "services/encryptedhls/EncryptedHlsRestorer.h"

#include <QAbstractListModel>
#include <functional>

class EncryptedHlsRestoreItems final : public QAbstractListModel {
    Q_OBJECT
public:
    struct Item {
        QString source;
        QString state { QStringLiteral("queued") };
        QString output;
        QString error;
        bool legacyName { false };
    };
    enum Role { NameRole = Qt::UserRole + 1, SourceRole, StateRole, OutputRole, ErrorRole, LegacyNameRole };
    explicit EncryptedHlsRestoreItems(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void add(const QString& source);
    void remove(int row);
    void resetStates();
    void clear();
    void update(int row, const QString& state, const QString& output, const QString& error, bool legacyName);
    QStringList sources() const;
    int countState(const QString& state) const;
private:
    QList<Item> m_items;
    QHash<QString, int> m_counts;
};

class EncryptedHlsRestoreViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(EncryptedHlsRestoreItems* items READ items CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(int successCount READ successCount NOTIFY stateChanged)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY stateChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory NOTIFY settingsChanged)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY settingsChanged)
public:
    using Translator = std::function<QString(const QString&)>;
    explicit EncryptedHlsRestoreViewModel(TsslStore& store, FileDialogController& dialogs,
        SessionRepository& repository, Translator translator, QObject* parent = nullptr);
    EncryptedHlsRestoreItems* items() { return &m_items; }
    int count() const { return m_items.rowCount(); }
    int successCount() const { return m_items.countState(QStringLiteral("completed")); }
    int failureCount() const { return m_items.countState(QStringLiteral("failed")); }
    bool running() const { return m_restorer.isRunning(); }
    double progress() const { return m_restorer.progress(); }
    QString phase() const { return m_restorer.phase(); }
    QString error() const { return m_error; }
    QString outputDirectory() const { return m_outputDirectory; }
    QString format() const { return m_format; }
    void setFormat(const QString& format);
    Q_INVOKABLE void chooseSources();
    Q_INVOKABLE void chooseOutputDirectory();
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openOutputDirectory();
signals:
    void stateChanged();
    void settingsChanged();
private:
    QString translatedError(const QString& error) const;
    FileDialogController& m_dialogs;
    SessionRepository& m_repository;
    Translator m_translate;
    EncryptedHlsRestorer m_restorer;
    EncryptedHlsRestoreItems m_items;
    QString m_outputDirectory;
    QString m_format;
    QString m_error;
};
