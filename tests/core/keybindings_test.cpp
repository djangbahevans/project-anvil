#include "anvil/core/keybindings.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <string>

using namespace anvil::core;
using nlohmann::json;
using Kind = Resolution::Kind;

namespace {

KeyChord chord_of(std::string_view text) {
    auto chord = parse_key_chord(text);
    EXPECT_TRUE(chord.has_value()) << text;
    return chord ? *chord : KeyChord{};
}

WhenExpression when_of(std::string_view text) {
    auto when = WhenExpression::parse(text);
    EXPECT_TRUE(when.has_value()) << text;
    return when ? *when : WhenExpression();
}

Keybinding binding(std::string_view key, std::string command, std::string_view when = {},
                   KeybindingSource source = KeybindingSource::builtin) {
    Keybinding result;
    result.chord = key.empty() ? KeyChord{} : chord_of(key);
    result.command = std::move(command);
    if (!when.empty()) {
        result.when = when_of(when);
    }
    result.source = source;
    return result;
}

Keybinding user(std::string_view key, std::string command, std::string_view when = {}) {
    return binding(key, std::move(command), when, KeybindingSource::user);
}

ContextKeys context_of(std::initializer_list<const char*> true_keys) {
    ContextKeys keys;
    for (const char* key : true_keys) {
        keys.set(key, true);
    }
    return keys;
}

// Presses the keys in a row, as the dispatcher does: the presses before the last are the chord in
// progress.
Resolution press(const KeybindingTable& table, std::string_view keys,
                 const ContextKeys& context = {}) {
    const KeyChord chord = chord_of(keys);
    return table.resolve(std::span(chord.steps).first(chord.steps.size() - 1), chord.steps.back(),
                         context);
}

std::string command_of(const Resolution& resolution) {
    switch (resolution.kind) {
    case Kind::none:
        return "-";
    case Kind::more_keys_needed:
        return "...";
    case Kind::found:
        return resolution.command;
    }
    return {};
}

std::string chord_text(const Keybinding& binding) {
    return format_key_chord(binding.chord);
}

} // namespace

TEST(KeybindingTableTest, AKeyThatIsBoundGivesItsCommand) {
    const KeybindingTable table({binding("ctrl+s", "save"), binding("ctrl+o", "open")});
    EXPECT_EQ(command_of(press(table, "ctrl+s")), "save");
    EXPECT_EQ(command_of(press(table, "ctrl+o")), "open");
    EXPECT_EQ(command_of(press(table, "ctrl+p")), "-");
    EXPECT_EQ(command_of(press(table, "s")), "-");
}

TEST(KeybindingTableTest, AnEmptyTableBindsNothing) {
    const KeybindingTable table;
    EXPECT_EQ(command_of(press(table, "ctrl+s")), "-");
    EXPECT_TRUE(table.bindings().empty());
    EXPECT_TRUE(table.lookup("save").empty());
}

TEST(KeybindingTableTest, ABindingWithAWhenAppliesOnlyWhereItHolds) {
    const KeybindingTable table(
        {binding("tab", "indent", "editorTextFocus && !suggestWidgetVisible")});
    EXPECT_EQ(command_of(press(table, "tab", context_of({"editorTextFocus"}))), "indent");
    EXPECT_EQ(
        command_of(press(table, "tab", context_of({"editorTextFocus", "suggestWidgetVisible"}))),
        "-");
    EXPECT_EQ(command_of(press(table, "tab")), "-");
}

TEST(KeybindingTableTest, TheLastBindingWhoseWhenHoldsWins) {
    const KeybindingTable table({binding("ctrl+d", "first"), binding("ctrl+d", "second", "a"),
                                 binding("ctrl+d", "third", "b")});
    EXPECT_EQ(command_of(press(table, "ctrl+d")), "first");
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"a"}))), "second");
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"b"}))), "third");
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"a", "b"}))), "third");
}

TEST(KeybindingTableTest, ALaterBindingWinsEvenWithoutAWhenOverOneWithAWhen) {
    const KeybindingTable table({binding("ctrl+d", "specific", "a"), binding("ctrl+d", "general")});
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"a"}))), "general");
}

