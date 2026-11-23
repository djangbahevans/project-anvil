#include "anvil/ui/key_translation.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace anvil::ui;
using anvil::core::Key;
using anvil::core::KeyPlatform;
using anvil::core::KeyStep;
using anvil::core::Modifiers;

namespace {

std::optional<KeyStep> translate(int key, Qt::KeyboardModifiers modifiers = {},
                                 KeyPlatform platform = KeyPlatform::linux, quint32 scan_code = 0) {
    const QKeyEvent event(QEvent::KeyPress, key, modifiers, scan_code, 0, 0, QString());
    return key_step_from_event(event, platform);
}

} // namespace

TEST(KeyTranslationTest, LettersAndDigitsAreTheirKeys) {
    EXPECT_EQ(translate(Qt::Key_A), (KeyStep{.key = Key::key_a}));
    EXPECT_EQ(translate(Qt::Key_Z), (KeyStep{.key = Key::key_z}));
    EXPECT_EQ(translate(Qt::Key_0), (KeyStep{.key = Key::digit0}));
    EXPECT_EQ(translate(Qt::Key_9), (KeyStep{.key = Key::digit9}));
}

TEST(KeyTranslationTest, FunctionKeysAndNavigationKeys) {
    EXPECT_EQ(translate(Qt::Key_F1), (KeyStep{.key = Key::f1}));
    EXPECT_EQ(translate(Qt::Key_F12), (KeyStep{.key = Key::f12}));
    EXPECT_EQ(translate(Qt::Key_F24), (KeyStep{.key = Key::f24}));
    EXPECT_EQ(translate(Qt::Key_Escape), (KeyStep{.key = Key::escape}));
    EXPECT_EQ(translate(Qt::Key_Return), (KeyStep{.key = Key::enter}));
    EXPECT_EQ(translate(Qt::Key_Enter), (KeyStep{.key = Key::enter}));
    EXPECT_EQ(translate(Qt::Key_Backspace), (KeyStep{.key = Key::backspace}));
    EXPECT_EQ(translate(Qt::Key_Delete), (KeyStep{.key = Key::delete_key}));
    EXPECT_EQ(translate(Qt::Key_Space), (KeyStep{.key = Key::space}));
    EXPECT_EQ(translate(Qt::Key_PageUp), (KeyStep{.key = Key::page_up}));
    EXPECT_EQ(translate(Qt::Key_Left), (KeyStep{.key = Key::left_arrow}));
    EXPECT_EQ(translate(Qt::Key_Down), (KeyStep{.key = Key::down_arrow}));
    EXPECT_EQ(translate(Qt::Key_Home), (KeyStep{.key = Key::home}));
    EXPECT_EQ(translate(Qt::Key_Insert), (KeyStep{.key = Key::insert}));
    EXPECT_EQ(translate(Qt::Key_Menu), (KeyStep{.key = Key::context_menu}));
}

TEST(KeyTranslationTest, PunctuationKeysAreTheKeysOfAUsKeyboard) {
    EXPECT_EQ(translate(Qt::Key_Minus), (KeyStep{.key = Key::minus}));
    EXPECT_EQ(translate(Qt::Key_Equal), (KeyStep{.key = Key::equal}));
    EXPECT_EQ(translate(Qt::Key_BracketLeft), (KeyStep{.key = Key::bracket_left}));
    EXPECT_EQ(translate(Qt::Key_Backslash), (KeyStep{.key = Key::backslash}));
    EXPECT_EQ(translate(Qt::Key_Semicolon), (KeyStep{.key = Key::semicolon}));
    EXPECT_EQ(translate(Qt::Key_Apostrophe), (KeyStep{.key = Key::quote}));
    EXPECT_EQ(translate(Qt::Key_QuoteLeft), (KeyStep{.key = Key::backquote}));
    EXPECT_EQ(translate(Qt::Key_Comma), (KeyStep{.key = Key::comma}));
    EXPECT_EQ(translate(Qt::Key_Period), (KeyStep{.key = Key::period}));
    EXPECT_EQ(translate(Qt::Key_Slash), (KeyStep{.key = Key::slash}));
}

