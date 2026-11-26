#include "anvil/core/line_breaks.hpp"
#include "anvil/core/text.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace anvil::core {
namespace {

TEST(LineBreaks, AgreeWithVsCodeOnEveryRecordedLine) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/line_break_cases.json");
    const nlohmann::json data = nlohmann::json::parse(file);
    ASSERT_GT(data.at("cases").size(), 1500u);
    std::size_t wrapped = 0;
    for (const nlohmann::json& c : data.at("cases")) {
        SCOPED_TRACE(c.dump());
        const std::string line = c.at("line");
        const Text text = Text::from_utf8(line).value();
        LineBreaks expected;
        for (const std::size_t units : c.at("starts")) {
            expected.starts.push_back(text.offset_at({0, units}));
        }
        expected.indent = c.at("indent");
        const WrapOptions options{c.at("tabSize"), c.at("column"), c.at("fullWidth")};
        const LineBreaks breaks = break_line(line, options);
        ASSERT_EQ(breaks.starts, expected.starts);
        ASSERT_EQ(breaks.indent, expected.indent);
        ASSERT_EQ(count_rows(line, options), expected.starts.size() + 1);
        wrapped += expected.starts.empty() ? 0 : 1;
    }
    EXPECT_GT(wrapped, 1000u);
}

TEST(LineBreaks, ALineThatIsShortInBytesWrapsWhenItsCharactersAreWide) {
    const WrapOptions options{8, 10, 3};
    EXPECT_EQ(count_rows("abcdefghij", options), 1u) << "exactly the column";
    EXPECT_EQ(count_rows("abcdefghijk", options), 2u);
    EXPECT_EQ(count_rows("ab\tcd", options), 1u) << "the tab ends at column 8";
    EXPECT_EQ(count_rows("ab\tcde", options), 2u);
    // A control character is drawn as wide as a full-width one: 3 columns in 1 byte.
    EXPECT_EQ(count_rows("abcdefg\x01", options), 1u);
    EXPECT_EQ(count_rows("abcdefgh\x01", options), 2u);
    // A full-width character that takes more columns than it has bytes.
    EXPECT_EQ(count_rows("\xE4\xB8\xAD\xE4\xB8\xAD", {4, 6, 3}), 1u);
    EXPECT_EQ(count_rows("\xE4\xB8\xAD\xE4\xB8\xAD", {4, 6, 4}), 2u);
}

} // namespace
} // namespace anvil::core