TEST(KeybindingTableTest, ExtensionsOverrideTheBuiltInBindingsAndTheUserOverridesBoth) {
    const KeybindingTable table({user("ctrl+d", "by-user"),
                                 binding("ctrl+d", "by-extension", {}, KeybindingSource::extension),
                                 binding("ctrl+d", "built-in")});
    EXPECT_EQ(command_of(press(table, "ctrl+d")), "by-user");
    ASSERT_EQ(table.bindings().size(), 3U);
    EXPECT_EQ(table.bindings()[0].command, "built-in");
    EXPECT_EQ(table.bindings()[1].command, "by-extension");
    EXPECT_EQ(table.bindings()[2].command, "by-user");

    const KeybindingTable no_user(
        {binding("ctrl+d", "by-extension", {}, KeybindingSource::extension),
         binding("ctrl+d", "built-in")});
    EXPECT_EQ(command_of(press(no_user, "ctrl+d")), "by-extension");
}

TEST(KeybindingTableTest, AUserBindingWithAWhenFallsBackToTheOnesBelowWhereItDoesNotHold) {
    const KeybindingTable table({binding("ctrl+d", "built-in"), user("ctrl+d", "by-user", "a")});
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"a"}))), "by-user");
    EXPECT_EQ(command_of(press(table, "ctrl+d")), "built-in");
}

TEST(KeybindingTableTest, WithinASourceTheLaterBindingWins) {
    const KeybindingTable table({binding("x", "one"), binding("x", "two")});
    EXPECT_EQ(command_of(press(table, "x")), "two");
}

TEST(KeybindingTableTest, ACommandArgumentIsPassedOn) {
    Keybinding with_args = binding("ctrl+k", "type");
    with_args.args = json{{"text", "hello"}};
    const KeybindingTable table({with_args});
    const Resolution resolution = press(table, "ctrl+k");
    ASSERT_EQ(resolution.kind, Kind::found);
    ASSERT_TRUE(resolution.args.has_value());
    EXPECT_EQ((*resolution.args)["text"], "hello");
    EXPECT_FALSE(press(KeybindingTable({binding("ctrl+k", "type")}), "ctrl+k").args.has_value());
}

TEST(KeybindingTableTest, ACommandMarkedWithACaretLetsThePressGoOn) {
    const KeybindingTable table({binding("ctrl+k", "^notify"), binding("ctrl+l", "plain")});
    const Resolution marked = press(table, "ctrl+k");
    EXPECT_EQ(marked.command, "notify");
    EXPECT_TRUE(marked.bubble);
    EXPECT_FALSE(press(table, "ctrl+l").bubble);
    EXPECT_EQ(table.lookup("notify").size(), 1U);
}

TEST(KeybindingTableTest, ABindingWhoseWhenIsFalseIsNeverUsed) {
    const KeybindingTable table({binding("x", "never", "false"), binding("y", "sometimes", "a")});
    EXPECT_EQ(table.bindings().size(), 1U);
    EXPECT_EQ(command_of(press(table, "x")), "-");
}

TEST(KeybindingTableTest, ABindingWithNoKeyIsNotInTheTable) {
    const KeybindingTable table({binding("", "unbound"), binding("x", "bound")});
    EXPECT_EQ(table.bindings().size(), 1U);
}

TEST(KeybindingTableTest, AChordNeedsItsNextKeys) {
    const KeybindingTable table(
        {binding("ctrl+k ctrl+c", "comment"), binding("ctrl+k ctrl+u", "uncomment")});
    EXPECT_EQ(command_of(press(table, "ctrl+k")), "...");
    EXPECT_EQ(command_of(press(table, "ctrl+k ctrl+c")), "comment");
    EXPECT_EQ(command_of(press(table, "ctrl+k ctrl+u")), "uncomment");
    EXPECT_EQ(command_of(press(table, "ctrl+k ctrl+x")), "-");
    EXPECT_EQ(command_of(press(table, "ctrl+c")), "-");
}

TEST(KeybindingTableTest, AChordCanHaveThreeStepsAndFourSteps) {
    const KeybindingTable table({binding("a b c", "abc"), binding("a b c d", "abcd")});
    EXPECT_EQ(command_of(press(table, "a")), "...");
    EXPECT_EQ(command_of(press(table, "a b")), "...");
    EXPECT_EQ(command_of(press(table, "a b c")), "...");
    EXPECT_EQ(command_of(press(table, "a b c d")), "abcd");
}

