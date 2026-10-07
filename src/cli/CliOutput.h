#pragma once
#include "cli/CliCommand.h"

namespace Cli {
struct RenderedOutput {
    QString standardOutput;
    QString standardError;
};

// Pure presentation: no console, settings, network, or database access.
RenderedOutput renderResult(const Invocation& invocation, const Result& result, int columns = 120);
}