TEST(KeyTranslationTest, TheCharactersOfShiftedKeysGoBackToTheirKeys) {
    struct Case {
        int qt;
        Key key;
    };
    for (const Case& c : {Case{Qt::Key_Exclam, Key::digit1},
                          Case{Qt::Key_At, Key::digit2},
                          Case{Qt::Key_NumberSign, Key::digit3},
                          Case{Qt::Key_Dollar, Key::digit4},
                          Case{Qt::Key_Percent, Key::digit5},
                          Case{Qt::Key_AsciiCircum, Key::digit6},
                          Case{Qt::Key_Ampersand, Key::digit7},
                          Case{Qt::Key_Asterisk, Key::digit8},
                          Case{Qt::Key_ParenLeft, Key::digit9},
                          Case{Qt::Key_ParenRight, Key::digit0},
                          Case{Qt::Key_Underscore, Key::minus},
                          Case{Qt::Key_Plus, Key::equal},
                          Case{Qt::Key_BraceLeft, Key::bracket_left},
                          Case{Qt::Key_BraceRight, Key::bracket_right},
                          Case{Qt::Key_Bar, Key::backslash},
                          Case{Qt::Key_Colon, Key::semicolon},
                          Case{Qt::Key_QuoteDbl, Key::quote},
                          Case{Qt::Key_AsciiTilde, Key::backquote},
                          Case{Qt::Key_Less, Key::comma},
                          Case{Qt::Key_Greater, Key::period},
                          Case{Qt::Key_Question, Key::slash}}) {
        const auto step = translate(c.qt, Qt::ShiftModifier);
        ASSERT_TRUE(step.has_value()) << c.qt;
        EXPECT_EQ(step->key, c.key) << c.qt;
        EXPECT_TRUE(step->modifiers.shift);
    }
}

TEST(KeyTranslationTest, TheKeypadIsToldByItsModifier) {
    EXPECT_EQ(translate(Qt::Key_5, Qt::KeypadModifier), (KeyStep{.key = Key::numpad5}));
    EXPECT_EQ(translate(Qt::Key_0, Qt::KeypadModifier), (KeyStep{.key = Key::numpad0}));
    EXPECT_EQ(translate(Qt::Key_5), (KeyStep{.key = Key::digit5}));
    EXPECT_EQ(translate(Qt::Key_Plus, Qt::KeypadModifier), (KeyStep{.key = Key::numpad_add}));
    EXPECT_EQ(translate(Qt::Key_Minus, Qt::KeypadModifier), (KeyStep{.key = Key::numpad_subtract}));
    EXPECT_EQ(translate(Qt::Key_Asterisk, Qt::KeypadModifier),
              (KeyStep{.key = Key::numpad_multiply}));
    EXPECT_EQ(translate(Qt::Key_Slash, Qt::KeypadModifier), (KeyStep{.key = Key::numpad_divide}));
    EXPECT_EQ(translate(Qt::Key_Period, Qt::KeypadModifier), (KeyStep{.key = Key::numpad_decimal}));
    EXPECT_EQ(translate(Qt::Key_Enter, Qt::KeypadModifier), (KeyStep{.key = Key::enter}));
}

TEST(KeyTranslationTest, ModifiersAreKeptOnLinuxAndWindows) {
    for (const KeyPlatform platform : {KeyPlatform::linux, KeyPlatform::windows}) {
        const auto step = translate(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier, platform);
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->modifiers, (Modifiers{.ctrl = true, .shift = true}));
        const auto all = translate(
            Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier,
            platform);
        EXPECT_EQ(all->modifiers,
                  (Modifiers{.ctrl = true, .shift = true, .alt = true, .meta = true}));
    }
}

TEST(KeyTranslationTest, OnMacOsQtsControlIsCommandAndQtsMetaIsControl) {
    const auto command = translate(Qt::Key_S, Qt::ControlModifier, KeyPlatform::macos);
    EXPECT_EQ(command->modifiers, (Modifiers{.meta = true}));
    const auto control = translate(Qt::Key_S, Qt::MetaModifier, KeyPlatform::macos);
    EXPECT_EQ(control->modifiers, (Modifiers{.ctrl = true}));
    const auto both = translate(Qt::Key_S, Qt::MetaModifier | Qt::ControlModifier | Qt::AltModifier,
                                KeyPlatform::macos);
    EXPECT_EQ(both->modifiers, (Modifiers{.ctrl = true, .alt = true, .meta = true}));
}

