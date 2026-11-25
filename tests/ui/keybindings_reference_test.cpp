#include "anvil/ui/keybindings_reference.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace anvil;

core::Keybinding bind(const char* key, const char* command) {
    core::Keybinding binding;
    binding.chord = *core::parse_key_chord(key);
    binding.command = command;
    return binding;
}

std::string reference(const ui::CommandTitles& titles, std::vector<core::Keybinding> bindings,
                      core::KeyPlatform platform = core::KeyPlatform::linux) {
    return ui::keybindings_reference(titles, core::KeybindingTable(std::move(bindings)), platform);
}

constexpr const char* heading = "Keyboard Shortcuts Reference\n\n";

TEST(KeybindingsReferenceTest, ATableWithoutBindingsHasTheHeadingOnly) {
    EXPECT_EQ(reference({}, {}), heading);
}

TEST(KeybindingsReferenceTest, ACommandIsALineOfItsTitleATabAndItsKeys) {
    EXPECT_EQ(reference({{"save", "Save"}}, {bind("ctrl+s", "save")}),
              std::string(heading) + "Save\tCtrl+S\n");
}

TEST(KeybindingsReferenceTest, TheKeysOfACommandAreSeparatedAndAChordKeepsItsSteps) {
    const std::string text =
        reference({{"open", "Open"}}, {bind("ctrl+k ctrl+s", "open"), bind("f4", "open")});
    EXPECT_EQ(text, std::string(heading) + "Open\tF4, Ctrl+K Ctrl+S\n");
}

TEST(KeybindingsReferenceTest, TheSameKeyUnderTwoConditionsIsListedOnce) {
    auto first = bind("ctrl+j", "toggle");
    auto second = bind("ctrl+j", "toggle");
    second.when = *core::WhenExpression::parse("editorTextFocus");
    EXPECT_EQ(reference({{"toggle", "Toggle"}}, {first, second}),
              std::string(heading) + "Toggle\tCtrl+J\n");
}

TEST(KeybindingsReferenceTest, ACommandWithoutATitleIsListedByItsId) {
    const std::string text =
        reference({{"empty", ""}}, {bind("f1", "empty"), bind("f2", "other.id")});
    EXPECT_EQ(text, std::string(heading) + "empty\tF1\nother.id\tF2\n");
}

TEST(KeybindingsReferenceTest, LinesAreSortedByTitleWithoutRegardToCaseThenById) {
    const std::string text = reference(
        {{"a", "banana"}, {"b", "Apple"}, {"c", "cherry"}, {"d", "apple"}, {"e", "Zed"}},
        {bind("f1", "a"), bind("f2", "b"), bind("f3", "c"), bind("f4", "d"), bind("f5", "e")});
    EXPECT_EQ(text,
              std::string(heading) + "Apple\tF2\napple\tF4\nbanana\tF1\ncherry\tF3\nZed\tF5\n");
}

TEST(KeybindingsReferenceTest, ARemovedBindingIsNotListedAndACommandWithoutKeysIsNot) {
    auto removal = bind("ctrl+s", "-save");
    EXPECT_EQ(reference({{"save", "Save"}, {"quit", "Quit"}, {"find", "Find"}},
                        {bind("ctrl+s", "save"), removal, bind("ctrl+f", "find")}),
              std::string(heading) + "Find\tCtrl+F\n");
}

TEST(KeybindingsReferenceTest, TitlesThatDifferOnlyInCaseAreOrderedByTheirIds) {
    EXPECT_EQ(reference({{"z", "Go"}, {"a", "go"}, {"m", "GO"}},
                        {bind("f1", "z"), bind("f2", "a"), bind("f3", "m")}),
              std::string(heading) + "go\tF2\nGO\tF3\nGo\tF1\n");
}

TEST(KeybindingsReferenceTest, ABindingToNothingIsNotListed) {
    EXPECT_EQ(reference({}, {bind("f1", "")}), heading);
}

TEST(KeybindingsReferenceTest, TheKeysAreSpelledForThePlatform) {
    const std::vector<core::Keybinding> bindings = {bind("ctrl+shift+p", "palette")};
    const ui::CommandTitles titles = {{"palette", "Palette"}};
    EXPECT_EQ(reference(titles, bindings, core::KeyPlatform::linux),
              std::string(heading) + "Palette\tCtrl+Shift+P\n");
    EXPECT_EQ(reference(titles, bindings, core::KeyPlatform::macos),
              std::string(heading) + "Palette\t⌃⇧P\n");
}

} // namespace
