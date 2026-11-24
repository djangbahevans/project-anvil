#include "anvil/workbench/navigation_history.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace anvil;
using namespace anvil::workbench;

namespace {

constexpr GroupId g1{1};
constexpr GroupId g2{2};

core::Uri file(const char* name) {
    return core::Uri::file(std::string("/work/") + name);
}

NavigationSelection at(std::size_t line, std::size_t column = 0) {
    return {{line, column}, {line, column}};
}

std::vector<std::size_t> lines_of(const NavigationStack& stack) {
    std::vector<std::size_t> lines;
    for (const NavigationEntry& entry : stack.entries()) {
        lines.push_back(entry.selection.line());
    }
    return lines;
}

using Lines = std::vector<std::size_t>;

} // namespace

TEST(NavigationStack, StartsEmptyAndCannotGoAnywhere) {
    NavigationStack stack;
    EXPECT_EQ(stack.current(), nullptr);
    EXPECT_FALSE(stack.can_go_back());
    EXPECT_FALSE(stack.can_go_forward());
    EXPECT_FALSE(stack.can_go_last());
    EXPECT_FALSE(stack.go_back());
    EXPECT_FALSE(stack.go_forward());
    EXPECT_FALSE(stack.go_last());
}

TEST(NavigationStack, TheFirstPlaceIsAnEntryThatHasNothingBeforeIt) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(3), std::nullopt);
    ASSERT_EQ(stack.entries().size(), 1U);
    EXPECT_FALSE(stack.can_go_back());
    EXPECT_FALSE(stack.can_go_forward());
    EXPECT_TRUE(stack.can_go_last());
}

TEST(NavigationStack, MovesWithinTenLinesReplaceTheEntryAndFartherOnesAddOne) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g1, file("a"), at(5), SelectionReason::user);
    stack.notify(g1, file("a"), at(9), SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({9}));

    stack.notify(g1, file("a"), at(19), SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({9, 19}));
}

TEST(NavigationStack, TheDistanceIsMeasuredFromTheLastStateAndNotFromTheEntry) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    for (std::size_t line = 8; line <= 40; line += 8) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    EXPECT_EQ(lines_of(stack), Lines({40}));
}

TEST(NavigationStack, AMoveOfTenLinesOrMoreAddsAnEntry) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g1, file("a"), at(10), SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({0, 10}));
}

TEST(NavigationStack, ASelectionIsPlacedByTheFirstOfItsTwoEnds) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(50), std::nullopt);
    stack.notify(g1, file("a"), {{60, 0}, {45, 0}}, SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({45}));
    stack.notify(g1, file("a"), {{20, 0}, {40, 0}}, SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({45, 20}));
}

TEST(NavigationStack, ANavigationNearTheLastPlaceStillAddsAnEntry) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g1, file("a"), at(4), SelectionReason::navigation);
    EXPECT_EQ(lines_of(stack), Lines({0, 4}));
}

TEST(NavigationStack, AnEditOrAMoveByTheUserNearTheLastPlaceDoesNot) {
    for (const SelectionReason reason : {SelectionReason::edit, SelectionReason::user}) {
        NavigationStack stack;
        stack.notify(g1, file("a"), at(0), std::nullopt);
        stack.notify(g1, file("a"), at(4), reason);
        EXPECT_EQ(lines_of(stack), Lines({4}));
    }
}

TEST(NavigationStack, EntriesCanBeRemovedByAPredicate) {
    NavigationStack stack;
    for (const std::size_t line : {0U, 20U, 40U, 60U}) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    stack.remove_if([](const NavigationEntry& entry) { return entry.selection.line() % 40 == 20; });
    EXPECT_EQ(lines_of(stack), Lines({0, 40}));
}

TEST(NavigationStack, ANavigationToTheSameLineReplacesTheEntry) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(4, 0), std::nullopt);
    stack.notify(g1, file("a"), at(4, 7), SelectionReason::navigation);
    ASSERT_EQ(stack.entries().size(), 1U);
    EXPECT_EQ(stack.entries()[0].selection, at(4, 7));
}

TEST(NavigationStack, AnotherDocumentOrAnotherGroupAddsAnEntryAtTheSameLine) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(2), std::nullopt);
    stack.notify(g1, file("b"), at(2), std::nullopt);
    stack.notify(g2, file("b"), at(2), std::nullopt);
    ASSERT_EQ(stack.entries().size(), 3U);
    EXPECT_EQ(stack.entries()[1].uri, file("b"));
    EXPECT_EQ(stack.entries()[2].group, g2);
}

