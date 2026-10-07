#include "services/credentials/NativeCredentialStore.h"

#ifndef Q_OS_WIN
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentRun>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(Q_OS_LINUX)
// GLib's 'signals' member collides with Qt's compatibility macro.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace {
using Result = std::expected<std::optional<QString>, QString>;
using Operation = NativeCredentialStore::Operation;

Result executeNative(Operation operation, const QString& target, const QString& secret)
{
    if (target.isEmpty() || target.contains(QChar::Null) || secret.contains(QChar::Null))
        return std::unexpected(QStringLiteral("Invalid system credential input"));
#ifdef Q_OS_MACOS
    const auto accountBytes = target.toUtf8();
    const auto account = CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(accountBytes.constData()), accountBytes.size(), kCFStringEncodingUTF8, false);
    const auto query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const auto release = qScopeGuard([&] { CFRelease(query); CFRelease(account); });
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("vibePlayerQT"));
    CFDictionarySetValue(query, kSecAttrAccount, account);
    // Console commands must not create a system authentication window. A locked
    // keychain or denied ACL returns an actionable failure instead of hanging.
    CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    OSStatus status = errSecSuccess;
    if (operation == Operation::Read) {
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef output = nullptr;
        status = SecItemCopyMatching(query, &output);
        const auto freeOutput = qScopeGuard([&] { if (output) CFRelease(output); });
        if (status == errSecItemNotFound) return std::optional<QString> {};
        if (status == errSecSuccess && output && CFGetTypeID(output) == CFDataGetTypeID()) {
            const auto data = static_cast<CFDataRef>(output);
            return std::optional<QString> {QString::fromUtf8(
                reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data))};
        }
    } else if (operation == Operation::Remove) {
        status = SecItemDelete(query);
        if (status == errSecSuccess || status == errSecItemNotFound) return std::optional<QString> {};
    } else {
        auto bytes = secret.toUtf8();
        const auto wipe = qScopeGuard([&] { bytes.fill('\0'); });
        const auto data = CFDataCreate(kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size());
        const auto attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const auto freeData = qScopeGuard([&] { CFRelease(attributes); CFRelease(data); });
        CFDictionarySetValue(attributes, kSecValueData, data);
        status = SecItemUpdate(query, attributes);
        if (status == errSecItemNotFound) {
            // These are the product's sibling entry points, never an unrestricted
            // ACL. The classic login keychain supports SecAccess on generic items.
            const auto trusted = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            SecAccessRef access = nullptr;
            const auto freeAccess = qScopeGuard([&] { if (access) CFRelease(access); CFRelease(trusted); });
            SecTrustedApplicationRef callingApp = nullptr;
            status = SecTrustedApplicationCreateFromPath(nullptr, &callingApp);
            if (status != errSecSuccess) return std::unexpected(QStringLiteral("Unable to create Keychain access policy"));
            CFArrayAppendValue(trusted, callingApp);
            CFRelease(callingApp);
            const QDir directory(QCoreApplication::applicationDirPath());
            for (const auto& name : {"vibePlayerQT", "vibePlayerQT-cli", "vibePlayerPlayback"}) {
                auto path = directory.absoluteFilePath(QString::fromLatin1(name));
                if (!QFileInfo::exists(path)) continue;
                const auto bundle = QDir(directory.absoluteFilePath(QStringLiteral("../.."))).absolutePath();
                if (qstrcmp(name, "vibePlayerQT") == 0 && bundle.endsWith(QStringLiteral(".app"))) path = bundle;
                SecTrustedApplicationRef application = nullptr;
                const auto encoded = QFile::encodeName(path);
                status = SecTrustedApplicationCreateFromPath(encoded.constData(), &application);
                if (status != errSecSuccess) return std::unexpected(QStringLiteral("Unable to trust a product entry point in Keychain"));
                CFArrayAppendValue(trusted, application);
                CFRelease(application);
            }
            status = SecAccessCreate(CFSTR("vibePlayerQT credentials"), trusted, &access);
            if (status != errSecSuccess) return std::unexpected(QStringLiteral("Unable to create Keychain access policy"));
            CFDictionarySetValue(query, kSecAttrAccess, access);
            CFDictionarySetValue(query, kSecValueData, data);
            status = SecItemAdd(query, nullptr);
        }
        if (status == errSecSuccess) return std::optional<QString> {};
    }
    return std::unexpected(QStringLiteral("Keychain access failed (%1); unlock the login keychain and allow this application in Keychain Access").arg(status));