TEST(KeybindingTableTest, TheWinnerDecidesWhetherABindingThatIsAlsoAPrefixWaits) {
    const KeybindingTable long_wins({binding("ctrl+k", "short"), binding("ctrl+k ctrl+c", "long")});
    EXPECT_EQ(command_of(press(long_wins, "ctrl+k")), "...");
    const KeybindingTable short_wins(
        {binding("ctrl+k ctrl+c", "long"), binding("ctrl+k", "short")});
    EXPECT_EQ(command_of(press(short_wins, "ctrl+k")), "short");
}

TEST(KeybindingTableTest, AChordIsFoundOnlyWhereItsWhenHolds) {
    const KeybindingTable table(
        {binding("ctrl+k ctrl+c", "comment", "a"), binding("ctrl+k", "short", "!a")});
    EXPECT_EQ(command_of(press(table, "ctrl+k", context_of({"a"}))), "...");
    EXPECT_EQ(command_of(press(table, "ctrl+k")), "short");
    EXPECT_EQ(command_of(press(table, "ctrl+k ctrl+c", context_of({"a"}))), "comment");
    EXPECT_EQ(command_of(press(table, "ctrl+k ctrl+c")), "-");
}

TEST(KeybindingTableTest, ARemovalTakesAwayTheBuiltInBindingOfACommandAtAKey) {
    const KeybindingTable table({binding("ctrl+d", "dup"), binding("ctrl+e", "dup"),
                                 binding("ctrl+d", "other"), user("ctrl+d", "-dup")});
    EXPECT_EQ(command_of(press(table, "ctrl+d")), "other");
    EXPECT_EQ(command_of(press(table, "ctrl+e")), "dup");
}

TEST(KeybindingTableTest, ARemovalWithoutAKeyTakesAwayEveryBindingOfTheCommand) {
    const KeybindingTable table({binding("ctrl+d", "dup"), binding("ctrl+e", "dup"),
                                 binding("ctrl+f", "keep"), user("", "-dup")});
    EXPECT_EQ(command_of(press(table, "ctrl+d")), "-");
    EXPECT_EQ(command_of(press(table, "ctrl+e")), "-");
    EXPECT_EQ(command_of(press(table, "ctrl+f")), "keep");
    EXPECT_EQ(table.bindings().size(), 1U);
}

TEST(KeybindingTableTest, ARemovalDoesNotTouchTheBindingsOfTheUser) {
    const KeybindingTable table(
        {binding("ctrl+d", "dup"), user("ctrl+d", "dup", "a"), user("ctrl+d", "-dup")});
    EXPECT_EQ(command_of(press(table, "ctrl+d", context_of({"a"}))), "dup");
    EXPECT_EQ(table.bindings().size(), 1U);
}

TEST(KeybindingTableTest, ARemovalWithAShorterKeyTakesAwayTheChordsThatStartWithIt) {
    const KeybindingTable table({binding("ctrl+k ctrl+c", "comment"),
                                 binding("ctrl+k ctrl+u", "comment"), user("ctrl+k", "-comment")});
    EXPECT_EQ(command_of(press(table, "ctrl+k")), "-");
    const KeybindingTable longer({binding("ctrl+k", "comment"), user("ctrl+k ctrl+c", "-comment")});
    EXPECT_EQ(command_of(press(longer, "ctrl+k")), "comment");
}

TEST(KeybindingTableTest, ARemovalWithAWhenTakesAwayOnlyBindingsThatImplyIt) {
    const KeybindingTable table({binding("x", "cmd", "a && b"), binding("y", "cmd", "a"),
                                 binding("z", "cmd"), user("", "-cmd", "a")});
    EXPECT_EQ(command_of(press(table, "x", context_of({"a", "b"}))), "-");
    EXPECT_EQ(command_of(press(table, "y", context_of({"a"}))), "-");
    EXPECT_EQ(command_of(press(table, "z", context_of({"a"}))), "cmd");
}

TEST(KeybindingTableTest, ARemovalOfOneCommandLeavesTheOthers) {
    const KeybindingTable table({binding("x", "a"), binding("x", "b"), user("x", "-a")});
    EXPECT_EQ(command_of(press(table, "x")), "b");
}

