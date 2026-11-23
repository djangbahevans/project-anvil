#include "anvil/core/key_chord.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>

using namespace anvil::core;
using nlohmann::json;

namespace {

KeyChord parsed(std::string_view text) {
    auto chord = parse_key_chord(text);
    EXPECT_TRUE(chord.has_value()) << text << ": " << (chord ? "" : chord.error().message);
    return chord ? *chord : KeyChord{};
}

KeyStep step(Key key, Modifiers modifiers = {}) {
    return {.modifiers = modifiers, .key = key};
}

std::string error_of(std::string_view text) {
    const auto chord = parse_key_chord(text);
    EXPECT_FALSE(chord.has_value()) << text;
    if (chord) {
        return {};
    }
    EXPECT_EQ(chord.error().code, anvil::ErrorCode::invalid_argument);
    return chord.error().message;
}

} // namespace

TEST(KeyTableTest, EveryKeyIsAtItsOwnPositionAndNamedOnce) {
    std::set<std::string> spellings;
    std::size_t index = 0;
    for (const KeyInfo& info : key_table) {
        EXPECT_EQ(static_cast<std::size_t>(info.key), index) << info.setting;
        EXPECT_EQ(&key_info(info.key), &key_table[index]);
        EXPECT_FALSE(info.label.empty());
        EXPECT_FALSE(info.setting.empty());
        EXPECT_EQ(info.setting, std::string(info.setting.begin(), info.setting.end()));
        EXPECT_TRUE(spellings.insert(std::string(info.setting)).second) << info.setting;
        if (!info.alias.empty()) {
            EXPECT_TRUE(spellings.insert(std::string(info.alias)).second) << info.alias;
        }
        ++index;
    }
    EXPECT_GT(key_table[0].setting.size(), 0U);
    EXPECT_GE(index, 120U);
}

TEST(KeyTableTest, EverySpellingFindsItsKeyWhateverTheCase) {
    for (const KeyInfo& info : key_table) {
        EXPECT_EQ(key_from_name(info.setting), info.key) << info.setting;
        std::string upper(info.setting);
        std::ranges::transform(upper, upper.begin(),
                               [](char c) { return static_cast<char>(std::toupper(c)); });
        EXPECT_EQ(key_from_name(upper), info.key) << upper;
        if (!info.alias.empty()) {
            EXPECT_EQ(key_from_name(info.alias), info.key) << info.alias;
        }
    }
    EXPECT_EQ(key_from_name("nonsense"), std::nullopt);
    EXPECT_EQ(key_from_name(""), std::nullopt);
}

TEST(KeyChordTest, ParsesTheKeysOfVsCodesKeyBindings) {
    EXPECT_EQ(parsed("k").steps, (std::vector<KeyStep>{step(Key::key_k)}));
    EXPECT_EQ(parsed("7").steps, (std::vector<KeyStep>{step(Key::digit7)}));
    EXPECT_EQ(parsed("f1").steps, (std::vector<KeyStep>{step(Key::f1)}));
    EXPECT_EQ(parsed("f19").steps, (std::vector<KeyStep>{step(Key::f19)}));
    EXPECT_EQ(parsed("escape").steps, (std::vector<KeyStep>{step(Key::escape)}));
    EXPECT_EQ(parsed("pageup").steps, (std::vector<KeyStep>{step(Key::page_up)}));
    EXPECT_EQ(parsed("left").steps, (std::vector<KeyStep>{step(Key::left_arrow)}));
    EXPECT_EQ(parsed("delete").steps, (std::vector<KeyStep>{step(Key::delete_key)}));
    EXPECT_EQ(parsed("numpad0").steps, (std::vector<KeyStep>{step(Key::numpad0)}));
    EXPECT_EQ(parsed("numpad_add").steps, (std::vector<KeyStep>{step(Key::numpad_add)}));
    EXPECT_EQ(parsed("oem_3").steps, (std::vector<KeyStep>{step(Key::backquote)}));
    EXPECT_EQ(parsed("`").steps, (std::vector<KeyStep>{step(Key::backquote)}));
    EXPECT_EQ(parsed("oem_8").steps, (std::vector<KeyStep>{step(Key::oem_8)}));
    EXPECT_EQ(parsed("-").steps, (std::vector<KeyStep>{step(Key::minus)}));
    EXPECT_EQ(parsed("\\").steps, (std::vector<KeyStep>{step(Key::backslash)}));
}