#elif defined(Q_OS_LINUX)
    const auto schema = secret_schema_new("org.vibePlayerQT.Credentials", SECRET_SCHEMA_NONE,
                                          "target", SECRET_SCHEMA_ATTRIBUTE_STRING, nullptr);
    auto* cancellable = g_cancellable_new();
    const auto release = qScopeGuard([&] { g_object_unref(cancellable); secret_schema_unref(schema); });
    // The synchronous libsecret API can otherwise wait indefinitely for a
    // missing/locked service or an unanswered unlock prompt. Cancellation is
    // explicitly thread-safe; stop/join the watchdog before releasing its handle.
    std::mutex mutex;
    std::condition_variable_any condition;
    std::jthread watchdog([&](std::stop_token stop) {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, stop, std::chrono::seconds(15), [] { return false; });
        if (!stop.stop_requested()) g_cancellable_cancel(cancellable);
    });
    const auto stopWatchdog = qScopeGuard([&] { watchdog.request_stop(); watchdog.join(); });
    GError* error = nullptr;
    const auto freeError = qScopeGuard([&] { if (error) g_error_free(error); });
    const auto account = target.toUtf8();
    if (operation != Operation::Write) {
        auto* attributes = secret_attributes_build(schema, "target", account.constData(), nullptr);
        const auto freeAttributes = qScopeGuard([&] { g_hash_table_unref(attributes); });
        auto* items = secret_service_search_sync(nullptr, schema, attributes, SECRET_SEARCH_ALL, cancellable, &error);
        const auto freeItems = qScopeGuard([&] { g_list_free_full(items, g_object_unref); });
        if (error) return std::unexpected(QStringLiteral("Secret Service is unavailable or timed out"));
        if (!items) return std::optional<QString> {};
        // clear_sync only removes unlocked entries. Never claim deletion (or a
        // missing password) when a matching item remains in a locked collection.
        for (auto* item = items; item; item = item->next) {
            if (secret_item_get_locked(SECRET_ITEM(item->data)))
                return std::unexpected(QStringLiteral("System keyring is locked; unlock it before accessing saved credentials"));
        }
    }
    if (operation == Operation::Read) {
        auto* password = secret_password_lookup_sync(schema, cancellable, &error,
                                                     "target", account.constData(), nullptr);
        const auto wipe = qScopeGuard([&] { if (password) secret_password_free(password); });
        if (!error) {
            if (!password) return std::unexpected(QStringLiteral("System credential could not be read; unlock the keyring"));
            return std::optional<QString> {QString::fromUtf8(password)};
        }
    } else if (operation == Operation::Remove) {
        const auto cleared = secret_password_clear_sync(schema, cancellable, &error, "target", account.constData(), nullptr);
        if (!error && cleared) {
            auto* attributes = secret_attributes_build(schema, "target", account.constData(), nullptr);
            const auto freeAttributes = qScopeGuard([&] { g_hash_table_unref(attributes); });
            auto* remaining = secret_service_search_sync(nullptr, schema, attributes, SECRET_SEARCH_ALL, cancellable, &error);
            const auto freeItems = qScopeGuard([&] { g_list_free_full(remaining, g_object_unref); });
            if (!error && !remaining) return std::optional<QString> {};
        }
    } else {
        auto bytes = secret.toUtf8();
        const auto wipe = qScopeGuard([&] { bytes.fill('\0'); });
        if (secret_password_store_sync(schema, nullptr, "vibePlayerQT credential", bytes.constData(),
                                        cancellable, &error, "target", account.constData(), nullptr))
            return std::optional<QString> {};
    }
    // Backend messages may contain private metadata; do not forward them to logs.
    return std::unexpected(QStringLiteral("Secret Service access failed or timed out; start and unlock a compatible system keyring (GNOME Keyring or KeePassXC)"));
#else
    Q_UNUSED(operation)
    return std::unexpected(QStringLiteral("System credential store is not available on this platform"));
#endif
}
}

std::expected<std::optional<QString>, QString> NativeCredentialStore::execute(
    Operation operation, const QString& target, const QString& secret)
{
    // A separate pool prevents deadlock when a repository operation already
    // runs inside QtConcurrent's global pool. The native calls never run on UI.
    static QThreadPool pool;
    auto future = QtConcurrent::run(&pool, [operation, target, secret] {
        return executeNative(operation, target, secret);
    });
    if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) {
        QEventLoop loop;
        QFutureWatcher<Result> watcher;
        QObject::connect(&watcher, &QFutureWatcher<Result>::finished, &loop, &QEventLoop::quit);
        watcher.setFuture(future);
        while (!future.isFinished()) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
            // A quit request can end the nested loop before the native result.
            // Continue dispatching rather than blocking UI in future.result().
            if (!future.isFinished()) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
                QThread::msleep(1);
            }
        }
    } else {
        future.waitForFinished();
    }
    return future.result();
}
#endif