TEST(KeybindingTableTest, AnExtensionCanRemoveABuiltInBinding) {
    const KeybindingTable table(
        {binding("x", "a"), binding("", "-a", {}, KeybindingSource::extension)});
    EXPECT_EQ(command_of(press(table, "x")), "-");
}

TEST(KeybindingTableTest, TheBindingsOfACommandAreListedMostSpecificFirst) {
    const KeybindingTable table(
        {binding("ctrl+a", "cmd"), binding("ctrl+b", "cmd", "x"), user("ctrl+c", "cmd")});
    std::vector<std::string> keys;
    for (const Keybinding* found : table.lookup("cmd")) {
        keys.push_back(chord_text(*found));
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"ctrl+c", "ctrl+b", "ctrl+a"}));
    EXPECT_TRUE(table.lookup("missing").empty());
}

TEST(KeybindingTableTest, ABindingThatALaterOneCoversIsNotListedForItsCommand) {
    const KeybindingTable table({binding("ctrl+a", "old"), binding("ctrl+a", "new")});
    EXPECT_TRUE(table.lookup("old").empty());
    EXPECT_EQ(table.lookup("new").size(), 1U);
    const KeybindingTable partial({binding("ctrl+a", "old"), binding("ctrl+a", "new", "x")});
    EXPECT_EQ(partial.lookup("old").size(), 1U);
    const KeybindingTable same_command({binding("ctrl+a", "cmd"), binding("ctrl+a", "cmd")});
    EXPECT_EQ(same_command.lookup("cmd").size(), 2U);
}

TEST(KeybindingTableTest, ThePrimaryBindingOfACommandIsTheMostSpecificOneThatHolds) {
    const KeybindingTable table({binding("ctrl+a", "cmd"), binding("ctrl+b", "cmd", "x")});
    EXPECT_EQ(chord_text(*table.lookup_primary("cmd", context_of({"x"}))), "ctrl+b");
    EXPECT_EQ(chord_text(*table.lookup_primary("cmd", context_of({}))), "ctrl+a");
    const KeybindingTable only({binding("ctrl+b", "cmd", "x")});
    EXPECT_EQ(chord_text(*only.lookup_primary("cmd", context_of({}))), "ctrl+b");
    EXPECT_EQ(only.lookup_primary("cmd", context_of({}), true), nullptr);
    EXPECT_EQ(only.lookup_primary("nothing", context_of({})), nullptr);
    const KeybindingTable none_holds(
        {binding("ctrl+a", "cmd", "x"), binding("ctrl+b", "cmd", "y")});
    EXPECT_EQ(chord_text(*none_holds.lookup_primary("cmd", context_of({}))), "ctrl+b");
    EXPECT_EQ(none_holds.lookup_primary("cmd", context_of({}), true), nullptr);
}

TEST(ReadKeybindingsTest, ReadsTheEntriesOfAKeybindingsFile) {
    const json file = json::parse(R"j([
        {"key": "ctrl+shift+k", "command": "editor.action.deleteLines", "when": "editorTextFocus"},
        {"key": "ctrl+k ctrl+c", "command": "comment", "args": {"line": true}},
        {"command": "-old.command"},
        {"key": "ctrl+d", "command": "-other", "when": "a"}
    ])j");
    const auto read = read_keybindings(file, KeybindingSource::user);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read->problems.empty());
    ASSERT_EQ(read->bindings.size(), 4U);
    EXPECT_EQ(chord_text(read->bindings[0]), "ctrl+shift+k");
    EXPECT_EQ(read->bindings[0].command, "editor.action.deleteLines");
    ASSERT_TRUE(read->bindings[0].when.has_value());
    EXPECT_EQ(read->bindings[0].when->canonical(), "editorTextFocus");
    EXPECT_EQ(read->bindings[0].source, KeybindingSource::user);
    EXPECT_EQ(chord_text(read->bindings[1]), "ctrl+k ctrl+c");
    ASSERT_TRUE(read->bindings[1].args.has_value());
    EXPECT_EQ((*read->bindings[1].args)["line"], true);
    EXPECT_TRUE(read->bindings[2].chord.steps.empty());
    EXPECT_EQ(read->bindings[2].command, "-old.command");
    EXPECT_FALSE(read->bindings[3].chord.steps.empty());
}

