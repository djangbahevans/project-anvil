#include "anvil/workbench/closed_editors.hpp"

#include <gtest/gtest.h>
#include <random>

using namespace anvil;
using namespace anvil::workbench;

namespace {

core::Uri file(std::size_t number) {
    return core::Uri::file("/work/" + std::to_string(number));
}

ClosedEditor closed(std::size_t number) {
    ClosedEditor editor;
    editor.uri = file(number);
    editor.index = number;
    return editor;
}

std::vector<core::Uri> uris(const std::vector<ClosedEditor>& editors) {
    std::vector<core::Uri> result;
    for (const ClosedEditor& editor : editors) {
        result.push_back(editor.uri);
    }
    return result;
}

} // namespace

TEST(ClosedEditors, StartsEmpty) {
    ClosedEditors stack;
    EXPECT_TRUE(stack.empty());
    EXPECT_TRUE(stack.take_last_batch().empty());
}

TEST(ClosedEditors, TheEditorsOfOneBatchComeBackTogetherInTheOrderTheyWereClosed) {
    ClosedEditors stack;
    stack.begin_batch();
    stack.push(closed(1));
    stack.begin_batch();
    stack.push(closed(2));
    stack.push(closed(3));
    stack.push(closed(4));

    EXPECT_EQ(uris(stack.take_last_batch()), (std::vector{file(2), file(3), file(4)}));
    EXPECT_EQ(uris(stack.take_last_batch()), std::vector{file(1)});
    EXPECT_TRUE(stack.empty());
}

TEST(ClosedEditors, EditorsPushedWithoutANewBatchJoinTheLastOne) {
    ClosedEditors stack;
    stack.push(closed(1));
    stack.push(closed(2));
    EXPECT_EQ(stack.take_last_batch().size(), 2U);
}

TEST(ClosedEditors, AnEditorThatIsClosedAgainReplacesItsEarlierEntry) {
    ClosedEditors stack;
    stack.begin_batch();
    stack.push(closed(1));
    stack.begin_batch();
    stack.push(closed(2));
    stack.begin_batch();
    ClosedEditor again = closed(1);
    again.index = 9;
    stack.push(again);

    ASSERT_EQ(stack.size(), 2U);
    const auto batch = stack.take_last_batch();
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].uri, file(1));
    EXPECT_EQ(batch[0].index, 9U);
    EXPECT_EQ(uris(stack.take_last_batch()), std::vector{file(2)});
}

TEST(ClosedEditors, AtMostTwentyAreKeptAndTheOldestGoFirst) {
    ClosedEditors stack;
    for (std::size_t i = 0; i < 30; ++i) {
        stack.begin_batch();
        stack.push(closed(i));
    }
    ASSERT_EQ(stack.size(), ClosedEditors::max_entries);
    EXPECT_EQ(stack.editors().front().uri, file(10));
    EXPECT_EQ(stack.editors().back().uri, file(29));
}

TEST(ClosedEditors, ABatchThatLosesItsOldestEditorsToTheLimitComesBackWithTheRest) {
    ClosedEditors stack;
    stack.begin_batch();
    for (std::size_t i = 0; i < 25; ++i) {
        stack.push(closed(i));
    }
    const auto batch = stack.take_last_batch();
    ASSERT_EQ(batch.size(), ClosedEditors::max_entries);
    EXPECT_EQ(batch.front().uri, file(5));
    EXPECT_EQ(batch.back().uri, file(24));
}

TEST(ClosedEditors, ADocumentCanBeForgotten) {
    ClosedEditors stack;
    stack.begin_batch();
    stack.push(closed(1));
    stack.push(closed(2));
    stack.remove_uri(file(1));
    EXPECT_EQ(uris(stack.editors()), std::vector{file(2)});
    stack.remove_uri(file(7));
    EXPECT_EQ(stack.size(), 1U);
}

TEST(ClosedEditors, KeepsWhatItWasGiven) {
    ClosedEditors stack;
    ClosedEditor editor = closed(1);
    editor.group = GroupId{3};
    editor.selections = {{{1, 2}, {1, 5}}, {{4, 0}, {4, 0}}};
    editor.primary = 1;
    editor.text = *core::Text::from_utf8("hello");
    stack.begin_batch();
    stack.push(editor);
    const auto taken = stack.take_last_batch();
    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(taken[0].group, GroupId{3});
    EXPECT_EQ(taken[0].selections, editor.selections);
    EXPECT_EQ(taken[0].primary, 1U);
    ASSERT_TRUE(taken[0].text);
    EXPECT_EQ(taken[0].text->to_string(), "hello");
}

TEST(ClosedEditorsModel, RandomClosesAndReopensAgreeWithAListOfListsReference) {
    std::mt19937 random(3);
    for (int round = 0; round < 300; ++round) {
        ClosedEditors stack;
        // The reference keeps batches as lists of document numbers, and drops numbers over the
        // limit from the oldest end.
        std::vector<std::vector<std::size_t>> batches;
        bool batch_is_open = false;
        const auto total = [&] {
            std::size_t count = 0;
            for (const auto& batch : batches) {
                count += batch.size();
            }
            return count;
        };
        for (int step = 0; step < 60; ++step) {
            if (random() % 4 == 0) {
                const auto taken = stack.take_last_batch();
                std::vector<std::size_t> expected;
                if (!batches.empty()) {
                    expected = batches.back();
                    batches.pop_back();
                }
                std::vector<std::size_t> got;
                for (const ClosedEditor& editor : taken) {
                    got.push_back(static_cast<std::size_t>(editor.index));
                }
                ASSERT_EQ(got, expected) << round << " " << step;
                batch_is_open = false;
                continue;
            }
            const std::size_t number = random() % 12;
            if (random() % 2 == 0 || !batch_is_open) {
                stack.begin_batch();
                batches.emplace_back();
                batch_is_open = true;
            }
            stack.push(closed(number));
            for (auto& batch : batches) {
                std::erase(batch, number);
            }
            batches.back().push_back(number);
            std::erase_if(batches, [](const auto& batch) { return batch.empty(); });
            while (total() > ClosedEditors::max_entries) {
                batches.front().erase(batches.front().begin());
                if (batches.front().empty()) {
                    batches.erase(batches.begin());
                }
            }
            ASSERT_EQ(stack.size(), total());
        }
    }
}
