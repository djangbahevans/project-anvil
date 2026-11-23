#include "anvil/core/key_dispatcher.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace anvil::core;
using Status = KeyDispatch::Status;
using namespace std::chrono_literals;

namespace {

KeyChord chord_of(std::string_view text) {
    auto chord = parse_key_chord(text);
    EXPECT_TRUE(chord.has_value()) << text;
    return chord ? *chord : KeyChord{};
}

Keybinding binding(std::string_view key, std::string command, std::string_view when = {}) {
    Keybinding result;
    result.chord = chord_of(key);
    result.command = std::move(command);
    if (!when.empty()) {
        result.when = *WhenExpression::parse(when);
    }
    return result;
}

KeyStep step_of(std::string_view text) {
    const KeyChord chord = chord_of(text);
    return chord.steps.front();
}

// A dispatcher with a clock that the test moves.
class KeyDispatcherTest : public ::testing::Test {
protected:
    void use(std::vector<Keybinding> bindings) {
        table_ = KeybindingTable(std::move(bindings));
        dispatcher_ = std::make_unique<KeyDispatcher>(table_, KeyPlatform::linux);
    }

    KeyDispatch press(std::string_view key, std::chrono::milliseconds after = 0ms) {
        now_ += after;
        return dispatcher_->dispatch(step_of(key), context_, now_);
    }

    KeybindingTable table_;
    std::unique_ptr<KeyDispatcher> dispatcher_;
    ContextKeys context_;
    KeyDispatcher::Time now_{};
};

} // namespace

TEST_F(KeyDispatcherTest, ABoundKeyRunsItsCommandAndIsHandled) {
    use({binding("ctrl+s", "save")});
    const KeyDispatch result = press("ctrl+s");
    EXPECT_TRUE(result.handled);
    EXPECT_EQ(result.command, "save");
    EXPECT_EQ(result.status, Status::unchanged);
    EXPECT_FALSE(dispatcher_->in_chord());
}

TEST_F(KeyDispatcherTest, AKeyThatIsBoundToNothingIsLeftToTheWidgets) {
    use({binding("ctrl+s", "save")});
    const KeyDispatch result = press("a");
    EXPECT_FALSE(result.handled);
    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.status, Status::unchanged);
}

TEST_F(KeyDispatcherTest, ACommandMarkedWithACaretRunsAndLeavesThePressToTheWidgets) {
    use({binding("ctrl+s", "^save")});
    const KeyDispatch result = press("ctrl+s");
    EXPECT_FALSE(result.handled);
    EXPECT_EQ(result.command, "save");
}

TEST_F(KeyDispatcherTest, ArgumentsAreHandedOn) {
    Keybinding with_args = binding("ctrl+k", "type");
    with_args.args = nlohmann::json{{"text", "x"}};
    use({with_args});
    const KeyDispatch result = press("ctrl+k");
    ASSERT_TRUE(result.args.has_value());
    EXPECT_EQ((*result.args)["text"], "x");
}

TEST_F(KeyDispatcherTest, TheWhenOfABindingIsReadFromTheContextAtThePress) {
    use({binding("tab", "indent", "editorTextFocus")});
    EXPECT_FALSE(press("tab").command.has_value());
    context_.set("editorTextFocus", true);
    EXPECT_EQ(press("tab").command, "indent");
}

TEST_F(KeyDispatcherTest, TheFirstKeyOfAChordWaitsAndTheSecondRunsTheCommand) {
    use({binding("ctrl+k ctrl+c", "comment")});
    const KeyDispatch first = press("ctrl+k");
    EXPECT_TRUE(first.handled);
    EXPECT_FALSE(first.command.has_value());
    EXPECT_EQ(first.status, Status::waiting);
    EXPECT_EQ(first.pending, (std::vector<std::string>{"Ctrl+K"}));
    EXPECT_TRUE(dispatcher_->in_chord());

    const KeyDispatch second = press("ctrl+c", 100ms);
    EXPECT_TRUE(second.handled);
    EXPECT_EQ(second.command, "comment");
    EXPECT_EQ(second.status, Status::closed);
    EXPECT_FALSE(dispatcher_->in_chord());
}

