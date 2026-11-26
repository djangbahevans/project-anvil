#include "anvil/core/editing.hpp"
#include "anvil/core/visible_column.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace anvil::core {
namespace {

nlohmann::json recorded() {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/cursor_column_cases.json");
    return nlohmann::json::parse(file);
}

// The byte offset of a UTF-16 column of a line, and the other way round.
std::size_t bytes_of(const std::string& line, std::size_t column) {
    return Text::from_utf8(line).value().offset_at({0, column});
}

std::size_t units_of(const std::string& line, std::size_t offset) {
    return Text::from_utf8(line).value().position_at(offset).column;
}

TEST(VisibleColumn, AgreesWithVsCodeOnEveryOffsetAndColumn) {
    const nlohmann::json data = recorded();
    ASSERT_GT(data.at("columns").size(), 300u);
    for (const nlohmann::json& c : data.at("columns")) {
        const std::string line = c.at("line");
        const std::size_t tab_size = c.at("tabSize");
        SCOPED_TRACE(c.dump());
        const nlohmann::json& columns = c.at("columns");
        for (std::size_t i = 0; i < columns.size(); ++i) {
            EXPECT_EQ(visible_column(line, bytes_of(line, columns[i]), tab_size),
                      c.at("visible")[i].get<std::size_t>())
                << "UTF-16 column " << columns[i];
        }
        const nlohmann::json& targets = c.at("targets");
        for (std::size_t target = 0; target < targets.size(); ++target) {
            EXPECT_EQ(units_of(line, offset_at_visible_column(line, target, tab_size)),
                      targets[target].get<std::size_t>())
                << "visible column " << target;
        }
    }
}

std::string joined(const nlohmann::json& lines) {
    std::string text;
    for (const nlohmann::json& line : lines) {
        if (&line != &lines.front()) {
            text += '\n';
        }
        text += line.get<std::string>();
    }
    return text;
}

TEST(VisibleColumn, TheArrowKeysMoveAsInVsCode) {
    const nlohmann::json data = recorded();
    std::size_t cases = 0;
    for (const nlohmann::json& c : data.at("moves")) {
        if (!c.at("arrows").get<bool>()) {
            continue;
        }
        ++cases;
        SCOPED_TRACE(c.dump());
        Document document(Text::from_utf8(joined(c.at("lines"))).value());
        EditController controller(document);
        controller.set_tab_size(c.at("tabSize"));
        controller.set_caret(document.text().offset_at({c.at("start")[0], c.at("start")[1]}));
        for (const nlohmann::json& step : c.at("steps")) {
            controller.move(step[0].get<int>() != 0 ? Motion::down : Motion::up);
            const Position position =
                document.text().position_at(document.selections().primary().active);
            ASSERT_EQ(position, (Position{step[1], step[2]}));
        }
    }
    EXPECT_GT(cases, 100u);
}

TEST(VisibleColumn, CursorsAreAddedAboveAndBelowAsInVsCode) {
    const nlohmann::json data = recorded();
    std::size_t cases = 0;
    for (const nlohmann::json& c : data.at("moves")) {
        if (c.at("arrows").get<bool>()) {
            continue;
        }
        ++cases;
        SCOPED_TRACE(c.dump());
        Document document(Text::from_utf8(joined(c.at("lines"))).value());
        EditController controller(document);
        controller.set_tab_size(c.at("tabSize"));
        const std::size_t start = document.text().offset_at({c.at("start")[0], c.at("start")[1]});
        controller.set_caret(start);
        std::vector<std::size_t> expected{start};
        for (const nlohmann::json& step : c.at("steps")) {
            controller.add_cursors_vertically(step[0].get<int>() != 0);
            expected.push_back(document.text().offset_at({step[1], step[2]}));
            std::vector<std::size_t> carets;
            for (const Selection& selection : document.selections().all()) {
                carets.push_back(selection.active);
            }
            std::vector<std::size_t> wanted = expected;
            std::ranges::sort(wanted);
            wanted.erase(std::ranges::unique(wanted).begin(), wanted.end());
            std::ranges::sort(carets);
            ASSERT_EQ(carets, wanted);
        }
    }
    EXPECT_GT(cases, 100u);
}

} // namespace
} // namespace anvil::core