TEST(KeyChordTest, ParsesModifiersInAnyOrderAndCase) {
    const Modifiers all{.ctrl = true, .shift = true, .alt = true, .meta = true};
    EXPECT_EQ(parsed("ctrl+shift+alt+meta+k").steps, (std::vector<KeyStep>{step(Key::key_k, all)}));
    EXPECT_EQ(parsed("meta+alt+shift+ctrl+k").steps, (std::vector<KeyStep>{step(Key::key_k, all)}));
    EXPECT_EQ(parsed("CTRL+Shift+ALT+Meta+K").steps, (std::vector<KeyStep>{step(Key::key_k, all)}));
    EXPECT_EQ(parsed("ctrl-shift-alt-meta-k").steps, (std::vector<KeyStep>{step(Key::key_k, all)}));
    EXPECT_EQ(parsed("ctrl+shift-alt+meta-k").steps, (std::vector<KeyStep>{step(Key::key_k, all)}));
    EXPECT_EQ(parsed("ctrl+ctrl+k").steps, parsed("ctrl+k").steps);
}

TEST(KeyChordTest, CmdWinAndSuperNameTheMetaKey) {
    for (const char* spelling : {"meta+k", "cmd+k", "win+k", "super+k", "CMD+K", "Win-K"}) {
        EXPECT_EQ(parsed(spelling).steps, (std::vector<KeyStep>{step(Key::key_k, {.meta = true})}))
            << spelling;
    }
}

TEST(KeyChordTest, ParsesSeveralStepsSeparatedByWhiteSpace) {
    const KeyChord chord = parsed("ctrl+k ctrl+c");
    ASSERT_EQ(chord.steps.size(), 2U);
    EXPECT_EQ(chord.steps[0], step(Key::key_k, {.ctrl = true}));
    EXPECT_EQ(chord.steps[1], step(Key::key_c, {.ctrl = true}));
    EXPECT_EQ(parsed("  ctrl+k   ctrl+c  "), chord);
    EXPECT_EQ(parsed("ctrl+k\tctrl+c"), chord);
    EXPECT_EQ(parsed("a b c d").steps.size(), 4U);
}

TEST(KeyChordTest, AModifierCanBeTheKeyItself) {
    EXPECT_EQ(parsed("shift shift").steps,
              (std::vector<KeyStep>{step(Key::shift), step(Key::shift)}));
    EXPECT_EQ(parsed("ctrl").steps, (std::vector<KeyStep>{step(Key::ctrl)}));
    EXPECT_EQ(parsed("ctrl+shift").steps, (std::vector<KeyStep>{step(Key::shift, {.ctrl = true})}));
}

TEST(KeyChordTest, AModifierThatIsAlsoTheKeyIsDropped) {
    EXPECT_EQ(parsed("ctrl+ctrl"), parsed("ctrl"));
    EXPECT_EQ(parsed("shift+shift"), parsed("shift"));
    EXPECT_EQ(parsed("alt+alt"), parsed("alt"));
    EXPECT_EQ(parsed("meta+meta"), parsed("meta"));
    EXPECT_EQ(parsed("cmd+meta"), parsed("meta"));
    EXPECT_NE(parsed("ctrl+shift"), parsed("shift"));
}

