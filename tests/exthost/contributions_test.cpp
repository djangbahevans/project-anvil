#include "anvil/exthost/contributions.hpp"
#include "anvil/exthost/init_data.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace anvil::exthost;
using anvil::core::KeyPlatform;
using anvil::core::LogLevel;
using nlohmann::json;

namespace {

class ContributionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "anvil-contributions-test";
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        logger_ = std::make_shared<anvil::core::RecordingLogger>();
    }
    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path extension(const std::string& directory, const std::string& name,
                                    const json& contributes,
                                    const std::optional<json>& nls = std::nullopt) {
        const auto dir = root_ / directory;
        std::filesystem::create_directories(dir);
        json manifest = {{"name", name}, {"publisher", "acme"}, {"contributes", contributes}};
        std::ofstream(dir / "package.json") << manifest.dump();
        if (nls) {
            std::ofstream(dir / "package.nls.json") << nls->dump();
        }
        return dir;
    }

    Contributions read(std::vector<std::filesystem::path> directories) {
        return read_contributions(directories, *logger_);
    }

    std::filesystem::path root_;
    std::shared_ptr<anvil::core::RecordingLogger> logger_;
};

} // namespace

TEST_F(ContributionsTest, CommandsCarryTheirTitleCategoryAndEnablement) {
    const auto dir = extension("a", "one",
                               {{"commands",
                                 {{{"command", "one.plain"}, {"title", "Plain"}},
                                  {{"command", "one.full"},
                                   {"title", "Full"},
                                   {"category", "Group"},
                                   {"enablement", "editorTextFocus"}}}}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.commands.size(), 2U);
    EXPECT_EQ(contributions.commands[0].command, "one.plain");
    EXPECT_EQ(contributions.commands[0].extension, "acme.one");
    EXPECT_EQ(contributions.commands[0].label(), "Plain");
    EXPECT_TRUE(contributions.commands[0].enablement.empty());
    EXPECT_EQ(contributions.commands[1].label(), "Group: Full");
    EXPECT_EQ(contributions.commands[1].enablement, "editorTextFocus");
    EXPECT_FALSE(logger_->contains(LogLevel::warning, "", "extensions"));
}

TEST_F(ContributionsTest, ATitleCanBeAnObjectWithTheTranslatedValue) {
    const auto dir = extension("a", "one",
                               {{"commands",
                                 {{{"command", "one.a"},
                                   {"title", {{"value", "Translated"}, {"original", "Original"}}},
                                   {"category", {{"value", "Cat"}, {"original", "Category"}}}}}}});
    EXPECT_EQ(read({dir}).commands.at(0).label(), "Cat: Translated");
}

TEST_F(ContributionsTest, PlaceholdersAreFilledFromThePackageNlsFile) {
    const auto dir = extension(
        "a", "one",
        {{"commands",
          {{{"command", "one.a"}, {"title", "%one.a.title%"}, {"category", "%one.category%"}},
           {{"command", "one.b"}, {"title", "%one.b.title%"}},
           {{"command", "one.c"}, {"title", "%missing%"}}}}},
        json{{"one.a.title", "Hello"},
             {"one.category", {{"message", "Greetings"}}},
             {"one.b.title", "Bye"}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.commands.size(), 3U);
    EXPECT_EQ(contributions.commands[0].label(), "Greetings: Hello");
    EXPECT_EQ(contributions.commands[1].label(), "Bye");
    EXPECT_EQ(contributions.commands[2].label(), "%missing%");
}

TEST_F(ContributionsTest, KeybindingsKeepEveryPlatformForm) {
    const auto dir = extension("a", "one",
                               {{"keybindings",
                                 {{{"command", "one.a"},
                                   {"key", "ctrl+1"},
                                   {"mac", "cmd+1"},
                                   {"linux", "ctrl+alt+1"},
                                   {"win", "ctrl+shift+1"},
                                   {"when", "editorTextFocus"},
                                   {"args", {{"x", 1}}}}}}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.keybindings.size(), 1U);
    const ContributedKeybinding& binding = contributions.keybindings[0];
    EXPECT_EQ(binding.command, "one.a");
    EXPECT_EQ(binding.key, "ctrl+1");
    EXPECT_EQ(binding.mac, "cmd+1");
    EXPECT_EQ(binding.linux, "ctrl+alt+1");
    EXPECT_EQ(binding.win, "ctrl+shift+1");
    EXPECT_EQ(binding.when, "editorTextFocus");
    EXPECT_EQ(json::parse(binding.args), (json{{"x", 1}}));
}

TEST_F(ContributionsTest, ThePaletteRulesAreRead) {
    const auto dir =
        extension("a", "one",
                  {{"menus",
                    {{"commandPalette",
                      {{{"command", "one.a"}, {"when", "false"}}, {{"command", "one.b"}}}}}}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.palette_rules.size(), 2U);
    EXPECT_EQ(contributions.palette_rules[0].command, "one.a");
    EXPECT_EQ(contributions.palette_rules[0].when, "false");
    EXPECT_TRUE(contributions.palette_rules[1].when.empty());
}

TEST_F(ContributionsTest, AMalformedSectionIsLoggedAndTheOthersStillLoad) {
    const auto bad = extension("a", "bad",
                               {{"commands", "nope"},
                                {"keybindings", {{{"command", "bad.k"}, {"key", "ctrl+2"}}}},
                                {"menus", 3}});
    const auto good =
        extension("b", "good", {{"commands", {{{"command", "good.a"}, {"title", "Good"}}}}});
    const Contributions contributions = read({bad, good});

    ASSERT_EQ(contributions.commands.size(), 1U);
    EXPECT_EQ(contributions.commands[0].command, "good.a");
    ASSERT_EQ(contributions.keybindings.size(), 1U);
    EXPECT_EQ(contributions.keybindings[0].command, "bad.k");
    EXPECT_TRUE(logger_->contains(LogLevel::warning,
                                  "acme.bad: contributes.commands is not an array", "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "acme.bad: contributes.menus is not an object",
                                  "extensions"));
    EXPECT_FALSE(logger_->contains(LogLevel::warning, "acme.good", "extensions"));
}

TEST_F(ContributionsTest, AnEntryThatCannotBeUsedIsSkippedAndLoggedOnce) {
    const auto dir = extension(
        "a", "one",
        {{"commands",
          {3,
           {{"title", "No command"}},
           {{"command", "one.notitle"}},
           {{"command", "one.badtitle"}, {"title", 4}},
           {{"command", "one.badenablement"}, {"title", "T"}, {"enablement", 1}},
           {{"command", "one.ok"}, {"title", "Fine"}}}},
         {"keybindings",
          {{{"key", "ctrl+1"}}, {{"command", "one.k"}, {"key", 5}}, {{"command", "one.k2"}}}}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.commands.size(), 1U);
    EXPECT_EQ(contributions.commands[0].command, "one.ok");
    EXPECT_EQ(contributions.keybindings.size(), 1U);
    EXPECT_EQ(contributions.keybindings[0].command, "one.k2");

    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.commands[0] is not an object",
                                  "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.commands[1] has no command",
                                  "extensions"));
    EXPECT_TRUE(
        logger_->contains(LogLevel::warning, "contributes.commands[2] has no title", "extensions"));
    EXPECT_TRUE(
        logger_->contains(LogLevel::warning, "contributes.commands[3] has no title", "extensions"));
    EXPECT_TRUE(logger_->contains(
        LogLevel::warning, "contributes.commands[4].enablement is not a string", "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.keybindings[0] has no command",
                                  "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning,
                                  "contributes.keybindings[1].key is not a string", "extensions"));
}

TEST_F(ContributionsTest, ManifestsThatCannotBeReadAreLeftToTheInitData) {
    std::filesystem::create_directories(root_ / "empty");
    const auto broken = root_ / "broken";
    std::filesystem::create_directories(broken);
    std::ofstream(broken / "package.json") << "{";
    const auto no_publisher = root_ / "np";
    std::filesystem::create_directories(no_publisher);
    std::ofstream(no_publisher / "package.json") << R"({"name": "x", "contributes": {}})";
    EXPECT_TRUE(read({root_ / "empty", broken, no_publisher, root_ / "missing"}).commands.empty());
}

TEST_F(ContributionsTest, AnExtensionWithoutContributesHasNone) {
    const auto dir = root_ / "a";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "package.json") << R"({"name": "x", "publisher": "p"})";
    const Contributions contributions = read({dir});
    EXPECT_TRUE(contributions.commands.empty());
    EXPECT_TRUE(contributions.keybindings.empty());
    EXPECT_TRUE(contributions.palette_rules.empty());
}

TEST_F(ContributionsTest, ExtensionKeybindingsUseTheFormOfThePlatform) {
    const auto dir = extension("a", "one",
                               {{"keybindings",
                                 {{{"command", "one.a"},
                                   {"key", "ctrl+1"},
                                   {"mac", "cmd+1"},
                                   {"linux", "ctrl+alt+1"},
                                   {"win", "ctrl+shift+1"}},
                                  {{"command", "one.b"}, {"key", "ctrl+2"}, {"mac", "cmd+2"}},
                                  {{"command", "one.c"}, {"mac", "cmd+3"}}}}});
    const Contributions contributions = read({dir});

    const auto keys = [&](KeyPlatform platform) {
        std::vector<std::string> result;
        for (const auto& binding : extension_keybindings(contributions, platform).bindings) {
            EXPECT_EQ(binding.source, anvil::core::KeybindingSource::extension);
            result.push_back(binding.command + " " +
                             anvil::core::format_key_chord(binding.chord, platform));
        }
        return result;
    };
    EXPECT_EQ(keys(KeyPlatform::linux),
              (std::vector<std::string>{"one.a ctrl+alt+1", "one.b ctrl+2"}));
    EXPECT_EQ(keys(KeyPlatform::windows),
              (std::vector<std::string>{"one.a ctrl+shift+1", "one.b ctrl+2"}));
    EXPECT_EQ(keys(KeyPlatform::macos),
              (std::vector<std::string>{"one.a cmd+1", "one.b cmd+2", "one.c cmd+3"}));
}

TEST_F(ContributionsTest, ExtensionKeybindingsKeepTheirWhenAndArguments) {
    const auto dir = extension("a", "one",
                               {{"keybindings",
                                 {{{"command", "one.a"},
                                   {"key", "ctrl+k ctrl+1"},
                                   {"when", "editorTextFocus && !editorReadonly"},
                                   {"args", {{"n", 2}}}}}}});
    const auto file = extension_keybindings(read({dir}), KeyPlatform::linux);
    ASSERT_EQ(file.bindings.size(), 1U);
    EXPECT_EQ(file.bindings[0].chord.steps.size(), 2U);
    ASSERT_TRUE(file.bindings[0].when.has_value());
    EXPECT_EQ(file.bindings[0].args, (json{{"n", 2}}));
    EXPECT_TRUE(file.problems.empty());
}

TEST_F(ContributionsTest, AnExtensionKeybindingThatCannotBeUsedIsReportedWithItsExtension) {
    const auto dir = extension("a", "one",
                               {{"keybindings",
                                 {{{"command", "one.a"}, {"key", "ctrl+nonsense"}},
                                  {{"command", "one.b"}, {"key", "ctrl+2"}, {"when", "a &&"}},
                                  {{"command", "one.c"}, {"key", "ctrl+3"}}}}});
    const auto file = extension_keybindings(read({dir}), KeyPlatform::linux);
    ASSERT_EQ(file.bindings.size(), 1U);
    EXPECT_EQ(file.bindings[0].command, "one.c");
    ASSERT_EQ(file.problems.size(), 2U);
    EXPECT_TRUE(file.problems[0].starts_with("acme.one: key binding of one.a: "))
        << file.problems[0];
    EXPECT_TRUE(file.problems[1].starts_with("acme.one: key binding of one.b: "))
        << file.problems[1];
}

TEST_F(ContributionsTest, TheInitDataActivatesAnExtensionForEachCommandItContributes) {
    const auto dir = root_ / "a";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "package.json") << R"({
        "name": "one", "publisher": "acme", "activationEvents": ["onLanguage:x", "onCommand:one.a"],
        "contributes": {"commands": [
            {"command": "one.a", "title": "A"}, {"command": "one.b", "title": "B"}, 3, {"title": "C"}]}})";
    const auto text = build_init_data({.extension_dirs = {dir}});
    ASSERT_TRUE(text.has_value()) << text.error().message;
    EXPECT_EQ(json::parse(*text)["extensions"]["activationEvents"]["acme.one"],
              (json{"onLanguage:x", "onCommand:one.a", "onCommand:one.b"}));
}

