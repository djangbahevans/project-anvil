#include "anvil/ui/quick_open.hpp"

#include "anvil/core/fuzzy.hpp"

#include <QLineEdit>
#include <QMetaObject>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace anvil::ui {
namespace {

// How long a read of the listing waits, which is the longest that a new filter waits to be seen.
constexpr std::chrono::milliseconds read_wait{10};
// How often the popup is given what matches while files are still arriving.
constexpr std::chrono::milliseconds report_interval{100};
// How many files are matched between two looks for a newer filter.
constexpr std::size_t files_per_look = 4096;

class RipgrepSource final : public FileSource {
public:
    explicit RipgrepSource(search::FileListing listing) : listing_(std::move(listing)) {}
    search::ListingBatch read(std::chrono::milliseconds timeout) override {
        return listing_.read(timeout);
    }

private:
    search::FileListing listing_;
};

core::FuzzyItem file_item(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return {path, {}, path};
    }
    return {path.substr(slash + 1), path.substr(0, slash), path};
}

core::FuzzyItem entry_item(const QuickOpenEntry& entry) {
    return {entry.label, entry.description, entry.path};
}

// What matched a filter: an entry or a file by its number.
struct Hit {
    std::size_t index = 0;
    core::ItemScore score;
};

// Keeps the best `limit` hits, best first.
template <class ItemOf>
void keep_best(std::vector<Hit>& hits, std::size_t limit, const core::PreparedQuery& query,
               const ItemOf& item_of) {
    const auto better = [&](const Hit& a, const Hit& b) {
        const int order =
            core::compare_items(item_of(a.index), a.score, item_of(b.index), b.score, query);
        return order != 0 ? order < 0 : a.index < b.index;
    };
    const std::size_t kept = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(kept), hits.end(),
                      better);
    hits.resize(kept);
}

} // namespace

FileSourceFactory ripgrep_file_source(std::filesystem::path ripgrep, std::filesystem::path folder,
                                      core::FileSearchOptions options) {
    return [ripgrep = std::move(ripgrep), folder = std::move(folder),
            options = std::move(options)]() -> std::expected<std::unique_ptr<FileSource>, Error> {
        auto listing = search::FileListing::start(ripgrep, folder, options);
        if (!listing) {
            return std::unexpected(listing.error());
        }
        return std::make_unique<RipgrepSource>(std::move(*listing));
    };
}

struct QuickOpen::Results {
    struct Match {
        core::Uri uri;
        std::string label;
        std::string description;
        std::vector<core::MatchRange> label_match;
        std::vector<core::MatchRange> description_match;
    };

    std::uint64_t serial = 0;
    std::vector<Match> matches;
    // Whether files are still arriving.
    bool listing = false;
    std::optional<std::string> error;
};

// The thread that lists the files and matches them against the newest filter text.
class QuickOpen::Search {
public:
    using Report = std::function<void(Results)>;

    Search(std::vector<QuickOpenEntry> entries, std::optional<Files> files, std::uint64_t serial,
           Report report)
        : entries_(std::move(entries)), files_of_(std::move(files)), report_(std::move(report)),
          serial_(serial), latest_(serial), thread_([this](std::stop_token stop) { run(stop); }) {}

    void set_filter(std::string filter, std::uint64_t serial) {
        {
            const std::lock_guard lock(mutex_);
            filter_ = std::move(filter);
            serial_ = serial;
            latest_ = serial;
        }
        changed_.notify_all();
    }

private:
    void run(std::stop_token stop) {
        std::unique_ptr<FileSource> source;
        if (files_of_) {
            for (const QuickOpenEntry& entry : entries_) {
                offered_.insert(entry.path);
            }
            if (auto started = files_of_->source()) {
                source = std::move(*started);
            } else {
                error_ = started.error().message;
                unreported_ = urgent_ = true;
            }
        }
        while (!stop.stop_requested()) {
            std::optional<std::string> filter;
            {
                std::unique_lock lock(mutex_);
                if (!source && !unreported_ && matched_ == files_.size()) {
                    changed_.wait(lock, stop, [&] { return serial_ != answering_; });
                }
                if (serial_ != answering_) {
                    answering_ = serial_;
                    filter = filter_;
                }
            }
            if (stop.stop_requested()) {
                return;
            }
            if (filter) {
                start_over(*filter);
            }
            if (source) {
                take(source);
            }
            if (match_new_files(stop) && unreported_ &&
                (urgent_ || std::chrono::steady_clock::now() - reported_at_ >= report_interval)) {
                report(source != nullptr);
            }
        }
    }