TEST_F(KeyDispatcherTest, AThreeStepChordReportsEachStepPressed) {
    use({binding("a b c", "abc")});
    EXPECT_EQ(press("a").pending, (std::vector<std::string>{"A"}));
    const KeyDispatch second = press("b");
    EXPECT_EQ(second.status, Status::waiting);
    EXPECT_EQ(second.pending, (std::vector<std::string>{"A", "B"}));
    const KeyDispatch third = press("c");
    EXPECT_EQ(third.command, "abc");
    EXPECT_EQ(third.status, Status::closed);
}

TEST_F(KeyDispatcherTest, AKeyThatContinuesNoChordEndsItAndIsSwallowed) {
    use({binding("ctrl+k ctrl+x", "chord"), binding("backspace", "simple")});
    ASSERT_EQ(press("ctrl+k").status, Status::waiting);
    const KeyDispatch result = press("backspace");
    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.status, Status::no_command);
    EXPECT_EQ(result.pending, (std::vector<std::string>{"Ctrl+K"}));
    EXPECT_EQ(result.key_label, "Backspace");
    EXPECT_FALSE(dispatcher_->in_chord());

    const KeyDispatch after = press("backspace");
    EXPECT_TRUE(after.handled);
    EXPECT_EQ(after.command, "simple");
    EXPECT_EQ(after.status, Status::unchanged);
}

TEST_F(KeyDispatcherTest, EscapeCancelsAChordWithoutReachingTheEditor) {
    use({binding("ctrl+k ctrl+c", "comment"), binding("escape", "closeFind")});
    ASSERT_EQ(press("ctrl+k").status, Status::waiting);
    const KeyDispatch result = press("escape");
    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.status, Status::no_command);
    EXPECT_EQ(result.key_label, "Escape");
    EXPECT_FALSE(dispatcher_->in_chord());
}

TEST_F(KeyDispatcherTest, ModifierKeysAloneDoNotDispatchAndDoNotEndAChord) {
    use({binding("ctrl+k ctrl+c", "comment")});
    ASSERT_EQ(press("ctrl+k").status, Status::waiting);
    for (const char* modifier : {"ctrl", "shift", "alt", "meta"}) {
        const KeyDispatch result = press(modifier);
        EXPECT_FALSE(result.handled) << modifier;
        EXPECT_EQ(result.status, Status::unchanged) << modifier;
        EXPECT_TRUE(dispatcher_->in_chord()) << modifier;
    }
    EXPECT_EQ(press("ctrl+c").command, "comment");
}

TEST_F(KeyDispatcherTest, AChordThatIsNotContinuedEndsAfterFiveSeconds) {
    use({binding("ctrl+k ctrl+c", "comment")});
    ASSERT_EQ(press("ctrl+k").status, Status::waiting);
    EXPECT_EQ(dispatcher_->check_timeout(now_ + 5000ms).status, Status::unchanged);
    EXPECT_TRUE(dispatcher_->in_chord());
    const KeyDispatch ended = dispatcher_->check_timeout(now_ + 5001ms);
    EXPECT_EQ(ended.status, Status::closed);
    EXPECT_FALSE(ended.handled);
    EXPECT_FALSE(dispatcher_->in_chord());
    EXPECT_EQ(dispatcher_->check_timeout(now_ + 20000ms).status, Status::unchanged);

    const KeyDispatch again = press("ctrl+c", 6000ms);
    EXPECT_FALSE(again.handled);
}

TEST_F(KeyDispatcherTest, EachStepOfAChordGivesTheFiveSecondsAgain) {
    use({binding("a b c", "abc")});
    press("a");
    press("b", 4000ms);
    EXPECT_EQ(dispatcher_->check_timeout(now_ + 4000ms).status, Status::unchanged);
    EXPECT_EQ(dispatcher_->check_timeout(now_ + 5001ms).status, Status::closed);
}

