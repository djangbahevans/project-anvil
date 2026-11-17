#include "anvil/platform/process.hpp"

// Process management is not implemented on Windows yet; every operation reports `unsupported`.
namespace anvil::platform {

struct Process::Impl {};

std::expected<Process, Error> Process::spawn(const ProcessSpec&) {
    return std::unexpected(
        Error{ErrorCode::unsupported, "starting processes is not implemented on Windows"});
}

Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;
Process::~Process() = default;

std::int64_t Process::id() const {
    return -1;
}

std::optional<int> Process::wait(std::chrono::milliseconds) {
    return std::nullopt;
}

void Process::terminate() {}
void Process::kill() {}

std::int64_t current_process_id() {
    return 0;
}

std::vector<std::string> current_environment() {
    return {};
}

} // namespace anvil::platform
