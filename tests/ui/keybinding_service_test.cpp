#include "anvil/ui/keybinding_service.hpp"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QSignalSpy>
#include <gtest/gtest.h>
#include <map>
#include <string>

using namespace anvil::ui;
using namespace anvil::core;

namespace {

Keybinding binding(std::string_view keys, std::string command, std::string_view when = "") {
    Keybinding result;
    result.chord = *parse_key_chord(keys);
    result.command = std::move(command);
    if (!when.empty()) {
        result.when = *WhenExpression::parse(when);
    }
    return result;
}

struct Recorder {
    std::vector<std::string> commands;
    std::vector<std::optional<nlohmann::json>> args;
    std::map<std::string, bool> context;
    bool composing = false;

    KeybindingService::Hooks hooks() {
        return {.fill_context =
                    [this](ContextKeys& keys) {
                        for (const auto& [key, value] : context) {
                            keys.set(key, value);
                        }
                    },
                .run =
                    [this](const std::string& command, const nlohmann::json* arguments) {
                        commands.push_back(command);
                        args.push_back(arguments ? std::optional(*arguments) : std::nullopt);
                    },
                .composing = [this] { return composing; }};
    }
};

QKeyEvent press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                QEvent::Type type = QEvent::KeyPress) {
    return QKeyEvent(type, key, modifiers);
}

} // namespace

TEST(KeybindingServiceTest, ABoundPressRunsItsCommandAndIsHandled) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    EXPECT_TRUE(service.handle_press(press(Qt::Key_S, Qt::ControlModifier)));
    EXPECT_EQ(recorder.commands, std::vector<std::string>{"save"});
}

TEST(KeybindingServiceTest, AnUnboundPressIsLeftToTheWidget) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    EXPECT_FALSE(service.handle_press(press(Qt::Key_A)));
    EXPECT_FALSE(service.handle_press(press(Qt::Key_S)));
    EXPECT_FALSE(service.handle_press(press(Qt::Key_Control, Qt::ControlModifier)));
    EXPECT_TRUE(recorder.commands.empty());
}

TEST(KeybindingServiceTest, TheWhenOfABindingReadsTheContextOfThePress) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("left", "cursorLeft", "editorTextFocus")});

    EXPECT_FALSE(service.handle_press(press(Qt::Key_Left)));
    recorder.context["editorTextFocus"] = true;
    EXPECT_TRUE(service.handle_press(press(Qt::Key_Left)));
    EXPECT_EQ(recorder.commands, std::vector<std::string>{"cursorLeft"});
}

TEST(KeybindingServiceTest, TheArgumentsOfABindingGoToTheCommand) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    Keybinding with_args = binding("ctrl+k", "type");
    with_args.args = nlohmann::json{{"text", "x"}};
    service.set_bindings({with_args, binding("ctrl+s", "save")});

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    service.handle_press(press(Qt::Key_S, Qt::ControlModifier));
    ASSERT_EQ(recorder.args.size(), 2U);
    EXPECT_EQ(recorder.args[0], (nlohmann::json{{"text", "x"}}));
    EXPECT_FALSE(recorder.args[1]);
}

TEST(KeybindingServiceTest, AKeyThatBubblesRunsTheCommandAndStillReachesTheWidget) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "^save")});
    EXPECT_FALSE(service.handle_press(press(Qt::Key_S, Qt::ControlModifier)));
    EXPECT_EQ(recorder.commands, std::vector<std::string>{"save"});
}

TEST(KeybindingServiceTest, ExtensionsSetContextKeysThatBindingsRead) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save", "myExt.active"),
                          binding("ctrl+o", "open", "myExt.mode == 'a'"),
                          binding("ctrl+p", "pick", "myExt.number == 3")});

    EXPECT_FALSE(service.handle_press(press(Qt::Key_S, Qt::ControlModifier)));
    service.set_extension_context("myExt.active", true);
    service.set_extension_context("myExt.mode", "a");
    service.set_extension_context("myExt.number", 3);
    EXPECT_TRUE(service.handle_press(press(Qt::Key_S, Qt::ControlModifier)));
    EXPECT_TRUE(service.handle_press(press(Qt::Key_O, Qt::ControlModifier)));
    EXPECT_TRUE(service.handle_press(press(Qt::Key_P, Qt::ControlModifier)));

    service.set_extension_context("myExt.active", nullptr);
    service.set_extension_context("myExt.mode", "b");
    EXPECT_FALSE(service.handle_press(press(Qt::Key_S, Qt::ControlModifier)));
    EXPECT_FALSE(service.handle_press(press(Qt::Key_O, Qt::ControlModifier)));
}