TEST(NavigationStack, ActivatingTheSameEditorAgainChangesNothing) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(2), std::nullopt);
    stack.notify(g1, file("a"), at(2), std::nullopt);
    EXPECT_EQ(stack.entries().size(), 1U);
}

TEST(NavigationStack, BackAndForwardWalkTheEntriesAndStopAtTheEnds) {
    NavigationStack stack;
    for (const std::size_t line : {0U, 20U, 40U}) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    ASSERT_EQ(lines_of(stack), Lines({0, 20, 40}));
    EXPECT_FALSE(stack.can_go_forward());

    EXPECT_EQ(stack.go_back()->selection.line(), 20U);
    EXPECT_EQ(stack.go_back()->selection.line(), 0U);
    EXPECT_FALSE(stack.can_go_back());
    EXPECT_FALSE(stack.go_back());
    EXPECT_EQ(stack.go_forward()->selection.line(), 20U);
    EXPECT_EQ(stack.go_forward()->selection.line(), 40U);
    EXPECT_FALSE(stack.go_forward());
    EXPECT_EQ(lines_of(stack), Lines({0, 20, 40}));
}

TEST(NavigationStack, GoingLastJumpsToTheNewestEntry) {
    NavigationStack stack;
    for (const std::size_t line : {0U, 20U, 40U}) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    stack.go_back();
    stack.go_back();
    EXPECT_EQ(stack.go_last()->selection.line(), 40U);
    EXPECT_FALSE(stack.can_go_forward());
}

TEST(NavigationStack, ANewPlaceAfterGoingBackDropsTheEntriesAheadOfIt) {
    NavigationStack stack;
    for (const std::size_t line : {0U, 20U, 40U, 60U}) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    stack.go_back();
    stack.go_back();
    stack.settle(g1, file("a"), at(20));
    stack.notify(g1, file("a"), at(100), SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({0, 20, 100}));
    EXPECT_FALSE(stack.can_go_forward());
}

TEST(NavigationStack, SettlingAfterWalkingMakesNearbyMovesReplaceTheEntryWalkedTo) {
    NavigationStack stack;
    for (const std::size_t line : {0U, 20U, 40U}) {
        stack.notify(g1, file("a"), at(line), SelectionReason::user);
    }
    const auto entry = stack.go_back();
    stack.settle(entry->group, entry->uri, entry->selection);
    stack.notify(g1, file("a"), at(23), SelectionReason::user);
    EXPECT_EQ(lines_of(stack), Lines({0, 23, 40}));
    EXPECT_TRUE(stack.can_go_forward());
}

TEST(NavigationStack, AtMostFiftyEntriesAreKeptAndTheOldestGoFirst) {
    NavigationStack stack;
    for (std::size_t i = 0; i < 70; ++i) {
        stack.notify(g1, file("a"), at(i * 20), SelectionReason::user);
    }
    ASSERT_EQ(stack.entries().size(), NavigationStack::max_entries);
    EXPECT_EQ(stack.entries().front().selection.line(), 20U * 20U);
    EXPECT_EQ(stack.entries().back().selection.line(), 69U * 20U);
    std::size_t steps = 0;
    while (stack.go_back()) {
        ++steps;
    }
    EXPECT_EQ(steps, NavigationStack::max_entries - 1);
    EXPECT_EQ(stack.current()->selection.line(), 20U * 20U);
}

TEST(NavigationStack, APlaceAddedAfterWalkingBackKeepsTheEntriesBehindItAndDropsTheOnesAhead) {
    NavigationStack stack;
    for (std::size_t i = 0; i < 50; ++i) {
        stack.notify(g1, file("a"), at(i * 20), SelectionReason::user);
    }
    stack.go_back();
    stack.go_back();
    stack.settle(g1, file("a"), at(47 * 20));
    stack.notify(g1, file("a"), at(5000), SelectionReason::user);
    ASSERT_EQ(stack.entries().size(), 49U);
    EXPECT_EQ(stack.current()->selection.line(), 5000U);
    EXPECT_EQ(stack.entries().front().selection.line(), 0U);
}

