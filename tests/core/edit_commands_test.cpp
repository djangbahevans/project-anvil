// Tests of Tab, Indent Line, Outdent Line, Delete Word Left and Delete Word Right: the cases
// recorded from VS Code's own commands (tests/core/data/edit_cases.json,
// `node scripts/generate-edit-cases.mjs`) and the behavior that a text and a selection do not show.
#include "anvil/core/editing.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace anvil::core;
using json = nlohmann::json;

namespace {

// [anchor line, anchor column, active line, active column], counted from 1, with the columns in
// UTF-16 units as VS Code counts them.
using Cursor = std::array<std::size_t, 4>;

std::size_t offset_of(const Text& text, std::size_t line, std::size_t column) {
    const std::size_t start = text.line_start(line - 1);
    std::size_t offset = start;
    while (text.utf16_offset(offset) - text.utf16_offset(start) < column - 1) {
        do {
            ++offset;
        } while (offset < text.size() && !text.is_char_boundary(offset));
    }
    return offset;
}

Cursor cursor_of(const Text& text, const Selection& selection) {
    const auto position = [&](std::size_t offset) {
        const std::size_t line = text.position_at(offset).line;
        return std::pair(line + 1,
                         text.utf16_offset(offset) - text.utf16_offset(text.line_start(line)) + 1);
    };
    const auto [anchor_line, anchor_column] = position(selection.anchor);
    const auto [active_line, active_column] = position(selection.active);
    return {anchor_line, anchor_column, active_line, active_column};
}

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text += (i ? "\n" : "") + lines[i];
    }
    return text;
}

std::vector<Cursor> sorted(std::vector<Cursor> cursors) {
    std::ranges::sort(cursors, [](const Cursor& a, const Cursor& b) {
        const auto key = [](const Cursor& c) {
            return std::pair(std::min(std::pair(c[0], c[1]), std::pair(c[2], c[3])),
                             std::max(std::pair(c[0], c[1]), std::pair(c[2], c[3])));
        };
        return key(a) < key(b);
    });
    return cursors;
}

EditOutcome run(EditController& controller, const std::string& command, bool insert_spaces) {
    if (command == "tab") {
        return controller.tab(insert_spaces);
    }
    if (command == "indent") {
        return controller.indent_lines(insert_spaces);
    }
    if (command == "outdent") {
        return controller.outdent_lines(insert_spaces);
    }
    return command == "deleteWordLeft" ? controller.delete_word_left()
                                       : controller.delete_word_right();
}

std::string describe(const std::string& text, const std::vector<Cursor>& cursors) {
    return "  " + json(text).dump() + " " + json(cursors).dump() + "\n";
}

Document document_with(const std::string& text, std::vector<Selection> selections) {
    Document document(*Text::from_utf8(text));
    EXPECT_TRUE(document.set_selections(SelectionSet::from(std::move(selections))).has_value());
    return document;
}

} // namespace

TEST(EditCommands, TheRecordedCasesGiveVsCodesTextAndSelections) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/edit_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);

    int failures = 0;
    int number = 0;
    for (const json& entry : recorded["cases"]) {
        ++number;
        const std::string command = entry["command"];
        const std::string text = join(entry["lines"].get<std::vector<std::string>>());
        const std::string expected_text =
            join(entry["expectedLines"].get<std::vector<std::string>>());
        const auto cursors = entry["selections"].get<std::vector<Cursor>>();
        const auto expected = entry["expectedSelections"].get<std::vector<Cursor>>();

        Document document(*Text::from_utf8(text));
        // VS Code lists the primary cursor first; the selections of the document are in the order
        // of the text.
        const std::vector<Cursor> in_text_order = sorted(cursors);
        std::vector<Selection> selections;
        for (const Cursor& c : in_text_order) {
            selections.push_back(
                {offset_of(document.text(), c[0], c[1]), offset_of(document.text(), c[2], c[3])});
        }
        const std::size_t primary = static_cast<std::size_t>(
            std::ranges::find(in_text_order, cursors.front()) - in_text_order.begin());
        ASSERT_TRUE(document.set_selections(SelectionSet::from(selections, primary)).has_value());

        EditController controller(document);
        controller.set_tab_size(entry["tabSize"].get<std::size_t>());
        run(controller, command, entry["insertSpaces"].get<bool>());

        std::vector<Cursor> got;
        for (const Selection& selection : document.selections().all()) {
            got.push_back(cursor_of(document.text(), selection));
        }
        const Cursor got_primary = cursor_of(document.text(), document.selections().primary());
        if (document.text().to_string() != expected_text || sorted(got) != sorted(expected) ||
            got_primary != expected.front()) {
            if (++failures <= 8) {
                ADD_FAILURE() << "case " << number << ": " << command
                              << " tabSize=" << entry["tabSize"]
                              << " insertSpaces=" << entry["insertSpaces"] << "\nfrom\n"
                              << describe(text, cursors) << "VS Code\n"
                              << describe(expected_text, expected) << "Anvil\n"
                              << describe(document.text().to_string(), got);
            }
        }
    }
    EXPECT_EQ(failures, 0) << failures << " of " << number << " recorded cases differ";
    EXPECT_GE(number, 1000);
}

TEST(EditCommands, EachCommandIsOneUndoStepThatTypingDoesNotJoin) {
    const std::string text = "one two\n    three four";
    for (const char* command : {"tab", "indent", "outdent", "deleteWordLeft", "deleteWordRight"}) {
        SCOPED_TRACE(command);
        const std::string start = "    " + text;
        Document document = document_with(start, {Selection{start.size() - 5, start.size() - 5}});
        EditController controller(document);
        controller.type("x");
        const std::string typed = document.text().to_string();
        ASSERT_TRUE(run(controller, command, true).change.has_value());
        const std::string after = document.text().to_string();
        ASSERT_NE(after, typed);
        controller.type("y");
        controller.undo();
        EXPECT_EQ(document.text().to_string(), after);
        controller.undo();
        EXPECT_EQ(document.text().to_string(), typed);
        controller.undo();
        EXPECT_EQ(document.text().to_string(), start);
    }
}

TEST(EditCommands, ACommandThatHasNothingToDoChangesNothing) {
    Document document = document_with("text", {Selection::caret(0)});
    EditController controller(document);
    EXPECT_FALSE(controller.delete_word_left().changed_anything());
    EXPECT_FALSE(controller.outdent_lines(true).changed_anything());
    ASSERT_TRUE(document.set_selections(SelectionSet::from({Selection::caret(4)})).has_value());
    EXPECT_FALSE(controller.delete_word_right().changed_anything());
    EXPECT_FALSE(document.is_dirty());
}