TEST_F(KeyDispatcherTest, LosingTheFocusEndsAChord) {
    use({binding("ctrl+k ctrl+c", "comment")});
    ASSERT_EQ(press("ctrl+k").status, Status::waiting);
    EXPECT_EQ(dispatcher_->cancel().status, Status::closed);
    EXPECT_FALSE(dispatcher_->in_chord());
    EXPECT_EQ(dispatcher_->cancel().status, Status::unchanged);
}

TEST_F(KeyDispatcherTest, ABindingThatIsAlsoAPrefixRunsOrWaitsAsTheWinnerSays) {
    use({binding("ctrl+k", "short"), binding("ctrl+k ctrl+c", "long")});
    EXPECT_EQ(press("ctrl+k").status, Status::waiting);
    EXPECT_EQ(press("ctrl+c").command, "long");

    use({binding("ctrl+k ctrl+c", "long"), binding("ctrl+k", "short")});
    const KeyDispatch result = press("ctrl+k");
    EXPECT_EQ(result.command, "short");
    EXPECT_FALSE(dispatcher_->in_chord());
}

TEST_F(KeyDispatcherTest,
       ASingleKeyBoundToNothingIsLeftToTheWidgetsAndAChordKeyBoundToNothingIsKept) {
    use({binding("ctrl+d", "")});
    const KeyDispatch single = press("ctrl+d");
    EXPECT_FALSE(single.handled);
    EXPECT_FALSE(single.command.has_value());

    use({binding("ctrl+k ctrl+c", "")});
    EXPECT_EQ(press("ctrl+k").status, Status::waiting);
    const KeyDispatch second = press("ctrl+c");
    EXPECT_TRUE(second.handled);
    EXPECT_EQ(second.status, Status::no_command);
    EXPECT_FALSE(second.command.has_value());
    EXPECT_FALSE(dispatcher_->in_chord());
}

TEST_F(KeyDispatcherTest, AChordKeyThatHasACommandRunsItAfterAChord) {
    use({binding("ctrl+k ctrl+c", "chord"), binding("ctrl+c", "copy")});
    EXPECT_EQ(press("ctrl+c").command, "copy");
    EXPECT_EQ(press("ctrl+k").status, Status::waiting);
    EXPECT_EQ(press("ctrl+c").command, "chord");
}

TEST_F(KeyDispatcherTest, ACommandMarkedWithACaretEndsAChordAndLeavesThePressToTheWidgets) {
    use({binding("ctrl+k ctrl+c", "^chord")});
    press("ctrl+k");
    const KeyDispatch result = press("ctrl+c");
    EXPECT_FALSE(result.handled);
    EXPECT_EQ(result.command, "chord");
    EXPECT_EQ(result.status, Status::closed);
}

TEST_F(KeyDispatcherTest, TheLabelsFollowThePlatform) {
    table_ = KeybindingTable({binding("cmd+k cmd+c", "comment")});
    KeyDispatcher mac(table_, KeyPlatform::macos);
    const KeyDispatch result = mac.dispatch(step_of("cmd+k"), context_, now_);
    EXPECT_EQ(result.pending, (std::vector<std::string>{"⌘K"}));
    KeyDispatcher windows(table_, KeyPlatform::windows);
    EXPECT_EQ(windows.dispatch(step_of("cmd+k"), context_, now_).pending,
              (std::vector<std::string>{"Windows+K"}));
}

TEST_F(KeyDispatcherTest, ANewTableIsUsedFromTheNextPress) {
    use({binding("ctrl+a", "old")});
    KeybindingTable replacement({binding("ctrl+a", "new")});
    dispatcher_->set_table(replacement);
    EXPECT_EQ(press("ctrl+a").command, "new");
}