TEST(NavigationStack, RemovingADocumentDropsItsEntriesAndMergesNeighborsThatBecomeTheSame) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(5), std::nullopt);
    stack.notify(g1, file("b"), at(5), std::nullopt);
    stack.notify(g1, file("a"), at(5), std::nullopt);
    stack.notify(g1, file("c"), at(5), std::nullopt);
    stack.go_back();
    stack.remove_uri(file("b"));
    ASSERT_EQ(stack.entries().size(), 2U);
    EXPECT_EQ(stack.entries()[0].uri, file("a"));
    EXPECT_EQ(stack.entries()[1].uri, file("c"));
    EXPECT_EQ(stack.current()->uri, file("c"));
    EXPECT_FALSE(stack.can_go_forward());
}

TEST(NavigationStack, RemovingADocumentThatIsNotInTheListChangesNothing) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g1, file("a"), at(30), SelectionReason::user);
    stack.go_back();
    stack.remove_uri(file("z"));
    EXPECT_EQ(lines_of(stack), Lines({0, 30}));
    EXPECT_TRUE(stack.can_go_forward());
}

TEST(NavigationStack, RemovingAGroupDropsItsEntries) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g2, file("b"), at(0), std::nullopt);
    stack.notify(g1, file("c"), at(0), std::nullopt);
    stack.remove_group(g2);
    ASSERT_EQ(stack.entries().size(), 2U);
    EXPECT_EQ(stack.entries()[1].uri, file("c"));
}

TEST(NavigationStack, AnEditorThatMovesToAnotherGroupTakesItsEntriesAndItsStateAlong) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.notify(g1, file("b"), at(0), std::nullopt);
    stack.move_editor(g1, g2, file("b"));
    EXPECT_EQ(stack.entries()[0].group, g1);
    EXPECT_EQ(stack.entries()[1].group, g2);
    stack.notify(g2, file("b"), at(3), SelectionReason::user);
    EXPECT_EQ(stack.entries().size(), 2U);
}

TEST(NavigationStack, ARenamedDocumentKeepsItsEntries) {
    NavigationStack stack;
    core::Uri untitled;
    untitled.scheme = "untitled";
    untitled.path = "Untitled-1";
    stack.notify(g1, untitled, at(0), std::nullopt);
    stack.replace_uri(untitled, file("saved"));
    EXPECT_EQ(stack.entries()[0].uri, file("saved"));
    stack.notify(g1, file("saved"), at(2), SelectionReason::user);
    EXPECT_EQ(stack.entries().size(), 1U);
}

TEST(NavigationStack, ClearingForgetsEverything) {
    NavigationStack stack;
    stack.notify(g1, file("a"), at(0), std::nullopt);
    stack.clear();
    EXPECT_EQ(stack.current(), nullptr);
    EXPECT_FALSE(stack.can_go_last());
    stack.notify(g1, file("a"), at(0), std::nullopt);
    EXPECT_EQ(stack.entries().size(), 1U);
}

TEST(NavigationHistory, OnlyEditsEnterTheListOfEditLocations) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(50), SelectionReason::user);
    EXPECT_FALSE(history.can_go_to_last_edit());
    history.selection_changed(g1, file("a"), at(50), SelectionReason::edit);
    EXPECT_TRUE(history.can_go_to_last_edit());
    EXPECT_EQ(history.edits().entries().size(), 1U);
    history.selection_changed(g1, file("b"), at(7), SelectionReason::navigation);
    EXPECT_EQ(history.edits().entries().size(), 1U);
}

TEST(NavigationHistory, EditsAreAlsoPlacesOfTheUser) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(40), SelectionReason::edit);
    EXPECT_EQ(lines_of(history.selections()), Lines({0, 40}));
    EXPECT_TRUE(history.can_go_back());
}

TEST(NavigationHistory, TheLastEditLocationIsTheNewestEditAndGoingThereIsAPlaceToGoBackFrom) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(100), SelectionReason::edit);
    history.selection_changed(g1, file("b"), at(7), SelectionReason::edit);
    history.selection_changed(g1, file("c"), at(0), SelectionReason::user);

    const auto last = history.go_to_last_edit();
    ASSERT_TRUE(last);
    EXPECT_EQ(last->uri, file("b"));
    EXPECT_EQ(last->selection.line(), 7U);
    history.arrived_at_edit(last->group, last->uri, last->selection);

    EXPECT_EQ(history.selections().current()->uri, file("b"));
    EXPECT_EQ(history.go_back()->uri, file("c"));
}