TEST(ReadKeybindingsTest, AnEntryThatCannotBeUsedIsReportedWithItsIndexAndSkipped) {
    const json file = json::parse(R"j([
        {"key": "ctrl+a", "command": "good"},
        {"key": "ctrl+nonsense", "command": "bad-key"},
        {"key": "ctrl+b"},
        {"command": "no-key"},
        {"key": 5, "command": "key-not-a-string"},
        {"key": "ctrl+c", "command": 7},
        {"key": "ctrl+d", "command": "bad-when", "when": "a &&"},
        {"key": "ctrl+e", "command": "when-not-a-string", "when": 3},
        "not an object",
        {"key": "ctrl+f", "command": ""},
        {"key": "ctrl+g", "command": "also-good", "when": null}
    ])j");
    const auto read = read_keybindings(file, KeybindingSource::user);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->bindings.size(), 2U);
    EXPECT_EQ(read->bindings[0].command, "good");
    EXPECT_EQ(read->bindings[1].command, "also-good");
    ASSERT_EQ(read->problems.size(), 9U);
    EXPECT_TRUE(read->problems[0].starts_with("entry 1: ")) << read->problems[0];
    EXPECT_NE(read->problems[0].find("unknown key"), std::string::npos);
    EXPECT_TRUE(read->problems[1].starts_with("entry 2: ")) << read->problems[1];
    EXPECT_TRUE(read->problems[2].starts_with("entry 3: "));
    EXPECT_TRUE(read->problems[3].starts_with("entry 4: "));
    EXPECT_TRUE(read->problems[4].starts_with("entry 5: "));
    EXPECT_TRUE(read->problems[5].starts_with("entry 6: "));
    EXPECT_NE(read->problems[5].find("when"), std::string::npos);
    EXPECT_TRUE(read->problems[6].starts_with("entry 7: "));
    EXPECT_TRUE(read->problems[7].starts_with("entry 8: "));
    EXPECT_TRUE(read->problems[8].starts_with("entry 9: "));
}

TEST(ReadKeybindingsTest, ANonArrayIsAnError) {
    EXPECT_FALSE(read_keybindings(json::object(), KeybindingSource::user).has_value());
    EXPECT_FALSE(read_keybindings(json("text"), KeybindingSource::user).has_value());
    const auto empty = read_keybindings(json::array(), KeybindingSource::user);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->bindings.empty());
}

TEST(ReadKeybindingsTest, AReadFileMakesATable) {
    const auto defaults = read_keybindings(json::parse(R"j([{"key":"ctrl+s","command":"save"},
                                                           {"key":"ctrl+w","command":"close"}])j"),
                                           KeybindingSource::builtin);
    const auto mine = read_keybindings(json::parse(R"j([{"key":"ctrl+s","command":"mine"},
                                                       {"command":"-close"}])j"),
                                       KeybindingSource::user);
    ASSERT_TRUE(defaults && mine);
    std::vector<Keybinding> all = defaults->bindings;
    all.insert(all.end(), mine->bindings.begin(), mine->bindings.end());
    const KeybindingTable table(std::move(all));
    EXPECT_EQ(command_of(press(table, "ctrl+s")), "mine");
    EXPECT_EQ(command_of(press(table, "ctrl+w")), "-");
}

TEST(WhenImpliesTest, ProvesWhatFollowsFromTheTermsThatAreThere) {
    EXPECT_TRUE(when_of("a && b").implies(when_of("a")));
    EXPECT_TRUE(when_of("a && b && c").implies(when_of("b && a")));
    EXPECT_FALSE(when_of("a").implies(when_of("a && b")));
    EXPECT_TRUE(when_of("a").implies(when_of("a || b")));
    EXPECT_TRUE(when_of("a || b").implies(when_of("a || b || c")));
    EXPECT_FALSE(when_of("a || b").implies(when_of("a")));
    EXPECT_TRUE(when_of("a").implies(when_of("a")));
    EXPECT_FALSE(when_of("a").implies(when_of("!a")));
}

TEST(WhenImpliesTest, ProvesNoMoreThanVsCodeDoes) {
    EXPECT_FALSE(when_of("(a || b) && c").implies(when_of("c || a")));
    EXPECT_TRUE(when_of("a && c").implies(when_of("c || a")));
}

