#include "anvil/ui/command_registry.hpp"

#include <gtest/gtest.h>

using anvil::ui::CommandRegistry;
using nlohmann::json;

TEST(CommandRegistryTest, ExecutesACommandByIdWithItsArguments) {
    CommandRegistry registry;
    json received;
    registry.add("test.run", "Run", [&](const json& args) { received = args; });

    EXPECT_TRUE(registry.execute("test.run", json::array({1, "two"})));
    EXPECT_EQ(received, json::array({1, "two"}));
}

TEST(CommandRegistryTest, ExecutingWithoutArgumentsPassesAnEmptyArray) {
    CommandRegistry registry;
    json received;
    registry.add("test.run", "Run", [&](const json& args) { received = args; });

    registry.execute("test.run");
    EXPECT_EQ(received, json::array());
}

TEST(CommandRegistryTest, AnUnknownIdIsNotExecuted) {
    CommandRegistry registry;
    int calls = 0;
    registry.add("test.run", "Run", [&](const json&) { ++calls; });

    EXPECT_FALSE(registry.execute("test.other"));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(registry.find("test.other"), nullptr);
}

TEST(CommandRegistryTest, KeepsRegistrationOrderAndTitles) {
    CommandRegistry registry;
    registry.add("b", "Bee", [](const json&) {});
    registry.add("a", "Ay", [](const json&) {});

    ASSERT_EQ(registry.commands().size(), 2U);
    EXPECT_EQ(registry.commands()[0].id, "b");
    EXPECT_EQ(registry.commands()[1].id, "a");
    ASSERT_NE(registry.find("a"), nullptr);
    EXPECT_EQ(registry.find("a")->title, "Ay");
}

TEST(CommandRegistryTest, AddingAnExistingIdReplacesItInPlace) {
    CommandRegistry registry;
    int first = 0;
    int second = 0;
    registry.add("one", "One", [&](const json&) { ++first; });
    registry.add("two", "Two", [](const json&) {});
    registry.add("one", "Uno", [&](const json&) { ++second; });

    ASSERT_EQ(registry.commands().size(), 2U);
    EXPECT_EQ(registry.commands()[0].id, "one");
    EXPECT_EQ(registry.commands()[0].title, "Uno");
    registry.execute("one");
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
}

TEST(CommandRegistryTest, AHandlerMayRegisterCommands) {
    CommandRegistry registry;
    int later = 0;
    registry.add("first", "First", [&](const json&) {
        for (int i = 0; i < 100; ++i) {
            registry.add("extra." + std::to_string(i), "Extra", [&](const json&) { ++later; });
        }
    });

    EXPECT_TRUE(registry.execute("first"));
    EXPECT_TRUE(registry.execute("extra.99"));
    EXPECT_EQ(later, 1);
}