TEST(KeybindingServiceTest, AChordShowsAMessageUntilItIsComplete) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    EXPECT_TRUE(service.handle_press(press(Qt::Key_K, Qt::ControlModifier)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toString(),
              QStringLiteral("(Ctrl+K) was pressed. Waiting for second key of chord..."));
    EXPECT_EQ(spy[0][1].toInt(), 0);
    EXPECT_TRUE(recorder.commands.empty());

    EXPECT_TRUE(service.handle_press(press(Qt::Key_C, Qt::ControlModifier)));
    EXPECT_EQ(recorder.commands, std::vector<std::string>{"comment"});
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy[1][0].toString(), QString());
}

TEST(KeybindingServiceTest, AChordOfThreeKeysWaitsForTheNextKey) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c ctrl+d", "deep")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    service.handle_press(press(Qt::Key_C, Qt::ControlModifier));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy[1][0].toString(),
              QStringLiteral("(Ctrl+K, Ctrl+C) was pressed. Waiting for next key of chord..."));
}

TEST(KeybindingServiceTest, AKeyThatDoesNotContinueAChordSaysSoAndIsSwallowed) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    EXPECT_TRUE(service.handle_press(press(Qt::Key_X, Qt::ControlModifier)));
    EXPECT_TRUE(recorder.commands.empty());
    ASSERT_EQ(spy.count(), 3);
    EXPECT_EQ(spy[1][0].toString(), QString());
    EXPECT_EQ(spy[2][0].toString(),
              QStringLiteral("The key combination (Ctrl+K, Ctrl+X) is not a command."));
    EXPECT_EQ(spy[2][1].toInt(), 10000);
}

TEST(KeybindingServiceTest, AChordEndsAfterFiveSeconds) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    const auto now = std::chrono::steady_clock::now();
    service.check_timeout(now + std::chrono::seconds(1));
    EXPECT_EQ(spy.count(), 1);
    service.check_timeout(now + std::chrono::seconds(6));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy[1][0].toString(), QString());

    EXPECT_FALSE(service.handle_press(press(Qt::Key_C, Qt::ControlModifier)));
    EXPECT_TRUE(recorder.commands.empty());
}

TEST(KeybindingServiceTest, ReplacingTheBindingsDropsAChordInProgress) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy[1][0].toString(), QString());
    EXPECT_FALSE(service.handle_press(press(Qt::Key_C, Qt::ControlModifier)));
}

TEST(KeybindingServiceTest, LosingTheApplicationFocusDropsAChordInProgress) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+k ctrl+c", "comment")});
    QSignalSpy spy(&service, &KeybindingService::chord_message);

    service.handle_press(press(Qt::Key_K, Qt::ControlModifier));
    emit qApp->applicationStateChanged(Qt::ApplicationInactive);
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy[1][0].toString(), QString());
}

TEST(KeybindingServiceTest, TheShortcutLabelIsThePrimaryKeyWhoseConditionHolds) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+y", "redo"), binding("ctrl+shift+z", "redo"),
                          binding("ctrl+h", "replace", "inEditor"),
                          binding("ctrl+r", "replace", "inTerminal")});
    EXPECT_EQ(service.shortcut_label("redo"), QStringLiteral("Ctrl+Shift+Z"));
    EXPECT_EQ(service.shortcut_label("unbound"), QString());

    recorder.context["inEditor"] = true;
    EXPECT_EQ(service.shortcut_label("replace"), QStringLiteral("Ctrl+H"));
    recorder.context["inEditor"] = false;
    recorder.context["inTerminal"] = true;
    EXPECT_EQ(service.shortcut_label("replace"), QStringLiteral("Ctrl+R"));
}