TEST(WhenImpliesTest, TrueImpliesOnlyTrueAndFalseImpliesEverything) {
    EXPECT_TRUE(WhenExpression().implies(WhenExpression()));
    EXPECT_FALSE(WhenExpression().implies(when_of("a")));
    EXPECT_TRUE(when_of("a").implies(WhenExpression()));
    EXPECT_TRUE(when_of("false").implies(when_of("a")));
    EXPECT_TRUE(when_of("false").is_false());
    EXPECT_FALSE(when_of("a").is_false());
    EXPECT_FALSE(WhenExpression().is_false());
    EXPECT_TRUE(when_of("a && !a").is_false());
}

TEST(KeybindingReplayTest, ResolutionsAndLookupsMatchVsCodes) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/keybinding_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);

    const auto source_of = [](const std::string& name) {
        return name == "user"        ? KeybindingSource::user
               : name == "extension" ? KeybindingSource::extension
                                     : KeybindingSource::builtin;
    };
    const auto context_from = [](const json& values) {
        ContextKeys keys;
        for (const auto& [name, value] : values.items()) {
            keys.set(name, value.get<bool>());
        }
        return keys;
    };

    std::size_t queries = 0;
    std::size_t tables = 0;
    for (const json& recorded_table : recorded["tables"]) {
        std::vector<Keybinding> bindings;
        for (const json& entry : recorded_table["bindings"]) {
            Keybinding binding;
            if (entry.contains("key")) {
                binding.chord = chord_of(entry["key"].get<std::string>());
            }
            binding.command = entry["command"];
            if (entry.contains("when")) {
                binding.when = when_of(entry["when"].get<std::string>());
            }
            binding.source = source_of(entry["source"]);
            bindings.push_back(std::move(binding));
        }
        const KeybindingTable table(std::move(bindings));
        ++tables;

        for (const json& query : recorded_table["queries"]) {
            const Resolution resolution = press(
                table,
                [&] {
                    std::string keys;
                    for (const json& key : query["presses"]) {
                        keys += (keys.empty() ? "" : " ") + key.get<std::string>();
                    }
                    return keys;
                }(),
                context_from(query["context"]));
            const std::string kind = query["kind"];
            const std::string where = recorded_table["bindings"].dump() + " " + query.dump();
            if (kind == "none") {
                EXPECT_EQ(resolution.kind, Kind::none) << where;
            } else if (kind == "more") {
                EXPECT_EQ(resolution.kind, Kind::more_keys_needed) << where;
            } else {
                ASSERT_EQ(resolution.kind, Kind::found) << where;
                EXPECT_EQ(resolution.command, query["command"].get<std::string>()) << where;
                EXPECT_EQ(resolution.bubble, query["bubble"].get<bool>()) << where;
            }
            ++queries;
        }

        for (const json& lookup : recorded_table["lookups"]) {
            std::vector<std::string> keys;
            for (const Keybinding* found : table.lookup(lookup["command"].get<std::string>())) {
                keys.push_back(chord_text(*found));
            }
            EXPECT_EQ(json(keys), lookup["keys"])
                << recorded_table["bindings"].dump() << " " << lookup.dump();
        }

        for (const json& primary : recorded_table["primaries"]) {
            const Keybinding* found = table.lookup_primary(primary["command"].get<std::string>(),
                                                           context_from(primary["context"]),
                                                           primary["enforce"].get<bool>());
            if (primary["key"].is_null()) {
                EXPECT_EQ(found, nullptr) << primary.dump();
            } else {
                ASSERT_NE(found, nullptr) << primary.dump();
                EXPECT_EQ(chord_text(*found), primary["key"].get<std::string>()) << primary.dump();
            }
        }
    }
    EXPECT_EQ(tables, 200U);
    EXPECT_EQ(queries, 3200U);
}

TEST(KeybindingReplayTest, WhatVsCodeProvesToImplyIsProvenHere) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/keybinding_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);
    for (const json& pair : recorded["implications"]) {
        const bool implied =
            when_of(pair["a"].get<std::string>()).implies(when_of(pair["b"].get<std::string>()));
        EXPECT_EQ(implied, pair["implies"].get<bool>()) << pair["a"] << " => " << pair["b"];
    }
}
