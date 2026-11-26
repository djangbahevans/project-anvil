#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/file_tree.hpp"
#include "anvil/platform/process.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::search {

// Where ripgrep is looked for.
struct RipgrepLocations {
    // The value of `ANVIL_RG`; empty when it is not set.
    std::filesystem::path override_path{};
    // The VS Code checkout, which carries a ripgrep for each platform; empty without one.
    std::filesystem::path vscode_dir{};
    // The value of `PATH`.
    std::string path_variable{};
};

// The ripgrep to run, as an absolute path: the override, whether or not it can be run, so that a
// wrong one is reported when it is started, else the one of the VS Code checkout, else `rg` on the
// path. None when there is none.
std::optional<std::filesystem::path> find_ripgrep(const RipgrepLocations& locations);

// The arguments of `rg` that lists the files of the directory it runs in, as VS Code passes them
// (`ripgrepFileSearch.ts`), with names that end in a null byte.
std::vector<std::string> list_arguments(const core::FileSearchOptions& options);

// What a read of a listing gives.
struct ListingBatch {
    // Paths relative to the folder, with forward slashes, as the bytes of their names.
    std::vector<std::string> files{};
    // True once the listing has ended: every file has been given.
    bool finished = false;
    // Why the listing is not complete, with what ripgrep said; set with `finished`. The files that
    // were listed are good.
    std::optional<Error> error{};
};

// The files below a folder, as ripgrep lists them while it runs (ADR 0020). Nothing happens
// between reads, so a caller that stops reading stops ripgrep once its pipe is full. Destroying a
// listing ends ripgrep. One thread uses a listing at a time.
class FileListing {
public:
    // Fails when ripgrep cannot be started or the folder is not a directory.
    static std::expected<FileListing, Error> start(const std::filesystem::path& ripgrep,
                                                   const std::filesystem::path& folder,
                                                   const core::FileSearchOptions& options);

    // Waits up to `timeout` for ripgrep to say something, and gives what has arrived, which can be
    // nothing. A listing that has finished gives nothing more.
    ListingBatch read(std::chrono::milliseconds timeout);

    // Ends ripgrep at once and finishes the listing.
    void cancel();

private:
    explicit FileListing(platform::Process process);

    // Reads what the output or the error has ready.
    void drain_output(std::vector<std::string>& files);
    void drain_error();
    void end(ListingBatch& batch);
    void fail(ListingBatch& batch, const Error& error);

    std::optional<platform::Process> process_;
    // The start of a name whose end has not arrived.
    std::string partial_;
    std::string messages_;
    bool output_closed_ = false;
    bool error_closed_ = false;
    std::optional<Error> read_error_;
};

} // namespace anvil::search
