// Model-based tests: random edits are applied to the rope-backed Text and to a plain std::string
// reference, and every observable result is compared.
#include "anvil/core/text.hpp"
#include "rope.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace anvil::core;

namespace {

// The reference implementation: naive, obviously correct, quadratic where convenient.
struct Reference {
    std::string text;

    static std::size_t char_length(unsigned char lead) {
        return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    }

    std::vector<std::size_t> boundaries() const {
        std::vector<std::size_t> result;
        for (std::size_t i = 0; i < text.size();
             i += char_length(static_cast<unsigned char>(text[i]))) {
            result.push_back(i);
        }
        result.push_back(text.size());
        return result;
    }

    std::vector<std::size_t> line_starts() const {
        std::vector<std::size_t> starts{0};
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                starts.push_back(i + 1);
            }
        }
        return starts;
    }

    std::size_t utf16_between(std::size_t from, std::size_t to) const {
        std::size_t units = 0;
        for (std::size_t i = from; i < to; i += char_length(static_cast<unsigned char>(text[i]))) {
            units += char_length(static_cast<unsigned char>(text[i])) == 4 ? 2 : 1;
        }
        return units;
    }

    Position position_at(std::size_t offset) const {
        const auto starts = line_starts();
        std::size_t line = 0;
        while (line + 1 < starts.size() && starts[line + 1] <= offset) {
            ++line;
        }
        return {line, utf16_between(starts[line], offset)};
    }

    std::size_t offset_at(Position position) const {
        const auto starts = line_starts();
        if (position.line >= starts.size()) {
            return text.size();
        }
        const std::size_t start = starts[position.line];
        std::size_t end =
            position.line + 1 < starts.size() ? starts[position.line + 1] - 1 : text.size();
        std::size_t offset = start;
        std::size_t units = 0;
        while (offset < end && units < position.column) {
            const std::size_t length = char_length(static_cast<unsigned char>(text[offset]));
            const std::size_t width = length == 4 ? 2 : 1;
            if (units + width > position.column) {
                break;
            }
            units += width;
            offset += length;
        }
        return offset;
    }
};

class RandomText {
public:
    explicit RandomText(unsigned seed) : random_(seed) {}

    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, bound - 1)(random_);
    }

    std::string make(std::size_t length) {
        static const char* pieces[] = {
            "a",    "b", "c", " ", "\n", "\n", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
            "\r\n", "\r"};
        std::string result;
        while (result.size() < length) {
            result += pieces[below(std::size(pieces))];
        }
        return result;
    }

private:
    std::mt19937 random_;
};

void expect_same(const Text& text, const Reference& reference, RandomText& random) {
    ASSERT_FALSE(text.check_invariants().has_value()) << *text.check_invariants();
    ASSERT_EQ(text.size(), reference.text.size());
    ASSERT_EQ(text.to_string(), reference.text);
    const auto starts = reference.line_starts();
    ASSERT_EQ(text.line_count(), starts.size());
    ASSERT_EQ(text.utf16_length(), reference.utf16_between(0, reference.text.size()));

    const auto boundaries = reference.boundaries();
    for (int i = 0; i < 25; ++i) {
        const std::size_t offset = boundaries[random.below(boundaries.size())];
        ASSERT_TRUE(text.is_char_boundary(offset));
        const Position position = reference.position_at(offset);
        ASSERT_EQ(text.position_at(offset), position) << "offset " << offset;
        ASSERT_EQ(text.offset_at(position), offset)
            << "position " << position.line << ":" << position.column;
        ASSERT_EQ(text.utf16_offset(offset), reference.utf16_between(0, offset));
    }
    for (int i = 0; i < 10; ++i) {
        const Position probe{random.below(starts.size() + 2), random.below(40)};
        ASSERT_EQ(text.offset_at(probe), reference.offset_at(probe))
            << "probe " << probe.line << ":" << probe.column;
    }
    for (int i = 0; i < 5; ++i) {
        const std::size_t line = random.below(starts.size());
        const std::size_t end =
            line + 1 < starts.size() ? starts[line + 1] - 1 : reference.text.size();
        ASSERT_EQ(text.line(line), reference.text.substr(starts[line], end - starts[line]));
        ASSERT_EQ(text.line_start(line), starts[line]);
        ASSERT_EQ(text.line_end(line), end);
    }
    for (int i = 0; i < 5; ++i) {
        const std::size_t offset = random.below(reference.text.size() + 1);
        const std::size_t length = random.below(3000);
        ASSERT_EQ(text.slice(offset, length),
                  reference.text.substr(std::min(offset, reference.text.size()), length));
    }
}

} // namespace

