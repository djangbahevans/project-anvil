#include "anvil/core/highlight_worker.hpp"
#include "anvil/core/text_buffer.hpp"
#include "anvil/core/tree_sitter_highlighter.hpp"

#include <gtest/gtest.h>

#ifdef ANVIL_TREE_SITTER

#include <chrono>
#include <future>

using namespace anvil::core;
using anvil::ErrorCode;

namespace {

Text text_of(std::string_view utf8) {
    auto text = Text::from_utf8(utf8);
    EXPECT_TRUE(text.has_value());
    return text ? *text : Text{};
}

std::string render(const Highlighter& highlighter, const Text& text) {
    std::string out;
    for (std::size_t line = 0; line < text.line_count(); ++line) {
        for (const HighlightSpan& span : highlighter.spans(line)) {
            out += std::to_string(line) + " " + std::to_string(span.begin) + "-" +
                   std::to_string(span.end) + " " + std::string(span.scope) + "\n";
        }
    }
    return out;
}

std::string fresh_render(const Text& text) {
    const auto highlighter = TreeSitterHighlighter::create("json", text);
    return highlighter ? render(**highlighter, text) : std::string();
}

// What the worker delivered, for a test to wait on.
class Results {
public:
    HighlightWorker::Deliver deliver() {
        return [this](std::uint64_t version, std::shared_ptr<const Highlighter> highlighter) {
            {
                const std::lock_guard lock(mutex_);
                versions_.push_back(version);
                last_ = std::move(highlighter);
            }
            arrived_.notify_all();
            std::unique_lock lock(mutex_);
            gate_open_.wait(lock, [this] { return !gated_ || versions_.size() <= gated_after_; });
        };
    }

    bool wait_for_version(std::uint64_t version) {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, std::chrono::seconds(60),
                                 [&] { return !versions_.empty() && versions_.back() >= version; });
    }
    bool wait_for_count(std::size_t count) {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, std::chrono::seconds(60),
                                 [&] { return versions_.size() >= count; });
    }
    // Holds the worker in the callback of every delivery after the first `count` ones until
    // `release`.
    void hold_after(std::size_t count) {
        const std::lock_guard lock(mutex_);
        gated_ = true;
        gated_after_ = count;
    }
    void release() {
        {
            const std::lock_guard lock(mutex_);
            gated_ = false;
        }
        gate_open_.notify_all();
    }
    std::vector<std::uint64_t> versions() {
        const std::lock_guard lock(mutex_);
        return versions_;
    }
    std::shared_ptr<const Highlighter> last() {
        const std::lock_guard lock(mutex_);
        return last_;
    }

private:
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::condition_variable gate_open_;
    std::vector<std::uint64_t> versions_;
    std::shared_ptr<const Highlighter> last_;
    bool gated_ = false;
    std::size_t gated_after_ = 0;
};

std::string document_of(int rows) {
    std::string out = "{\n";
    for (int i = 0; i < rows; ++i) {
        out += "  \"key" + std::to_string(i) + "\": [1, \"v\", {\"a\": null}]";
        out += i + 1 < rows ? ",\n" : "\n";
    }
    return out + "}\n";
}

DocumentChange change_at(std::uint64_t version, TextChange change) {
    return DocumentChange{version, ChangeCause::edit, {std::move(change)}};
}

} // namespace

TEST(HighlightWorker, ParsesTheTextItIsGivenOffTheCallingThread) {
    Results results;
    const Text text = text_of(document_of(50));
    auto worker = HighlightWorker::create("json", text, 7, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(7));
    EXPECT_EQ(results.versions(), std::vector<std::uint64_t>{7});
    EXPECT_EQ(render(*results.last(), text), fresh_render(text));
}

TEST(HighlightWorker, ALanguageWithoutAGrammarAndATextThatIsTooLargeAreRefused) {
    Results results;
    const auto unsupported = HighlightWorker::create("cpp", Text{}, 1, results.deliver());
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().code, ErrorCode::unsupported);

    const Text big = text_of(std::string(max_highlighted_bytes + 1, ' '));
    const auto too_large = HighlightWorker::create("json", big, 1, results.deliver());
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().code, ErrorCode::limit_exceeded);
}

TEST(HighlightWorker, EditsInQuickSuccessionEndInTheSpansOfAFullParseOfTheFinalText) {
    Results results;
    TextBuffer buffer(text_of(document_of(300)));
    auto worker = HighlightWorker::create("json", buffer.snapshot(), 1, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(1));

    std::uint64_t version = 1;
    for (int i = 0; i < 200; ++i) {
        const std::size_t at = buffer.snapshot().line_start(1 + static_cast<std::size_t>(i)) + 3;
        const auto change = buffer.insert(at, i % 7 == 0 ? "\n" : "x");
        ASSERT_TRUE(change.has_value());
        (*worker)->edit(DocumentChange{++version, ChangeCause::edit, {*change}}, buffer.snapshot());
    }
    ASSERT_TRUE(results.wait_for_version(version));
    EXPECT_EQ(render(*results.last(), buffer.snapshot()), fresh_render(buffer.snapshot()));

    const auto delivered = results.versions();
    EXPECT_TRUE(std::ranges::is_sorted(delivered));
    EXPECT_EQ(delivered.back(), version);
}