TEST(KeybindingServiceTest, TheMacLabelUsesTheSymbolsOfTheMac) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::macos);
    service.set_bindings({binding("cmd+s", "save")});
    EXPECT_EQ(service.shortcut_label("save"), QStringLiteral("⌘S"));
}

TEST(KeybindingServiceTest, TheEventFilterHandlesAPressOnAWidget) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    QLineEdit target;

    QKeyEvent event = press(Qt::Key_S, Qt::ControlModifier);
    EXPECT_TRUE(service.eventFilter(&target, &event));
    EXPECT_EQ(recorder.commands, std::vector<std::string>{"save"});

    QKeyEvent typed = press(Qt::Key_A);
    EXPECT_FALSE(service.eventFilter(&target, &typed));
}

TEST(KeybindingServiceTest, TheEventFilterIgnoresAnObjectThatIsNotAWidget) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    QObject target;

    QKeyEvent event = press(Qt::Key_S, Qt::ControlModifier);
    EXPECT_FALSE(service.eventFilter(&target, &event));
    EXPECT_TRUE(recorder.commands.empty());
}

TEST(KeybindingServiceTest, TheEventFilterIgnoresAnythingButKeyPresses) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    QLineEdit target;

    QKeyEvent release = press(Qt::Key_S, Qt::ControlModifier, QEvent::KeyRelease);
    EXPECT_FALSE(service.eventFilter(&target, &release));
    QEvent other(QEvent::FocusIn);
    EXPECT_FALSE(service.eventFilter(&target, &other));
    EXPECT_TRUE(recorder.commands.empty());
}

TEST(KeybindingServiceTest, APressIsLeftAloneWhileAModalDialogIsOpen) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save")});
    QDialog dialog;
    dialog.setModal(true);
    dialog.show();
    ASSERT_NE(QApplication::activeModalWidget(), nullptr);

    QKeyEvent event = press(Qt::Key_S, Qt::ControlModifier);
    EXPECT_FALSE(service.eventFilter(&dialog, &event));
    EXPECT_TRUE(recorder.commands.empty());
    dialog.hide();

    EXPECT_TRUE(service.eventFilter(&dialog, &event));
}

TEST(KeybindingServiceTest, APressIsLeftAloneWhileAnInputMethodComposes) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("left", "cursorLeft")});
    QLineEdit target;
    recorder.composing = true;

    QKeyEvent event = press(Qt::Key_Left);
    EXPECT_FALSE(service.eventFilter(&target, &event));
    EXPECT_TRUE(recorder.commands.empty());
    recorder.composing = false;
    EXPECT_TRUE(service.eventFilter(&target, &event));
}

TEST(KeybindingServiceTest, ABoundKeyIsClaimedFromTheToolkitShortcutsBeforeItsPressArrives) {
    Recorder recorder;
    KeybindingService service(recorder.hooks(), KeyPlatform::linux);
    service.set_bindings({binding("ctrl+s", "save"), binding("ctrl+k ctrl+c", "comment")});
    QLineEdit target;

    QKeyEvent bound = press(Qt::Key_S, Qt::ControlModifier, QEvent::ShortcutOverride);
    bound.ignore();
    EXPECT_FALSE(service.eventFilter(&target, &bound));
    EXPECT_TRUE(bound.isAccepted());
    EXPECT_TRUE(recorder.commands.empty());

    QKeyEvent chord_start = press(Qt::Key_K, Qt::ControlModifier, QEvent::ShortcutOverride);
    chord_start.ignore();
    service.eventFilter(&target, &chord_start);
    EXPECT_TRUE(chord_start.isAccepted());

    QKeyEvent unbound = press(Qt::Key_A, Qt::NoModifier, QEvent::ShortcutOverride);
    unbound.ignore();
    service.eventFilter(&target, &unbound);
    EXPECT_FALSE(unbound.isAccepted());
}