TEST(KeyChordTest, SpellingsOfTheSameChordAreEqual) {
    EXPECT_EQ(parsed("ctrl+shift+k"), parsed("Shift+Ctrl+K"));
    EXPECT_EQ(parsed("ctrl+shift+k"), parsed("ctrl-shift-k"));
    EXPECT_EQ(parsed("cmd+shift+p"), parsed("meta+shift+p"));
    EXPECT_EQ(parsed("ctrl+oem_minus"), parsed("ctrl+-"));
    EXPECT_EQ(parsed("ctrl+oem_plus"), parsed("ctrl+="));
    EXPECT_EQ(parsed("oem_comma"), parsed(","));
    EXPECT_EQ(parsed("ctrl+k ctrl+c"), parsed("CTRL+K   CTRL+C"));
    EXPECT_NE(parsed("ctrl+k"), parsed("ctrl+shift+k"));
    EXPECT_NE(parsed("ctrl+k"), parsed("ctrl+k ctrl+k"));
    EXPECT_LT(parsed("ctrl+k"), parsed("ctrl+shift+k"));
}

TEST(KeyChordTest, FormatsInVsCodesLowercaseForm) {
    EXPECT_EQ(format_key_chord(parsed("Shift+Ctrl+K")), "ctrl+shift+k");
    EXPECT_EQ(format_key_chord(parsed("meta+alt+shift+ctrl+K")), "ctrl+shift+alt+meta+k");
    EXPECT_EQ(format_key_chord(parsed("CTRL+K CTRL+C")), "ctrl+k ctrl+c");
    EXPECT_EQ(format_key_chord(parsed("ctrl+oem_minus")), "ctrl+-");
    EXPECT_EQ(format_key_chord(parsed("ctrl+left")), "ctrl+left");
    EXPECT_EQ(format_key_chord(parsed("numpad_add")), "numpad_add");
    EXPECT_EQ(format_key_chord(parsed("ctrl+ctrl")), "ctrl");
    EXPECT_EQ(format_key_chord(KeyChord{}), "");
}

TEST(KeyChordTest, TheMetaKeyIsWrittenAsTheSystemCallsIt) {
    const KeyChord chord = parsed("meta+shift+p");
    EXPECT_EQ(format_key_chord(chord), "shift+meta+p");
    EXPECT_EQ(format_key_chord(chord, KeyPlatform::linux), "shift+meta+p");
    EXPECT_EQ(format_key_chord(chord, KeyPlatform::windows), "shift+win+p");
    EXPECT_EQ(format_key_chord(chord, KeyPlatform::macos), "shift+cmd+p");
}

TEST(KeyChordTest, AFormattedChordParsesBackToTheSameChord) {
    for (const KeyInfo& info : key_table) {
        for (const Modifiers modifiers :
             {Modifiers{}, Modifiers{.ctrl = true}, Modifiers{.shift = true, .meta = true},
              Modifiers{.ctrl = true, .shift = true, .alt = true, .meta = true}}) {
            KeyChord chord;
            chord.steps = {step(info.key, modifiers), step(Key::key_k, {.alt = true})};
            for (const KeyPlatform platform :
                 {KeyPlatform::linux, KeyPlatform::windows, KeyPlatform::macos}) {
                const std::string text = format_key_chord(chord, platform);
                const auto again = parse_key_chord(text);
                ASSERT_TRUE(again.has_value()) << text;
                // A modifier that is also the key is not held, so the round trip starts there.
                KeyChord normalized = parsed(format_key_chord(chord, KeyPlatform::linux));
                EXPECT_EQ(*again, normalized) << text;
            }
        }
    }
}

