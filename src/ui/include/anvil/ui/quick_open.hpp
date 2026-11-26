#pragma once

#include "anvil/core/file_tree.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/search/ripgrep.hpp"
#include "anvil/ui/quick_pick.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::ui {

// Gives the files of a folder in batches, as `search::FileListing` does, on the thread that asks.
class FileSource {
public:
    virtual ~FileSource() = default;
    virtual search::ListingBatch read(std::chrono::milliseconds timeout) = 0;
};

// Starts a listing. It is called on the thread that then reads the source.
using FileSourceFactory = std::function<std::expected<std::unique_ptr<FileSource>, Error>()>;

// The files of `folder` as ripgrep lists them.
FileSourceFactory ripgrep_file_source(std::filesystem::path ripgrep, std::filesystem::path folder,
                                      core::FileSearchOptions options);

// A file or a document that quick open offers.
struct QuickOpenEntry {
    core::Uri uri;
    // The name, and the directory that it is shown with: relative to the workspace folder, or in
    // full for a file outside it.
    std::string label{};
    std::string description{};
    // What a filter with a path separator is matched against. For a file below the folder that is
    // listed it is the path relative to it, so that the file is offered once.
    std::string path{};
};

// Go to File: a quick pick over the files of a folder, matched and ranked as VS Code does it.
//
// It runs in a popup that other lists use too, such as the command palette: `open` takes the popup
// over, in place when it is showing, and `end` gives it up.
//
// With an empty filter it offers the entries it was given, in their order. With a filter it offers
// the ones of them that match, best first, and then the files of the folder that match, best first,
// at most `max_results` in all. The folder is listed each time the popup opens, and the listing and
// the matching run on a thread of their own, so that typing does not wait for them; the popup shows
// what matches while the listing runs. Closing the popup ends both.
class QuickOpen : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t max_results = 512;

    // The folder whose files are offered.
    struct Files {
        std::filesystem::path folder;
        FileSourceFactory source;
    };

    // The icon of a file or document, for the row that offers it. It is asked on the thread of
    // the popup, for the rows that the popup is given.
    using IconSource = std::function<std::optional<QuickPickIcon>(const core::Uri&)>;

    // The popup must outlive this object.
    explicit QuickOpen(QuickPick& pick, QObject* parent = nullptr);
    ~QuickOpen() override;

    void set_icon_source(IconSource source) {
        icon_source_ = std::move(source);
    }

    QuickPick* pick() const {
        return pick_;
    }

    // Shows the popup with `filter` in its field, offers the entries, and lists the files when
    // there is a folder.
    void open(std::vector<QuickOpenEntry> entries, std::optional<Files> files,
              const QString& filter = {});
    // Ends the listing and the matching, and leaves the popup as it is, for another use of it.
    void end();
    bool is_open() const {
        return search_ != nullptr;
    }
    // Whether the shown items answer the filter text and every file has been listed.
    bool is_complete() const {
        return complete_;
    }
    // Where the item at this position of the popup's list leads.
    std::optional<core::Uri> uri_of(std::size_t item) const;

signals:
    // The user chose a file or document. The popup is closed.
    void chosen(const anvil::core::Uri& uri);
    // The listing failed: a sentence for the status bar.
    void failed(const QString& message);

private:
    struct Results;
    class Search;

    void apply(const Results& results);
    std::optional<QuickPickIcon> icon_of(const core::Uri& uri) const {
        return icon_source_ ? icon_source_(uri) : std::nullopt;
    }
    void set_filter(const QString& text);

    QPointer<QuickPick> pick_;
    IconSource icon_source_;
    // To the popup, while it is ours.
    std::vector<QMetaObject::Connection> connections_;
    std::unique_ptr<Search> search_;
    // Where the items that the popup shows lead.
    std::vector<core::Uri> shown_;
    // Counts the filter texts, over every time the popup opens, so that results that were on their
    // way for an older one are told from those for the one there is.
    std::uint64_t serial_ = 0;
    // The number of the filter text that the shown items answer.
    std::uint64_t applied_ = 0;
    bool complete_ = false;
    bool reported_ = false;
};

} // namespace anvil::ui