TEST(TextModel, RandomEditsMatchTheReference) {
    for (unsigned seed = 1; seed <= 6; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        RandomText random(seed);

        Reference reference;
        reference.text = normalize_line_endings(random.make(random.below(40000)));
        Text text = *Text::from_utf8(reference.text);
        expect_same(text, reference, random);

        for (int step = 0; step < 300; ++step) {
            const auto boundaries = reference.boundaries();
            std::size_t start = boundaries[random.below(boundaries.size())];
            std::size_t end = boundaries[random.below(boundaries.size())];
            if (start > end) {
                std::swap(start, end);
            }
            // Mostly small edits, sometimes large insertions or deletions.
            const std::size_t kind = random.below(10);
            if (kind == 0) {
                end = std::min(end, start + random.below(100));
                while (end < reference.text.size() && !text.is_char_boundary(end)) {
                    ++end;
                }
            } else if (kind > 2) {
                std::size_t limit = start + random.below(20);
                while (end > limit) {
                    --end;
                }
                while (end > start && !text.is_char_boundary(end)) {
                    --end;
                }
                end = std::max(end, start);
            }
            const std::string replacement =
                random.make(kind == 1 ? random.below(6000) : random.below(kind > 5 ? 3 : 30));

            auto updated = text.replaced(start, end, replacement);
            ASSERT_TRUE(updated.has_value()) << updated.error().message << " at step " << step;
            reference.text.replace(start, end - start, normalize_line_endings(replacement));
            text = std::move(*updated);
            ASSERT_NO_FATAL_FAILURE(expect_same(text, reference, random)) << "step " << step;
        }
    }
}

TEST(TextModel, EditsFarFromTheCurrentSizeKeepTheTreeBalanced) {
    RandomText random(99);
    Text text;
    std::string reference;
    // Repeated appends, prepends, then deletes from the middle.
    for (int i = 0; i < 3000; ++i) {
        const std::string piece = random.make(1 + random.below(8));
        const std::string normalized = normalize_line_endings(piece);
        if (i % 2 == 0) {
            text = *text.replaced(text.size(), text.size(), piece);
            reference += normalized;
        } else {
            text = *text.replaced(0, 0, piece);
            reference = normalized + reference;
        }
    }
    ASSERT_FALSE(text.check_invariants().has_value()) << *text.check_invariants();
    ASSERT_EQ(text.to_string(), reference);
    const std::size_t middle = reference.size() / 2;
    std::size_t from = middle;
    while (!text.is_char_boundary(from)) {
        ++from;
    }
    text = *text.replaced(from, text.size(), "");
    reference.resize(from);
    ASSERT_FALSE(text.check_invariants().has_value());
    EXPECT_EQ(text.to_string(), reference);
}

TEST(Rope, SplitAndConcatRoundTripAtEveryTestedOffset) {
    RandomText random(7);
    const std::string source = normalize_line_endings(random.make(120000));
    const auto tree = rope::build(source);
    ASSERT_FALSE(rope::check_invariants(tree).has_value());

    Reference boundaries_of;
    boundaries_of.text = source;
    const auto boundaries = boundaries_of.boundaries();
    for (int i = 0; i < 300; ++i) {
        const std::size_t offset = boundaries[random.below(boundaries.size())];
        auto [left, right] = rope::split(tree, offset);
        ASSERT_FALSE(rope::check_invariants(left).has_value());
        ASSERT_FALSE(rope::check_invariants(right).has_value());
        ASSERT_EQ(rope::byte_size(left), offset);
        // Re-joining trees of very different heights must stay balanced.
        const auto joined = rope::concat(left, right);
        ASSERT_FALSE(rope::check_invariants(joined).has_value()) << *rope::check_invariants(joined);
        std::string text;
        rope::append_range(joined, 0, source.size(), text);
        ASSERT_EQ(text, source);
    }
}

TEST(Rope, RepeatedSmallEditsDoNotFragmentIntoTinyChunks) {
    RandomText random(5);
    Text text = *Text::from_utf8(std::string(200000, 'x'));
    std::string reference(200000, 'x');
    for (int i = 0; i < 20000; ++i) {
        const std::size_t offset = random.below(reference.size());
        text = *text.replaced(offset, offset, "y");
        reference.insert(offset, "y");
    }
    ASSERT_EQ(text.to_string(), reference);
    // Chunks hold between half and all of the limit once merged; a trail of one-byte chunks would
    // push the count towards the size of the text.
    EXPECT_LT(text.chunk_count(), reference.size() / 400);
    EXPECT_FALSE(text.check_invariants().has_value());
}