TEST(KeyTranslationTest, ShiftTabIsTheTabKeyWithShift) {
    const auto step = translate(Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(step, (KeyStep{.modifiers = {.shift = true}, .key = Key::tab}));
    EXPECT_EQ(translate(Qt::Key_Backtab)->modifiers, (Modifiers{.shift = true}));
}

TEST(KeyTranslationTest, AModifierPressedAloneIsTheKeyAndNotAHeldModifier) {
    EXPECT_EQ(translate(Qt::Key_Control, Qt::ControlModifier), (KeyStep{.key = Key::ctrl}));
    EXPECT_EQ(translate(Qt::Key_Shift, Qt::ShiftModifier), (KeyStep{.key = Key::shift}));
    EXPECT_EQ(translate(Qt::Key_Alt, Qt::AltModifier), (KeyStep{.key = Key::alt}));
    EXPECT_EQ(translate(Qt::Key_Meta, Qt::MetaModifier), (KeyStep{.key = Key::meta}));
    EXPECT_EQ(translate(Qt::Key_Control, Qt::ControlModifier | Qt::ShiftModifier),
              (KeyStep{.modifiers = {.shift = true}, .key = Key::ctrl}));
}

TEST(KeyTranslationTest, AKeyThatBindingsCannotNameIsNothing) {
    EXPECT_EQ(translate(Qt::Key_unknown), std::nullopt);
    EXPECT_EQ(translate(Qt::Key_Eacute), std::nullopt);
    EXPECT_EQ(translate(Qt::Key_VolumeUp), std::nullopt);
    EXPECT_EQ(translate(0x1000000 - 1), std::nullopt);
}

TEST(KeyTranslationTest, QtKeysGoToKeys) {
    EXPECT_EQ(key_from_qt(Qt::Key_K, {}), Key::key_k);
    EXPECT_EQ(key_from_qt(Qt::Key_Dollar, Qt::ShiftModifier), Key::digit4);
    EXPECT_EQ(key_from_qt(Qt::Key_Plus, Qt::KeypadModifier), Key::numpad_add);
    EXPECT_EQ(key_from_qt(Qt::Key_Plus, {}), Key::equal);
}

#ifdef Q_OS_LINUX
TEST(KeyTranslationTest, OnLinuxTheKeyAtAPlaceWinsOverTheCharacterItMakes) {
    // The key at the place of Q on an AZERTY keyboard makes `a`.
    EXPECT_EQ(translate(Qt::Key_A, Qt::ControlModifier, KeyPlatform::linux, 16 + 8)->key,
              Key::key_q);
    // The key at the place of `[` on a layout that makes `ü` there.
    EXPECT_EQ(translate(Qt::Key_Udiaeresis, {}, KeyPlatform::linux, 26 + 8)->key,
              Key::bracket_left);
    EXPECT_EQ(translate(Qt::Key_Z, {}, KeyPlatform::linux, 44 + 8)->key, Key::key_z);
    EXPECT_EQ(translate(Qt::Key_1, {}, KeyPlatform::linux, 2 + 8)->key, Key::digit1);
    EXPECT_EQ(translate(Qt::Key_Equal, {}, KeyPlatform::linux, 13 + 8)->key, Key::equal);
    EXPECT_EQ(translate(Qt::Key_Slash, {}, KeyPlatform::linux, 53 + 8)->key, Key::slash);
    EXPECT_EQ(translate(Qt::Key_Backslash, {}, KeyPlatform::linux, 43 + 8)->key, Key::backslash);
}

TEST(KeyTranslationTest, OnLinuxAKeyWithNoPlaceInTheTableIsToldByTheCharacterItMakes) {
    EXPECT_EQ(translate(Qt::Key_Escape, {}, KeyPlatform::linux, 9)->key, Key::escape);
    EXPECT_EQ(translate(Qt::Key_F5, {}, KeyPlatform::linux, 71)->key, Key::f5);
    EXPECT_EQ(translate(Qt::Key_K, {}, KeyPlatform::linux, 3)->key, Key::key_k);
}

TEST(KeyTranslationTest, ThePlaceOfAKeyIsOnlyReadOnLinux) {
    EXPECT_EQ(translate(Qt::Key_A, {}, KeyPlatform::windows, 16 + 8)->key, Key::key_a);
    EXPECT_EQ(translate(Qt::Key_A, {}, KeyPlatform::macos, 16 + 8)->key, Key::key_a);
}
#endif