TEST(KeyChordTest, LabelsShowTheKeysAsTheInterfaceDoes) {
    EXPECT_EQ(key_chord_label(parsed("ctrl+shift+k"), KeyPlatform::windows), "Ctrl+Shift+K");
    EXPECT_EQ(key_chord_label(parsed("ctrl+shift+k"), KeyPlatform::linux), "Ctrl+Shift+K");
    EXPECT_EQ(key_chord_label(parsed("shift+cmd+k"), KeyPlatform::macos), "⇧⌘K");
    EXPECT_EQ(key_chord_label(parsed("ctrl+alt+shift+cmd+k"), KeyPlatform::macos), "⌃⇧⌥⌘K");
    EXPECT_EQ(key_chord_label(parsed("meta+k"), KeyPlatform::windows), "Windows+K");
    EXPECT_EQ(key_chord_label(parsed("meta+k"), KeyPlatform::linux), "Super+K");
    EXPECT_EQ(key_chord_label(parsed("ctrl+k ctrl+c"), KeyPlatform::linux), "Ctrl+K Ctrl+C");
    EXPECT_EQ(key_chord_label(parsed("ctrl+k ctrl+c"), KeyPlatform::macos), "⌃K ⌃C");
    EXPECT_EQ(key_chord_label(parsed("f5"), KeyPlatform::linux), "F5");
    EXPECT_EQ(key_chord_label(parsed("ctrl+left"), KeyPlatform::linux), "Ctrl+LeftArrow");
    EXPECT_EQ(key_chord_label(parsed("ctrl+left"), KeyPlatform::macos), "⌃←");
    EXPECT_EQ(key_chord_label(parsed("alt+down"), KeyPlatform::macos), "⌥↓");
    EXPECT_EQ(key_chord_label(parsed("ctrl+="), KeyPlatform::linux), "Ctrl+=");
    EXPECT_EQ(key_chord_label(KeyChord{}, KeyPlatform::linux), "");
}

TEST(KeyChordTest, AnEmptyTextIsAnError) {
    EXPECT_NE(error_of("").find("at 0"), std::string::npos);
    EXPECT_NE(error_of("   ").find("at 0"), std::string::npos);
    EXPECT_NE(error_of("\t").find("no key"), std::string::npos);
}

TEST(KeyChordTest, AnUnknownKeyIsAnErrorThatGivesItsPosition) {
    EXPECT_NE(error_of("foo").find("at 0"), std::string::npos);
    EXPECT_NE(error_of("foo").find("unknown key 'foo'"), std::string::npos);
    EXPECT_NE(error_of("ctrl+foo").find("at 5"), std::string::npos);
    EXPECT_NE(error_of("ctrl+k foo").find("at 7"), std::string::npos);
    EXPECT_NE(error_of("ctrl+k  ctrl+leftarrow").find("at 13"), std::string::npos);
    EXPECT_NE(error_of("  ctrl+f25").find("at 7"), std::string::npos);
    EXPECT_NE(error_of("ctrl+é").find("at 5"), std::string::npos);
}

TEST(KeyChordTest, AnUnknownModifierIsAnErrorThatNamesIt) {
    const std::string message = error_of("foo+k");
    EXPECT_NE(message.find("unknown modifier 'foo'"), std::string::npos);
    EXPECT_NE(message.find("at 0"), std::string::npos);
    EXPECT_NE(error_of("ctrl+foo+k").find("at 5"), std::string::npos);
    EXPECT_NE(error_of("ctrl+foo+k").find("unknown modifier 'foo'"), std::string::npos);
    EXPECT_NE(error_of("ctrl+k  hyper-k").find("at 8"), std::string::npos);
}

TEST(KeyChordTest, AModifierWithoutAKeyIsAnError) {
    EXPECT_NE(error_of("ctrl+").find("at 5"), std::string::npos);
    EXPECT_NE(error_of("ctrl+").find("not followed by a key"), std::string::npos);
    EXPECT_NE(error_of("ctrl+shift+").find("at 11"), std::string::npos);
    EXPECT_NE(error_of("ctrl+k alt-").find("at 11"), std::string::npos);
    EXPECT_NE(error_of("ctrl+ k").find("at 5"), std::string::npos);
}