    // Reads what has arrived, and lets the source go once the listing has finished.
    void take(std::unique_ptr<FileSource>& source) {
        search::ListingBatch batch = source->read(read_wait);
        if (!batch.files.empty()) {
            std::ranges::move(batch.files, std::back_inserter(files_));
            unreported_ = true;
        }
        if (batch.finished) {
            if (batch.error) {
                error_ = batch.error->message;
            }
            source.reset();
            unreported_ = urgent_ = true;
        }
    }

    void start_over(const std::string& filter) {
        query_ = core::prepare_query(filter);
        entry_hits_.clear();
        file_hits_.clear();
        matched_ = 0;
        unreported_ = urgent_ = true;
        if (query_.normalized.empty()) {
            return;
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            core::ItemScore score = core::score_item(entry_item(entries_[i]), query_);
            if (score.score != 0) {
                entry_hits_.push_back({i, std::move(score)});
            }
        }
        keep_best(entry_hits_, max_results, query_,
                  [this](std::size_t i) { return entry_item(entries_[i]); });
    }

    // False when a newer filter or the end of the search interrupts.
    bool match_new_files(const std::stop_token& stop) {
        if (query_.normalized.empty()) {
            matched_ = files_.size();
            return true;
        }
        if (matched_ == files_.size()) {
            return true;
        }
        for (std::size_t seen = 0; matched_ < files_.size(); ++matched_, ++seen) {
            if (seen % files_per_look == 0 && (stop.stop_requested() || latest_ != answering_)) {
                return false;
            }
            if (offered_.contains(files_[matched_])) {
                continue;
            }
            core::ItemScore score = core::score_item(file_item(files_[matched_]), query_);
            if (score.score != 0) {
                file_hits_.push_back({matched_, std::move(score)});
            }
        }
        // A score does not change as more files arrive, so only the best ones need keeping.
        keep_best(file_hits_, max_results, query_,
                  [this](std::size_t i) { return file_item(files_[i]); });
        return true;
    }

    void report(bool listing) {
        Results results{.serial = answering_, .matches = {}, .listing = listing, .error = error_};
        const auto add = [&](core::Uri uri, const core::FuzzyItem& item, const Hit* hit) {
            results.matches.push_back(
                {std::move(uri), std::string(item.label), std::string(item.description),
                 hit ? hit->score.label_match : std::vector<core::MatchRange>{},
                 hit ? hit->score.description_match : std::vector<core::MatchRange>{}});
        };
        if (query_.normalized.empty()) {
            for (std::size_t i = 0; i < std::min(entries_.size(), max_results); ++i) {
                add(entries_[i].uri, entry_item(entries_[i]), nullptr);
            }
        } else {
            for (const Hit& hit : entry_hits_) {
                add(entries_[hit.index].uri, entry_item(entries_[hit.index]), &hit);
            }
            for (const Hit& hit : file_hits_) {
                if (results.matches.size() == max_results) {
                    break;
                }
                const std::string& path = files_[hit.index];
                add(core::Uri::file(files_of_->folder / path), file_item(path), &hit);
            }
        }
        report_(std::move(results));
        unreported_ = urgent_ = false;
        reported_at_ = std::chrono::steady_clock::now();
    }

    const std::vector<QuickOpenEntry> entries_;
    const std::optional<Files> files_of_;
    const Report report_;

    std::mutex mutex_;
    std::condition_variable_any changed_;
    // The newest filter and its number, guarded by the mutex; `latest_` repeats the number for
    // the matching to look at without it.
    std::string filter_;
    std::uint64_t serial_;
    std::atomic<std::uint64_t> latest_;

    // The thread's own.
    std::vector<std::string> files_;
    std::unordered_set<std::string> offered_;
    std::uint64_t answering_ = 0;
    core::PreparedQuery query_;
    std::vector<Hit> entry_hits_;
    std::vector<Hit> file_hits_;
    std::size_t matched_ = 0;
    std::optional<std::string> error_;
    bool unreported_ = false;
    bool urgent_ = false;
    std::chrono::steady_clock::time_point reported_at_;

