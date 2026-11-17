// Model-based test of Document: random edit batches, undo, redo, selections and saves are mirrored
// by a naive reference, and every change list is replayed on the previous text.
#include "anvil/core/document.hpp"
#include "document_support.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace anvil::core;
using anvil::core::testing::replay_changes;

namespace {

class Random {
public:
    explicit Random(unsigned seed) : engine_(seed) {}

    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, bound - 1)(engine_);
    }

    std::string text(std::size_t length) {
        static const char* pieces[] = {
            "a", "b", " ", "\n", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80"};
        std::string result;
        while (result.size() < length) {
            result += pieces[below(std::size(pieces))];
        }
        return result;
    }

private:
    std::mt19937 engine_;
};

std::vector<std::size_t> boundaries(const std::string& text) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) {
            result.push_back(i);
        }
    }
    return result;
}

struct ReferenceStep {
    std::uint64_t id;
    std::uint64_t group;
    std::string before;
    std::string after;
};

// The semantics of Document, written the slow and obvious way.
struct Reference {
    std::string text;
    std::vector<ReferenceStep> undo;
    std::vector<ReferenceStep> redo;
    std::uint64_t next_id = 1;
    std::uint64_t next_group = 1;
    std::uint64_t saved_id = 0;
    bool sealed = false;

    std::uint64_t current_id() const {
        return undo.empty() ? 0 : undo.back().id;
    }

    void edit(const std::vector<Edit>& edits, bool join) {
        std::vector<Edit> sorted = edits;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const Edit& a, const Edit& b) { return a.start < b.start; });
        std::string after = text;
        for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
            after.replace(it->start, it->end - it->start, normalize_line_endings(it->text));
        }
        const bool joined = join && !undo.empty() && !sealed;
        undo.push_back({next_id++, joined ? undo.back().group : next_group++, text, after});
        redo.clear();
        sealed = false;
        text = after;
    }

    void undo_group() {
        const std::uint64_t group = undo.back().group;
        while (!undo.empty() && undo.back().group == group) {
            text = undo.back().before;
            redo.push_back(undo.back());
            undo.pop_back();
        }
    }

    void redo_group() {
        const std::uint64_t group = redo.back().group;
        while (!redo.empty() && redo.back().group == group) {
            text = redo.back().after;
            undo.push_back(redo.back());
            redo.pop_back();
        }
    }

    void save() {
        saved_id = current_id();
        sealed = true;
    }
};

// Random non-overlapping edits at character boundaries of `text`.
std::vector<Edit> random_edits(Random& random, const std::string& text) {
    const auto cuts = boundaries(text);
    std::vector<std::size_t> points;
    const std::size_t count = 1 + random.below(6);
    for (std::size_t i = 0; i < 2 * count; ++i) {
        points.push_back(cuts[random.below(cuts.size())]);
    }
    std::sort(points.begin(), points.end());
    std::vector<Edit> edits;
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        // Keep replaced ranges short so documents do not collapse to nothing: the end is the
        // last character boundary not beyond the limit.
        const std::size_t limit = std::min(points[i + 1], points[i] + random.below(40));
        const std::size_t end = *(std::upper_bound(cuts.begin(), cuts.end(), limit) - 1);
        edits.push_back(
            {points[i], end, random.text(random.below(random.below(20) == 0 ? 3000 : 12))});
    }
    // Present the edits in a scrambled order, which Document must accept.
    for (std::size_t i = edits.size(); i > 1; --i) {
        std::swap(edits[i - 1], edits[random.below(i)]);
    }
    return edits;
}

// Edits are valid only if no replaced ranges overlap; scrambling and clipping above can break
// that rarely, so overlapping batches are skipped.
bool overlaps(std::vector<Edit> edits) {
    std::stable_sort(edits.begin(), edits.end(),
                     [](const Edit& a, const Edit& b) { return a.start < b.start; });
    for (std::size_t i = 1; i < edits.size(); ++i) {
        if (edits[i - 1].end > edits[i].start) {
            return true;
        }
    }
    return false;
}

void expect_matches(const Document& document, const Reference& reference) {
    ASSERT_EQ(document.text().to_string(), reference.text);
    ASSERT_FALSE(document.text().check_invariants().has_value())
        << *document.text().check_invariants();
    ASSERT_EQ(document.can_undo(), !reference.undo.empty());
    ASSERT_EQ(document.can_redo(), !reference.redo.empty());
    ASSERT_EQ(document.is_dirty(), reference.current_id() != reference.saved_id);
    // Selections must stay valid for the current text.
    Document copy = document;
    ASSERT_TRUE(copy.set_selections(document.selections()).has_value());
}

} // namespace

TEST(DocumentModel, RandomOperationsMatchTheReference) {
    for (unsigned seed = 1; seed <= 8; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        Random random(seed);

        Reference reference;
        reference.text = normalize_line_endings(random.text(random.below(6000)));
        Document document(*Text::from_utf8(reference.text));
        ASSERT_NO_FATAL_FAILURE(expect_matches(document, reference));

        for (int step = 0; step < 250; ++step) {
            SCOPED_TRACE("step " + std::to_string(step));
            const Text before = document.text();
            const std::uint64_t version_before = document.version();
            std::optional<DocumentChange> change;

            switch (random.below(10)) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5: {
                auto edits = random_edits(random, reference.text);
                if (overlaps(edits)) {
                    continue;
                }
                const bool join = random.below(3) == 0;
                auto result = document.apply_edits(edits, EditOptions{.join_previous = join});
                ASSERT_TRUE(result.has_value()) << result.error().message;
                reference.edit(edits, join);
                change = std::move(*result);
                break;
            }
            case 6:
            case 7: {
                auto result = document.undo();
                ASSERT_TRUE(result.has_value());
                ASSERT_EQ(result->has_value(), !reference.undo.empty());
                if (result->has_value()) {
                    reference.undo_group();
                    change = std::move(**result);
                }
                break;
            }
            case 8: {
                auto result = document.redo();
                ASSERT_TRUE(result.has_value());
                ASSERT_EQ(result->has_value(), !reference.redo.empty());
                if (result->has_value()) {
                    reference.redo_group();
                    change = std::move(**result);
                }
                break;
            }
            default: {
                document.mark_saved();
                reference.save();
                const auto cuts = boundaries(reference.text);
                const std::size_t a = cuts[random.below(cuts.size())];
                const std::size_t b = cuts[random.below(cuts.size())];
                ASSERT_TRUE(document.set_selections(SelectionSet::from({{a, b}})).has_value());
                break;
            }
            }

            ASSERT_NO_FATAL_FAILURE(expect_matches(document, reference));
            if (change) {
                ASSERT_EQ(document.version(), version_before + 1);
                ASSERT_EQ(change->version, document.version());
                ASSERT_EQ(replay_changes(before, change->changes).to_string(), reference.text);
            }
        }
    }
}