TEST(KeyChordTest, AFifthStepIsAnError) {
    EXPECT_TRUE(parse_key_chord("a b c d").has_value());
    const std::string message = error_of("a b c d e");
    EXPECT_NE(message.find("at 8"), std::string::npos);
    EXPECT_NE(message.find("at most 4"), std::string::npos);
    EXPECT_NE(error_of("ctrl+k ctrl+k ctrl+k ctrl+k ctrl+k").find("at 28"), std::string::npos);
}

TEST(KeyChordTest, ThePlusSignAloneIsNotAKey) {
    EXPECT_NE(error_of("+").find("unknown key"), std::string::npos);
    EXPECT_NE(error_of("ctrl++").find("unknown key"), std::string::npos);
    EXPECT_TRUE(parse_key_chord("ctrl+-").has_value());
    EXPECT_TRUE(parse_key_chord("ctrl--").has_value());
}

TEST(KeyChordTest, KeysByPositionAndNamesOfTheInterfaceAreNotKeyBindingNames) {
    EXPECT_FALSE(parse_key_chord("[KeyK]").has_value());
    EXPECT_FALSE(parse_key_chord("leftarrow").has_value());
    EXPECT_FALSE(parse_key_chord("del").has_value());
    EXPECT_FALSE(parse_key_chord("keyincomposition").has_value());
}

TEST(KeyChordReplayTest, TheCasesRecordedFromVsCodeParseAndFormatTheSame) {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/key_chord_cases.json");
    ASSERT_TRUE(file.is_open());
    const json recorded = json::parse(file);
    ASSERT_GT(recorded["cases"].size(), 1000U);

    // What the port does on purpose differently: it accepts `super` for meta and any white space
    // between steps, and it allows four steps at most.
    const auto differs_on_purpose = [](const std::string& input) {
        std::istringstream words(input);
        std::string word;
        std::size_t steps = 0;
        while (words >> word) {
            ++steps;
        }
        return input.find("super") != std::string::npos ||
               input.find_first_of("\t\n\r") != std::string::npos || steps > KeyChord::max_steps;
    };

    std::size_t compared = 0;
    for (const json& recorded_case : recorded["cases"]) {
        const std::string input = recorded_case["input"];
        if (differs_on_purpose(input)) {
            continue;
        }
        const auto chord = parse_key_chord(input);
        if (recorded_case["steps"].is_null()) {
            EXPECT_FALSE(chord.has_value()) << "[" << input << "] should fail";
            continue;
        }
        ASSERT_TRUE(chord.has_value()) << "[" << input << "] " << chord.error().message;
        ++compared;
        const json& steps = recorded_case["steps"];
        ASSERT_EQ(chord->steps.size(), steps.size()) << input;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const KeyStep& step = chord->steps[i];
            EXPECT_EQ(step.modifiers.ctrl, steps[i]["ctrl"].get<bool>()) << input;
            EXPECT_EQ(step.modifiers.shift, steps[i]["shift"].get<bool>()) << input;
            EXPECT_EQ(step.modifiers.alt, steps[i]["alt"].get<bool>()) << input;
            EXPECT_EQ(step.modifiers.meta, steps[i]["meta"].get<bool>()) << input;
            EXPECT_EQ(key_info(step.key).setting, steps[i]["key"].get<std::string>()) << input;
        }
        if (recorded_case.contains("duplicate_modifier")) {
            continue;
        }
        const std::pair<const char*, KeyPlatform> platforms[] = {{"linux", KeyPlatform::linux},
                                                                 {"windows", KeyPlatform::windows},
                                                                 {"macos", KeyPlatform::macos}};
        for (const auto& [name, platform] : platforms) {
            EXPECT_EQ(format_key_chord(*chord, platform),
                      recorded_case["settings"][name].get<std::string>())
                << input << " on " << name;
            EXPECT_EQ(key_chord_label(*chord, platform),
                      recorded_case["labels"][name].get<std::string>())
                << input << " on " << name;
        }
    }
    EXPECT_GT(compared, 1000U);
}