TEST_F(ContributionsTest, ThemesCarryTheirLabelKindAndAPathInsideTheExtension) {
    const auto dir = extension(
        "a", "one",
        {{"themes",
          {{{"label", "Plain"}, {"path", "./themes/plain.json"}},
           {{"id", "light-id"}, {"label", "%light%"}, {"uiTheme", "vs"}, {"path", "light.json"}},
           {{"label", "Contrast"}, {"uiTheme", "hc-black"}, {"path", "hc.json"}}}}},
        json{{"light", "Translated Light"}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.themes.size(), 3U);
    EXPECT_EQ(contributions.themes[0].label, "Plain");
    EXPECT_TRUE(contributions.themes[0].id.empty());
    EXPECT_EQ(contributions.themes[0].ui_theme, "vs-dark");
    EXPECT_EQ(contributions.themes[0].path, dir / "themes" / "plain.json");
    EXPECT_EQ(contributions.themes[0].extension, "acme.one");
    EXPECT_EQ(contributions.themes[1].id, "light-id");
    EXPECT_EQ(contributions.themes[1].label, "Translated Light");
    EXPECT_EQ(contributions.themes[1].ui_theme, "vs");
    EXPECT_EQ(contributions.themes[2].ui_theme, "hc-black");
}

TEST_F(ContributionsTest, ThemesThatCannotBeUsedAreSkippedAndLogged) {
    const auto dir = extension("a", "one",
                               {{"themes",
                                 {3,
                                  {{"path", "a.json"}},
                                  {{"label", "No path"}},
                                  {{"label", "Escapes"}, {"path", "../outside.json"}},
                                  {{"label", "Absolute"}, {"path", "/etc/passwd"}},
                                  {{"label", "Fine"}, {"path", "fine.json"}}}}});
    const Contributions contributions = read({dir});
    ASSERT_EQ(contributions.themes.size(), 1U);
    EXPECT_EQ(contributions.themes[0].label, "Fine");
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.themes[0] is not an object",
                                  "extensions"));
    EXPECT_TRUE(
        logger_->contains(LogLevel::warning, "contributes.themes[1] has no label", "extensions"));
    EXPECT_TRUE(
        logger_->contains(LogLevel::warning, "contributes.themes[2] has no path", "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.themes[3].path is outside",
                                  "extensions"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "contributes.themes[4].path is outside",
                                  "extensions"));
}
