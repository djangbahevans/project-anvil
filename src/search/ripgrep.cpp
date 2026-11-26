#include "anvil/search/ripgrep.hpp"

#include "anvil/platform/executable.hpp"
#include "anvil/platform/target.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace anvil::search {
namespace {

// How much of the output one read takes at most, so that a batch stays small enough to handle at
// once.
constexpr std::size_t batch_bytes = 256 * 1024;
// How much of what ripgrep says is wrong is kept.
constexpr std::size_t message_bytes = 4 * 1024;

// A glob of the settings as ripgrep is given it: relative to the searched folder unless it starts
// with `**`, as `anchorGlob` of VS Code does.
std::string anchored(std::string glob) {
    std::ranges::replace(glob, '\\', '/');
    while (!glob.empty() && glob.back() == '/') {
        glob.pop_back();
    }
    if (glob.empty() || glob.starts_with("**") || glob.starts_with('/')) {
        return glob;
    }
    return "/" + glob;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::optional<std::filesystem::path> find_ripgrep(const RipgrepLocations& locations) {
    std::error_code error;
    if (!locations.override_path.empty()) {
        return std::filesystem::absolute(locations.override_path, error);
    }
    const std::string target = platform::vscode_target_platform();
    const char* name = target.starts_with("win32") ? "rg.exe" : "rg";
    if (!locations.vscode_dir.empty()) {
        const std::filesystem::path bundled = locations.vscode_dir / "node_modules" / "@vscode" /
                                              "ripgrep-universal" / "bin" / target / name;
        if (platform::is_executable(bundled)) {
            return std::filesystem::absolute(bundled, error);
        }
    }
    return platform::find_executable(name, locations.path_variable);
}

std::vector<std::string> list_arguments(const core::FileSearchOptions& options) {
    std::vector<std::string> arguments{"--files", "--hidden", "--case-sensitive",
                                       "--no-require-git"};
    for (const std::string& exclude : options.excludes) {
        if (const std::string glob = anchored(exclude); !glob.empty()) {
            arguments.emplace_back("-g");
            arguments.push_back("!" + glob);
        }
    }
    if (!options.use_ignore_files) {
        arguments.emplace_back("--no-ignore");
    } else if (!options.use_parent_ignore_files) {
        arguments.emplace_back("--no-ignore-parent");
    }
    if (options.follow_symlinks) {
        arguments.emplace_back("--follow");
    }
    arguments.emplace_back("--no-config");
    if (!options.use_global_ignore_files) {
        arguments.emplace_back("--no-ignore-global");
    }
    arguments.emplace_back("--null");
    arguments.emplace_back("--path-separator=/");
    return arguments;
}

FileListing::FileListing(platform::Process process) : process_(std::move(process)) {}

std::expected<FileListing, Error> FileListing::start(const std::filesystem::path& ripgrep,
                                                     const std::filesystem::path& folder,
                                                     const core::FileSearchOptions& options) {
    platform::ProcessSpec spec{.executable = ripgrep,
                               .arguments = list_arguments(options),
                               .environment = platform::current_environment(),
                               .working_directory = folder,
                               .pipe_output = true,
                               .pipe_error = true};
    auto process = platform::Process::spawn(spec);
    if (!process) {
        return std::unexpected(process.error());
    }
    return FileListing(std::move(*process));
}

void FileListing::drain_output(std::vector<std::string>& files) {
    std::array<std::byte, 16 * 1024> buffer{};
    std::size_t taken = 0;
    while (!output_closed_ && taken < batch_bytes) {
        const auto read = process_->read_output(buffer, std::chrono::milliseconds(0));
        if (!read) {
            read_error_ = read.error();
            return;
        }
        if (read->bytes == 0) {
            output_closed_ = read->closed;
            return;
        }
        taken += read->bytes;
        std::string_view text(reinterpret_cast<const char*>(buffer.data()), read->bytes);
        for (std::size_t end = text.find('\0'); end != std::string_view::npos;
             end = text.find('\0')) {
            partial_.append(text.substr(0, end));
            files.push_back(std::exchange(partial_, {}));
            text.remove_prefix(end + 1);
        }
        partial_.append(text);
    }
}

void FileListing::drain_error() {
    std::array<std::byte, 4 * 1024> buffer{};
    std::size_t taken = 0;
    while (!error_closed_ && taken < batch_bytes) {
        const auto read = process_->read_error(buffer, std::chrono::milliseconds(0));
        if (!read) {
            read_error_ = read.error();
            return;
        }
        if (read->bytes == 0) {
            error_closed_ = read->closed;
            return;
        }
        taken += read->bytes;
        messages_.append(reinterpret_cast<const char*>(buffer.data()),
                         std::min(read->bytes, message_bytes - messages_.size()));
    }
}

ListingBatch FileListing::read(std::chrono::milliseconds timeout) {
    ListingBatch batch;
    if (!process_) {
        batch.finished = true;
        return batch;
    }
    if (!output_closed_ || !error_closed_) {
        if (const auto ready = process_->wait_for_output(timeout); !ready) {
            fail(batch, ready.error());
            return batch;
        }
        drain_output(batch.files);
        drain_error();
        if (read_error_) {
            fail(batch, *read_error_);
            return batch;
        }
    }
    if (output_closed_ && error_closed_) {
        end(batch);
    }
    return batch;
}

void FileListing::end(ListingBatch& batch) {
    // Both streams are closed, so ripgrep is about to exit; until it has, the listing goes on.
    const std::optional<int> status = process_->wait(std::chrono::milliseconds(100));
    if (!status) {
        return;
    }
    // 1 is a folder with no files to list.
    if (*status != 0 && *status != 1) {
        const std::string said(trimmed(messages_));
        batch.error =
            Error{ErrorCode::io_error,
                  said.empty() ? "ripgrep failed with status " + std::to_string(*status) : said};
    }
    batch.finished = true;
    process_.reset();
}

void FileListing::fail(ListingBatch& batch, const Error& error) {
    batch.error = error;
    batch.finished = true;
    process_.reset();
}

void FileListing::cancel() {
    process_.reset();
}

} // namespace anvil::search