TEST(NavigationHistory, GoingToTheLastEditLocationAgainStaysOnTheSamePlace) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(100), SelectionReason::edit);
    for (int i = 0; i < 3; ++i) {
        const auto last = history.go_to_last_edit();
        history.arrived_at_edit(last->group, last->uri, last->selection);
    }
    EXPECT_EQ(history.edits().entries().size(), 1U);
    EXPECT_EQ(lines_of(history.selections()), Lines({0, 100}));
}

TEST(NavigationHistory, WalkingBackAndForwardDoesNotAddPlaces) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(30), SelectionReason::user);
    history.selection_changed(g1, file("b"), at(60), SelectionReason::user);
    const Lines before = lines_of(history.selections());

    for (int round = 0; round < 3; ++round) {
        while (const auto entry = history.go_back()) {
            history.arrived(entry->group, entry->uri, entry->selection);
        }
        while (const auto entry = history.go_forward()) {
            history.arrived(entry->group, entry->uri, entry->selection);
        }
    }
    EXPECT_EQ(lines_of(history.selections()), before);
    EXPECT_FALSE(history.can_go_forward());
}

TEST(NavigationHistory, RemovalsApplyToBothLists) {
    NavigationHistory history;
    history.editor_activated(g1, file("a"), at(0));
    history.selection_changed(g1, file("a"), at(50), SelectionReason::edit);
    history.selection_changed(g2, file("b"), at(50), SelectionReason::edit);

    history.move_editor(g2, g1, file("b"));
    EXPECT_EQ(history.edits().entries().back().group, g1);
    history.remove_group(g1);
    EXPECT_FALSE(history.can_go_to_last_edit());
    EXPECT_FALSE(history.can_go_back());

    history.selection_changed(g1, file("u"), at(0), SelectionReason::edit);
    history.replace_uri(file("u"), file("v"));
    EXPECT_EQ(history.edits().entries().back().uri, file("v"));
    history.remove_uri(file("v"));
    EXPECT_FALSE(history.can_go_to_last_edit());
    history.selection_changed(g1, file("w"), at(0), SelectionReason::edit);
    history.clear();
    EXPECT_FALSE(history.can_go_to_last_edit());
    EXPECT_EQ(history.selections().current(), nullptr);
}

TEST(NavigationHistoryModel, RandomVisitsKeepTheListConsistentAndWalkableInBothDirections) {
    std::mt19937 random(7);
    for (int round = 0; round < 200; ++round) {
        NavigationHistory history;
        const auto pick_uri = [&] {
            return file(std::string(1, static_cast<char>('a' + random() % 4)).c_str());
        };
        const auto pick_group = [&] {
            return GroupId{static_cast<std::uint32_t>(1 + random() % 2)};
        };
        for (int step = 0; step < 150; ++step) {
            switch (random() % 7) {
            case 0:
                history.editor_activated(pick_group(), pick_uri(), at(random() % 80));
                break;
            case 1:
            case 2:
            case 3:
                history.selection_changed(pick_group(), pick_uri(), at(random() % 80),
                                          static_cast<SelectionReason>(random() % 3));
                break;
            case 4:
                if (const auto entry = history.go_back()) {
                    history.arrived(entry->group, entry->uri, entry->selection);
                }
                break;
            case 5:
                if (const auto entry = history.go_forward()) {
                    history.arrived(entry->group, entry->uri, entry->selection);
                }
                break;
            default:
                history.remove_uri(pick_uri());
                break;
            }
            for (const NavigationStack* stack : {&history.selections(), &history.edits()}) {
                ASSERT_LE(stack->entries().size(), NavigationStack::max_entries);
                ASSERT_EQ(stack->current() == nullptr, stack->entries().empty());
            }
        }

        const std::vector<NavigationEntry> entries = history.selections().entries();
        while (history.go_back()) {
        }
        std::vector<NavigationEntry> walked;
        if (history.selections().current()) {
            walked.push_back(*history.selections().current());
        }
        while (const auto entry = history.go_forward()) {
            walked.push_back(*entry);
        }
        ASSERT_EQ(walked, entries) << round;
    }
}