    // Last, so that it is stopped and joined before what it uses is destroyed.
    std::jthread thread_;
};

QuickOpen::QuickOpen(QWidget* window, const WorkbenchColors& colors, QObject* parent)
    : QObject(parent), pick_(new QuickPick(window, colors)) {
    pick_->set_filtering(false);
    pick_->set_placeholder(tr("Search files by name"));
    connect(pick_->filter_field(), &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!search_) {
            return;
        }
        complete_ = false;
        search_->set_filter(text.toStdString(), ++serial_);
        pick_->set_awaiting_items(true);
    });
    connect(pick_, &QuickPick::accepted, this, [this](std::size_t item) {
        const std::optional<core::Uri> uri = uri_of(item);
        search_.reset();
        if (uri) {
            emit chosen(*uri);
        }
    });
    connect(pick_, &QuickPick::cancelled, this, [this] { search_.reset(); });
}

QuickOpen::~QuickOpen() = default;

std::optional<core::Uri> QuickOpen::uri_of(std::size_t item) const {
    return item < shown_.size() ? std::optional(shown_[item]) : std::nullopt;
}

void QuickOpen::open(std::vector<QuickOpenEntry> entries, std::optional<Files> files) {
    search_.reset();
    complete_ = false;
    reported_ = false;

    // The entries show at once; the thread only has to answer what is typed.
    std::vector<QuickPickItem> items;
    shown_.clear();
    for (std::size_t i = 0; i < std::min(entries.size(), max_results); ++i) {
        items.push_back({.label = QString::fromStdString(entries[i].label),
                         .description = QString::fromStdString(entries[i].description),
                         .detail = {},
                         .keybinding = {}});
        shown_.push_back(entries[i].uri);
    }
    pick_->set_empty_message(tr("No recently opened files"));
    pick_->set_items(std::move(items));
    pick_->open();

    ++serial_;
    search_ = std::make_unique<Search>(
        std::move(entries), std::move(files), serial_, [this](Results results) {
            QMetaObject::invokeMethod(
                this, [this, results = std::move(results)] { apply(results); },
                Qt::QueuedConnection);
        });
    search_->set_filter({}, serial_);
}

void QuickOpen::apply(const Results& results) {
    // Closed meanwhile, or results of a filter text that has changed since.
    if (!search_ || results.serial != serial_) {
        return;
    }
    if (results.error && !reported_) {
        reported_ = true;
        emit failed(tr("Listing the files failed: %1").arg(QString::fromStdString(*results.error)));
    }

    // A row that the user moved to stays chosen while more files arrive.
    std::optional<core::Uri> held;
    if (const auto active = pick_->active_item(); active && *active != 0) {
        held = uri_of(*active);
    }
    std::vector<QuickPickItem> items;
    std::vector<core::Uri> uris;
    items.reserve(results.matches.size());
    for (const Results::Match& match : results.matches) {
        items.push_back({.label = QString::fromStdString(match.label),
                         .description = QString::fromStdString(match.description),
                         .detail = {},
                         .keybinding = {},
                         .label_match = match.label_match,
                         .description_match = match.description_match});
        uris.push_back(match.uri);
    }
    complete_ = !results.listing;
    // With an empty filter no file is searched for.
    pick_->set_empty_message(pick_->filter_field()->text().isEmpty()
                                 ? tr("No recently opened files")
                             : results.listing ? tr("Searching…")
                                               : tr("No matching results"));
    // Files that arrive and change nothing that shows leave the list as it is.
    if (applied_ != results.serial || uris != shown_ || uris.empty()) {
        applied_ = results.serial;
        shown_ = std::move(uris);
        pick_->set_items(std::move(items));
        if (held) {
            if (const auto found = std::ranges::find(shown_, *held); found != shown_.end()) {
                pick_->set_active_item(static_cast<std::size_t>(found - shown_.begin()));
            }
        }
    }
    // Last: a held Enter chooses here, which ends the search.
    if (!results.listing || !results.matches.empty() || pick_->filter_field()->text().isEmpty()) {
        pick_->set_awaiting_items(false);
    }
}

} // namespace anvil::ui
