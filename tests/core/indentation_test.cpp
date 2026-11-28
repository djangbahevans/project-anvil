#include "anvil/core/document.hpp"
#include "anvil/core/indentation.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace anvil::core;
using nlohmann::json;

namespace {

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text += (i ? "\n" : "") + lines[i];
    }
    return text;
}

Document document_of(const std::string& text) {
    return Document(*Text::from_utf8(text));
}

} // namespace

TEST(GuessIndentation, TheRecordedCasesGiveVsCodesGuess) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/indentation_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);

    int failures = 0;
    for (const json& entry : recorded["cases"]) {
        const std::string text = join(entry["lines"].get<std::vector<std::string>>());
        const Indentation guess =
            guess_indentation(*Text::from_utf8(text), {entry["tabSize"].get<std::size_t>(),
                                                       entry["insertSpaces"].get<bool>()});
        const Indentation expected{entry["expectedTabSize"].get<std::size_t>(),
                                   entry["expectedInsertSpaces"].get<bool>()};
        if (guess != expected && ++failures <= 8) {
            ADD_FAILURE() << entry.dump() << "\ngives tabSize " << guess.tab_size
                          << " insertSpaces " << guess.insert_spaces;
        }
    }
    EXPECT_EQ(failures, 0);
    EXPECT_GE(recorded["cases"].size(), 2000u);
}

TEST(GuessIndentation, OnlyTheFirstTenThousandLinesAreLookedAt) {
    std::string text;
    for (int line = 0; line < 10000; ++line) {
        text += "x\n";
    }
    text += "a\n  b\n  c";
    EXPECT_EQ(guess_indentation(*Text::from_utf8(text), {8, false}), (Indentation{8, false}));
    EXPECT_EQ(guess_indentation(*Text::from_utf8(text.substr(6)), {8, false}),
              (Indentation{2, true}));
}

TEST(DocumentIndentation, IsWhatTheTextShowsOrTheDefaultsWithoutDetection) {
    Document document = document_of("a\n  b\n  c");
    EXPECT_EQ(document.resolve_indentation({4, false}, true), (Indentation{2, true}));
    EXPECT_EQ(document.resolve_indentation({4, false}, false), (Indentation{4, false}));
    EXPECT_EQ(document.resolve_indentation({4, false}, true), (Indentation{2, true}));
}

TEST(DocumentIndentation, DoesNotFollowEditsUntilItIsAskedSomethingElse) {
    Document document = document_of("");
    EXPECT_EQ(document.resolve_indentation({4, true}, true), (Indentation{4, true}));
    const std::vector<Edit> type = {{0, 0, "a\n  b\n  c"}};
    ASSERT_TRUE(document.apply_edits(type).has_value());
    EXPECT_EQ(document.resolve_indentation({4, true}, true), (Indentation{4, true}));
    EXPECT_EQ(document.resolve_indentation({8, true}, true), (Indentation{2, true}));
}

TEST(DocumentIndentation, IsGuessedAgainAfterItIsForgotten) {
    Document document = document_of("");
    EXPECT_EQ(document.resolve_indentation({4, true}, true), (Indentation{4, true}));
    const std::vector<Edit> content = {{0, 0, "a\n\tb\n\tc"}};
    ASSERT_TRUE(document.apply_edits(content).has_value());
    document.forget_indentation();
    EXPECT_EQ(document.resolve_indentation({4, true}, true), (Indentation{4, false}));
}
