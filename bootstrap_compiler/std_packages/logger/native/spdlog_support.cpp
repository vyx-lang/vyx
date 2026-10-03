#include "spdlog_dci.hpp"

#include <cstring>
#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>

spdlog::logger *dci_default_logger() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_unique<spdlog::logger>("vyx", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%l] %v");
    return logger.release();
}

void dci_drop_logger(spdlog::logger *logger) {
    delete logger;
}

// logger.log(level, string_view) is inline in logger.h. Force an out-of-line
// body so a Vyx DCI direct call has a linkable symbol in this TU.
namespace {
using DciLoggerLogFn =
    void (spdlog::logger::*)(spdlog::level::level_enum, spdlog::string_view_t);
}  // namespace

extern "C" __attribute__((used)) void *dci_force_logger_log_emit() {
    DciLoggerLogFn fn = static_cast<DciLoggerLogFn>(&spdlog::logger::log);
    void *opaque = nullptr;
    std::memcpy(&opaque, &fn, sizeof(opaque));
    return opaque;
}
