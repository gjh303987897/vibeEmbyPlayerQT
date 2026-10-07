#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "services/management/ServiceOperations.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUuid>

Cli::Result Cli::Engine::privacy(const Invocation& invocation, SessionRepository& repository)
{
    const bool configured = repository.privacyPinConfigured();
    if (invocation.command == "privacy set" && configured)
        return failure(ExitCode::Conflict,"A PIN already exists; use privacy change");
    if (invocation.command != "privacy set") {
        if (!configured) return failure(ExitCode::NotFound,"No privacy PIN is configured");
        auto old = secret(invocation,"pin-env","Current PIN: ");
        if (!old) return failure(old.error());
        if (!ServiceOperations::verifyPrivacyPin(repository,*old))
            return failure(ExitCode::Authentication,"Current PIN is incorrect");
    }
    if (invocation.command == "privacy remove") {
        if (!confirm(invocation,"Remove the privacy PIN? Private services remain locked until a new PIN is set."))
            return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage,"Removal requires confirmation");
        repository.setPrivacyPinHash({},{});
        m_private = false; m_pin.clear(); m_serviceId.clear(); m_serviceName.clear(); m_directory = "/";
        return success(QStringLiteral("Privacy PIN removed"));
    }
    auto pin = secret(invocation,"new-pin-env","New PIN: ");
    if (!pin) return failure(pin.error());
    auto confirmation = secret(invocation,"confirm-pin-env","Confirm new PIN: ");
    if (!confirmation) return failure(confirmation.error());
    if (*pin != *confirmation) return failure(ExitCode::Usage,"PIN confirmation does not match");
    static const QRegularExpression pattern(QStringLiteral("^\\d{4,12}$"));
    if (!pattern.match(*pin).hasMatch()) return failure(ExitCode::Usage,"PIN must contain 4 to 12 digits");
    const auto salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    repository.setPrivacyPinHash(salt,QString::fromLatin1(
        QCryptographicHash::hash((salt + ':' + *pin).toUtf8(),QCryptographicHash::Sha256).toHex()));
    m_private = false;
    m_pin.clear(); m_serviceId.clear(); m_serviceName.clear(); m_directory = "/";
    return success(QStringLiteral("Privacy PIN saved"));
}