TEST(HighlightWorker, EditsThatArriveDuringAParseAreCoalescedIntoOne) {
    Results results;
    results.hold_after(0);
    TextBuffer buffer(text_of(document_of(100)));
    auto worker = HighlightWorker::create("json", buffer.snapshot(), 1, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_count(1));

    // The worker is held in the callback of its first result; these queue up behind it.
    std::uint64_t version = 1;
    for (int i = 0; i < 50; ++i) {
        const auto change = buffer.insert(buffer.snapshot().line_start(2) + 3, "x");
        (*worker)->edit(DocumentChange{++version, ChangeCause::edit, {*change}}, buffer.snapshot());
    }
    results.release();
    ASSERT_TRUE(results.wait_for_version(version));

    EXPECT_EQ(results.versions(), (std::vector<std::uint64_t>{1, version}));
    EXPECT_EQ((*worker)->parses(), 2u);
    EXPECT_EQ((*worker)->full_parses(), 1u);
    EXPECT_EQ(render(*results.last(), buffer.snapshot()), fresh_render(buffer.snapshot()));
}

TEST(HighlightWorker, AnEditThatWasNotReportedIsFollowedByAParseFromScratch) {
    Results results;
    TextBuffer buffer(text_of(document_of(40)));
    auto worker = HighlightWorker::create("json", buffer.snapshot(), 1, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(1));

    const auto missed = buffer.insert(buffer.snapshot().line_start(3), "[]\n");
    ASSERT_TRUE(missed.has_value());
    const auto reported = buffer.insert(buffer.snapshot().line_start(5) + 2, "x");
    ASSERT_TRUE(reported.has_value());
    // Version 2 never reaches the worker.
    (*worker)->edit(change_at(3, *reported), buffer.snapshot());
    ASSERT_TRUE(results.wait_for_version(3));

    EXPECT_EQ((*worker)->full_parses(), 2u);
    EXPECT_EQ(render(*results.last(), buffer.snapshot()), fresh_render(buffer.snapshot()));
}

TEST(HighlightWorker, ReplacingTheTextParsesItFromScratch) {
    Results results;
    auto worker = HighlightWorker::create("json", text_of(document_of(10)), 1, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(1));

    const Text other = text_of("[true, false, null]\n");
    (*worker)->replace(other, 9);
    ASSERT_TRUE(results.wait_for_version(9));
    EXPECT_EQ(render(*results.last(), other), fresh_render(other));
}

TEST(HighlightWorker, AnEditWithNoChangesAtTheSameVersionIsIgnored) {
    Results results;
    const Text text = text_of(document_of(10));
    auto worker = HighlightWorker::create("json", text, 4, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(4));
    (*worker)->edit(DocumentChange{4, ChangeCause::edit, {}}, text);
    EXPECT_EQ((*worker)->parses(), 1u);
}

TEST(HighlightWorker, ADeliveredHighlighterIsNotAffectedByLaterEdits) {
    Results results;
    TextBuffer buffer(text_of(document_of(20)));
    auto worker = HighlightWorker::create("json", buffer.snapshot(), 1, results.deliver());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE(results.wait_for_version(1));
    const auto first = results.last();
    const Text first_text = buffer.snapshot();

    const auto change = buffer.insert(buffer.snapshot().line_start(2), "[1]\n");
    (*worker)->edit(change_at(2, *change), buffer.snapshot());
    ASSERT_TRUE(results.wait_for_version(2));
    EXPECT_EQ(render(*first, first_text), fresh_render(first_text));
}

TEST(HighlightWorker, DestroyingItDuringTheFirstParseEndsTheThreadAndDeliversNothingMore) {
    std::atomic<int> delivered = 0;
    const Text big = text_of(document_of(70000));
    ASSERT_LE(big.size(), max_highlighted_bytes);
    {
        auto worker = HighlightWorker::create(
            "json", big, 1,
            [&](std::uint64_t, std::shared_ptr<const Highlighter>) { ++delivered; });
        ASSERT_TRUE(worker.has_value());
    }
    const int after_destruction = delivered.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(delivered.load(), after_destruction);
}

TEST(HighlightWorker, DestroyingItWhileEditsAreQueuedLeavesNothingRunning) {
    TextBuffer buffer(text_of(document_of(3000)));
    std::atomic<int> delivered = 0;
    auto worker = HighlightWorker::create(
        "json", buffer.snapshot(), 1,
        [&](std::uint64_t, std::shared_ptr<const Highlighter>) { ++delivered; });
    ASSERT_TRUE(worker.has_value());
    std::uint64_t version = 1;
    for (int i = 0; i < 100; ++i) {
        const auto change = buffer.insert(buffer.snapshot().line_start(2) + 3, "x");
        (*worker)->edit(DocumentChange{++version, ChangeCause::edit, {*change}}, buffer.snapshot());
    }
    worker->reset();
    const int after_destruction = delivered.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(delivered.load(), after_destruction);
}

#else

TEST(HighlightWorker, NeedsTheTreeSitterSources) {
    GTEST_SKIP() << "built without .deps/tree-sitter; run scripts/fetch-tree-sitter.sh";
}

#endif
