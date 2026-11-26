#include "../timing.hpp"
#include "anvil/core/log.hpp"
#include "anvil/ui/codicons.hpp"
#include "anvil/ui/quick_pick.hpp"
#include "anvil/ui/resources.hpp"
#include "anvil/ui/scroll_bar.hpp"
#include "anvil/ui/theme_store.hpp"
#include "anvil/ui/zoom.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <gtest/gtest.h>
#include <memory>

using anvil::core::MatchRange;
using anvil::ui::QuickPick;
using anvil::ui::QuickPickItem;
using anvil::ui::WorkbenchColors;
using Ranges = std::vector<MatchRange>;

namespace {

using anvil::test::timing_is_meaningful;

class QuickPickTest : public ::testing::Test {
protected:
    void SetUp() override {
        host_ = std::make_unique<QWidget>();
        host_->resize(900, 600);
        auto* layout = new QVBoxLayout(host_.get());
        other_ = new QLineEdit(host_.get());
        second_ = new QLineEdit(host_.get());
        layout->addWidget(other_);
        layout->addWidget(second_);
        layout->addStretch();
        pick_ = new QuickPick(host_.get(), colors_);
        pick_->set_items(sample_items());
        host_->show();
        host_->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(host_.get()));
    }
    void TearDown() override {
        host_.reset();
    }

    static std::vector<QuickPickItem> items_of(const std::vector<QString>& labels) {
        std::vector<QuickPickItem> items;
        for (const QString& label : labels) {
            items.push_back({label, {}, {}, {}});
        }
        return items;
    }
    static std::vector<QuickPickItem> sample_items() {
        return items_of({"Format Document", "Open File…", "Open Folder…", "Save As…", "Save",
                         "Toggle Terminal", "Go to Line", "Save All"});
    }

    void type(const QString& text) {
        QTest::keyClicks(pick_->filter_field(), text);
    }
    void press(Qt::Key key) {
        QTest::keyClick(pick_->filter_field(), key);
    }
    int current_row() const {
        return pick_->list()->currentIndex().row();
    }
    QString label_at(std::size_t row) const {
        return pick_->list()->model()->index(static_cast<int>(row), 0).data().toString();
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<QWidget> host_;
    QLineEdit* other_ = nullptr;
    QLineEdit* second_ = nullptr;
    QuickPick* pick_ = nullptr;
};

} // namespace

TEST_F(QuickPickTest, ShowsEveryItemInTheOrderGivenWhenThereIsNoFilter) {
    EXPECT_TRUE(pick_->isHidden());
    pick_->open();
    EXPECT_TRUE(pick_->isVisible());
    EXPECT_EQ(pick_->shown_items(), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(current_row(), 0);
    EXPECT_TRUE(pick_->filter_field()->hasFocus());
}

TEST_F(QuickPickTest, FiltersAndRanksAsTheUserTypes) {
    pick_->open();
    type("op");
    ASSERT_EQ(pick_->shown_items().size(), 2U);
    EXPECT_EQ(label_at(0), "Open File…");
    EXPECT_EQ(label_at(1), "Open Folder…");
    EXPECT_EQ(pick_->label_highlights(0), (Ranges{{0, 2}}));
    EXPECT_EQ(current_row(), 0);

    pick_->filter_field()->clear();
    type("sav");
    EXPECT_EQ(label_at(0), "Save");
    EXPECT_EQ(pick_->shown_items().size(), 3U);

    pick_->filter_field()->clear();
    type("tt");
    ASSERT_EQ(pick_->shown_items().size(), 2U);
    EXPECT_EQ(label_at(0), "Toggle Terminal");
    EXPECT_EQ(pick_->label_highlights(0), (Ranges{{0, 1}, {7, 8}}));
    EXPECT_EQ(label_at(1), "Format Document");
}

TEST_F(QuickPickTest, ADescriptionIsMatchedToo) {
    pick_->set_items({{"Toggle Terminal", "View", {}, {}}, {"Save", "File", {}, {}}});
    pick_->open();
    type("file");
    ASSERT_EQ(pick_->shown_items().size(), 1U);
    EXPECT_EQ(pick_->shown_items().front(), 1U);
}

TEST_F(QuickPickTest, ShowsAMessageWhenNothingMatches) {
    pick_->set_empty_message("No matching commands");
    pick_->open();
    type("zzz");
    EXPECT_TRUE(pick_->shown_items().empty());
    EXPECT_TRUE(pick_->list()->isHidden());
    bool shown = false;
    for (const QLabel* label : pick_->findChildren<QLabel*>()) {
        shown = shown || (label->text() == "No matching commands" && label->isVisible());
    }
    EXPECT_TRUE(shown);

    pick_->filter_field()->clear();
    EXPECT_FALSE(pick_->list()->isHidden());
}

TEST_F(QuickPickTest, AnIgnoredPrefixIsNotMatched) {
    pick_->set_ignored_prefix(">");
    pick_->open(">");
    EXPECT_EQ(pick_->shown_items().size(), 8U);
    EXPECT_EQ(pick_->filter_field()->text(), ">");
    type("open");
    EXPECT_EQ(pick_->shown_items().size(), 2U);
    EXPECT_EQ(pick_->label_highlights(0), (Ranges{{0, 4}}));
}

TEST_F(QuickPickTest, UpAndDownMoveTheSelectionAndWrapAround) {
    pick_->open();
    press(Qt::Key_Down);
    EXPECT_EQ(current_row(), 1);
    press(Qt::Key_Up);
    EXPECT_EQ(current_row(), 0);
    press(Qt::Key_Up);
    EXPECT_EQ(current_row(), 7);
    press(Qt::Key_Down);
    EXPECT_EQ(current_row(), 0);
}

TEST_F(QuickPickTest, PageKeysMoveByAPageAndStopAtTheEnds) {
    std::vector<QString> labels;
    for (int i = 0; i < 100; ++i) {
        labels.push_back(QString("Item %1").arg(i));
    }
    pick_->set_items(items_of(labels));
    pick_->open();
    press(Qt::Key_PageDown);
    const int page = current_row();
    EXPECT_GT(page, 3);
    EXPECT_LT(page, 100);
    press(Qt::Key_PageDown);
    EXPECT_EQ(current_row(), 2 * page);
    press(Qt::Key_PageUp);
    press(Qt::Key_PageUp);
    press(Qt::Key_PageUp);
    EXPECT_EQ(current_row(), 0);
    for (int i = 0; i < 20; ++i) {
        press(Qt::Key_PageDown);
    }
    EXPECT_EQ(current_row(), 99);
}

TEST_F(QuickPickTest, EnterChoosesTheSelectedItemAndClosesThePopup) {
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    QSignalSpy cancelled(pick_, &QuickPick::cancelled);
    pick_->open();
    type("save");
    press(Qt::Key_Down);
    ASSERT_EQ(pick_->shown_items().size(), 3U);
    const std::size_t second = pick_->shown_items()[1];
    press(Qt::Key_Return);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.front().front().value<std::size_t>(), second);
    EXPECT_EQ(cancelled.count(), 0);
    EXPECT_TRUE(pick_->isHidden());
}

TEST_F(QuickPickTest, ADisabledItemCannotBeChosenAndThePopupStaysOpen) {
    auto items = items_of({"Alpha", "Beta"});
    items[0].disabled = true;
    pick_->set_items(std::move(items));
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    pick_->open();

    press(Qt::Key_Return);
    EXPECT_EQ(accepted.count(), 0);
    EXPECT_TRUE(pick_->isVisible());

    const QRect row = pick_->list()->visualRect(pick_->list()->model()->index(0, 0));
    QTest::mouseClick(pick_->list()->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    EXPECT_EQ(accepted.count(), 0);
    EXPECT_TRUE(pick_->isVisible());

    press(Qt::Key_Down);
    press(Qt::Key_Return);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.front().front().value<std::size_t>(), 1U);
}

TEST_F(QuickPickTest, EscapeClosesThePopupWithoutAChoice) {
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    QSignalSpy cancelled(pick_, &QuickPick::cancelled);
    pick_->open();
    press(Qt::Key_Escape);
    EXPECT_TRUE(pick_->isHidden());
    EXPECT_EQ(accepted.count(), 0);
    EXPECT_EQ(cancelled.count(), 1);
}

TEST_F(QuickPickTest, EnterWithNothingShownDoesNotChoose) {
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    pick_->open();
    type("zzz");
    press(Qt::Key_Return);
    EXPECT_EQ(accepted.count(), 0);
    EXPECT_TRUE(pick_->isVisible());
}

TEST_F(QuickPickTest, AClickChoosesAnItem) {
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    pick_->open();
    const QModelIndex index = pick_->list()->model()->index(2, 0);
    QTest::mouseClick(pick_->list()->viewport(), Qt::LeftButton, {},
                      pick_->list()->visualRect(index).center());
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.front().front().value<std::size_t>(), 2U);
    EXPECT_TRUE(pick_->isHidden());
}

TEST_F(QuickPickTest, TheFocusReturnsToWhereItWasWhenThePopupCloses) {
    second_->setFocus();
    ASSERT_TRUE(second_->hasFocus());
    pick_->open();
    EXPECT_FALSE(second_->hasFocus());
    press(Qt::Key_Escape);
    EXPECT_TRUE(second_->hasFocus());

    pick_->open();
    press(Qt::Key_Return);
    EXPECT_TRUE(second_->hasFocus());
}

TEST_F(QuickPickTest, TheFocusLeavingThePopupClosesIt) {
    QSignalSpy cancelled(pick_, &QuickPick::cancelled);
    pick_->open();
    other_->setFocus();
    EXPECT_TRUE(pick_->isHidden());
    EXPECT_EQ(cancelled.count(), 1);
    EXPECT_TRUE(other_->hasFocus());
}

TEST_F(QuickPickTest, ReplacingTheItemsWhileOpenFiltersThemAgain) {
    pick_->open();
    type("open");
    ASSERT_EQ(pick_->shown_items().size(), 2U);
    pick_->set_items(items_of({"Open Recent", "Close", "Reopen Closed Editor"}));
    EXPECT_EQ(pick_->shown_items(), (std::vector<std::size_t>{0, 2}));
}

TEST_F(QuickPickTest, SitsAtTheTopCenterOfTheWindowNoWiderThanItsLimit) {
    pick_->set_top_offset(24);
    pick_->open();
    EXPECT_EQ(pick_->width(), 600);
    EXPECT_EQ(pick_->y(), 24);
    EXPECT_NEAR(pick_->x() + pick_->width() / 2, host_->width() / 2, 1);

    host_->resize(400, 600);
    QApplication::processEvents();
    EXPECT_EQ(pick_->width(), 400 - 16);
    EXPECT_NEAR(pick_->x() + pick_->width() / 2, host_->width() / 2, 1);
}

TEST_F(QuickPickTest, TheListFitsItsRowsAndShowsTwentyAtMost) {
    pick_->set_items(items_of({"One", "Two", "Three"}));
    pick_->open();
    EXPECT_EQ(pick_->list()->height(), 3 * QuickPick::row_height);

    std::vector<QString> many;
    for (int i = 0; i < 500; ++i) {
        many.push_back(QString::number(i));
    }
    pick_->set_items(items_of(many));
    EXPECT_EQ(pick_->list()->height(), QuickPick::max_visible_rows * QuickPick::row_height + 6);
    EXPECT_LT(pick_->y() + pick_->height(), host_->height());
}

TEST_F(QuickPickTest, TheListIsCappedByTheHeightOfALowWindow) {
    std::vector<QString> many;
    for (int i = 0; i < 500; ++i) {
        many.push_back(QString::number(i));
    }
    host_->resize(900, 300);
    QApplication::processEvents();
    pick_->set_items(items_of(many));
    pick_->open();
    EXPECT_LT(pick_->list()->height(), 12 * QuickPick::row_height);
    EXPECT_GE(pick_->list()->height(), 3 * QuickPick::row_height);
    EXPECT_LE(pick_->y() + pick_->height(), host_->height());
}

TEST_F(QuickPickTest, ADetailMakesItsRowTwoLinesHigh) {
    pick_->set_items({{"One", {}, "detail of one", {}}, {"Two", {}, {}, {}}});
    pick_->open();
    EXPECT_EQ(pick_->list()->sizeHintForRow(0), 2 * QuickPick::row_height);
    EXPECT_EQ(pick_->list()->sizeHintForRow(1), QuickPick::row_height);
    EXPECT_EQ(pick_->list()->height(), 3 * QuickPick::row_height);
}

TEST_F(QuickPickTest, FiltersFiveThousandItemsWithoutNoticeableDelay) {
    std::vector<QString> labels;
    const char* categories[] = {"View", "File", "Git", "Preferences", "Debug", "Terminal"};
    const char* verbs[] = {"Toggle", "Open", "Close", "Focus", "Move", "Select", "Delete"};
    for (int i = 0; i < 5000; ++i) {
        labels.push_back(
            QString("%1: %2 Something %3").arg(categories[i % 6], verbs[(i / 6) % 7]).arg(i));
    }
    pick_->set_items(items_of(labels));
    pick_->open();

    qint64 slowest = 0;
    for (const QString& query : {QString("t"), QString("to"), QString("tog"), QString("togg"),
                                 QString("toggle f"), QString("toggle fi")}) {
        pick_->filter_field()->clear();
        QElapsedTimer timer;
        timer.start();
        pick_->filter_field()->setText(query);
        QApplication::processEvents();
        slowest = std::max(slowest, timer.elapsed());
        EXPECT_FALSE(pick_->shown_items().empty()) << query.toStdString();
    }
    std::printf("[ measured ] slowest filter of 5000 items: %lld ms\n",
                static_cast<long long>(slowest));
    if (timing_is_meaningful) {
        EXPECT_LT(slowest, 500);
    }
}

namespace {

QuickPickItem separator(const QString& label) {
    return {label, {}, {}, {}, false, true};
}

} // namespace

TEST_F(QuickPickTest, SeparatorsAreNotRowsAndTheItemsKeepTheirPositions) {
    pick_->set_items({separator("First"),
                      {"One", {}, {}, {}},
                      {"Two", {}, {}, {}},
                      separator("Second"),
                      {"Three", {}, {}, {}},
                      separator("Last")});
    pick_->open();
    EXPECT_EQ(pick_->shown_items(), (std::vector<std::size_t>{1, 2, 4}));
    EXPECT_EQ(pick_->list()->model()->rowCount(), 3);

    QSignalSpy accepted(pick_, &QuickPick::accepted);
    press(Qt::Key_Down);
    press(Qt::Key_Down);
    press(Qt::Key_Return);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.takeFirst().at(0).toULongLong(), 4U);
}

TEST_F(QuickPickTest, NavigationNeverStopsOnASeparatorAndWrapsAroundTheItems) {
    pick_->set_items({{"One", {}, {}, {}}, separator("Group"), {"Two", {}, {}, {}}});
    pick_->open();
    EXPECT_EQ(current_row(), 0);
    press(Qt::Key_Down);
    EXPECT_EQ(current_row(), 1);
    press(Qt::Key_Down);
    EXPECT_EQ(current_row(), 0);
    press(Qt::Key_Up);
    EXPECT_EQ(current_row(), 1);
    press(Qt::Key_PageDown);
    EXPECT_EQ(current_row(), 1);
    press(Qt::Key_PageUp);
    EXPECT_EQ(current_row(), 0);
}

TEST_F(QuickPickTest, SeparatorsAreLeftOutAndNotMatchedWhileAFilterIsTyped) {
    pick_->set_items(
        {separator("Group"), {"Two", {}, {}, {}}, separator("Other"), {"Three", {}, {}, {}}});
    pick_->open();
    type("t");
    EXPECT_EQ(pick_->shown_items(), (std::vector<std::size_t>{1, 3}));
    pick_->filter_field()->clear();
    type("Group");
    EXPECT_TRUE(pick_->shown_items().empty());
    pick_->filter_field()->clear();
    EXPECT_EQ(pick_->shown_items(), (std::vector<std::size_t>{1, 3}));
}

TEST_F(QuickPickTest, ASeparatorDrawsALineAboveTheNextRowAndItsLabelAtTheRightEnd) {
    pick_->set_items(
        {{"Top", {}, {}, {}}, separator("Group"), {"Next", {}, {}, {}}, {"After", {}, {}, {}}});
    pick_->open();
    QApplication::processEvents();
    const QImage image = pick_->grab().toImage();
    const int list_top = pick_->list()->geometry().top();
    const int line_x = pick_->width() / 2;
    const QColor border = colors_.color("pickerGroup.border");
    EXPECT_EQ(image.pixelColor(line_x, list_top + QuickPick::row_height).rgb(), border.rgb());
    EXPECT_NE(image.pixelColor(line_x, list_top + 2 * QuickPick::row_height).rgb(), border.rgb());
    EXPECT_NE(image.pixelColor(line_x, list_top).rgb(), border.rgb());

    // The label is in the right part of the row of the next item, and the other rows have none.
    const auto ink = [&](int row) {
        int pixels = 0;
        const QColor background = colors_.color("quickInput.background");
        const QColor highlight = colors_.color("quickInputList.focusBackground");
        for (int y = list_top + row * QuickPick::row_height + 3;
             y < list_top + (row + 1) * QuickPick::row_height - 3; ++y) {
            for (int x = pick_->width() * 3 / 4; x < pick_->width() - 12; ++x) {
                const QColor color = image.pixelColor(x, y);
                pixels += color != background && color != highlight;
            }
        }
        return pixels;
    };
    EXPECT_GT(ink(1), 10);
    EXPECT_EQ(ink(2), 0);

    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        std::vector<QuickPickItem> items = {{"Auto Detect", {}, {}, {}},
                                            {"Configure File Association for '.md'…", {}, {}, {}},
                                            separator("languages (identifier)")};
        for (const char* name : {"Agent", "Batch", "BibTeX", "C", "C#", "C++", "Clojure"}) {
            items.push_back({name, QString("(%1)").arg(QString(name).toLower()), {}, {}});
        }
        pick_->set_items(std::move(items));
        pick_->set_placeholder("Select Language Mode");
        pick_->set_top_offset(30);
        QApplication::processEvents();
        host_->grab().save(QString::fromUtf8(directory) + "/quick_pick_separators.png");
    }
}

TEST_F(QuickPickTest, TheCornersAreRoundedAndTheBorderFollowsThem) {
    pick_->set_items(items_of({"One", "Two"}));
    pick_->set_top_offset(40);
    pick_->open();
    QApplication::processEvents();
    const QImage image = host_->grab().toImage();
    const QColor card = colors_.color("quickInput.background");
    const int left = pick_->x();
    const int top = pick_->y();
    EXPECT_NE(image.pixelColor(left, top), card) << "the corner is outside the arc";
    EXPECT_NE(image.pixelColor(left, top), colors_.color("widget.border"));
    EXPECT_EQ(image.pixelColor(left + pick_->width() / 2, top), colors_.color("widget.border"));
}

TEST_F(QuickPickTest, AShadowSurroundsThePopupAndIsGoneWithIt) {
    colors_.set_colors({{"widget.shadow", QColor(0, 0, 0, 0x5c)}});
    pick_->set_items(items_of({"One", "Two"}));
    pick_->set_top_offset(60);
    pick_->open();
    QApplication::processEvents();
    const QImage open = host_->grab().toImage();
    const int below = pick_->y() + pick_->height() + 4;
    const int middle = pick_->x() + pick_->width() / 2;
    pick_->cancel();
    QApplication::processEvents();
    const QImage closed = host_->grab().toImage();
    EXPECT_NE(open.pixelColor(middle, below), closed.pixelColor(middle, below))
        << "the shadow darkens the window under the popup";
    EXPECT_EQ(open.pixelColor(middle, pick_->y() + pick_->height() + 40),
              closed.pixelColor(middle, pick_->y() + pick_->height() + 40))
        << "and fades out within its blur";
}

TEST_F(QuickPickTest, NoShadowIsDrawnWhenTheThemeHasNone) {
    pick_->set_items(items_of({"One", "Two"}));
    pick_->set_top_offset(60);
    pick_->open();
    QApplication::processEvents();
    const QImage open = host_->grab().toImage();
    const int below = pick_->y() + pick_->height() + 4;
    const int middle = pick_->x() + pick_->width() / 2;
    pick_->cancel();
    QApplication::processEvents();
    EXPECT_EQ(open.pixelColor(middle, below), host_->grab().toImage().pixelColor(middle, below));
}

TEST_F(QuickPickTest, RendersTheList) {
    pick_->set_items({{"Format Document", {}, {}, "Shift+Alt+F"},
                      {"Open File…", {}, {}, "Ctrl+O"},
                      {"Open Folder…", "File", "Opens a folder in the explorer", "Ctrl+K Ctrl+O"},
                      {"Save", {}, {}, "Ctrl+S"},
                      {"Toggle Terminal", {}, {}, {}}});
    pick_->set_ignored_prefix(">");
    pick_->set_top_offset(10);
    pick_->open(">op");
    QApplication::processEvents();
    const QImage image = host_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/quick_pick.png");
    }
}

TEST_F(QuickPickTest, RepaintsWithTheColorsOfTheWorkbench) {
    pick_->set_items(items_of({"One", "Two", "Three"}));
    pick_->open();
    const auto grab = [&] {
        QApplication::processEvents();
        return pick_->grab().toImage();
    };
    // Inside the list, at the left edge of a row: the first row is the chosen one, the second is
    // not.
    const int first_row_y = pick_->list()->geometry().top() + QuickPick::row_height / 2;
    const int second_row_y = first_row_y + QuickPick::row_height;
    // Inside the highlight, which is inset from the edge, and left of the text.
    const int x = pick_->list()->geometry().left() + 6;
    const int border_y = pick_->height() / 2;

    const QImage before = grab();
    EXPECT_EQ(before.pixelColor(x, first_row_y), colors_.color("quickInputList.focusBackground"));
    EXPECT_EQ(before.pixelColor(x, second_row_y), colors_.color("quickInput.background"));
    EXPECT_EQ(before.pixelColor(0, border_y), colors_.color("widget.border"));

    QSignalSpy changed(&colors_, &WorkbenchColors::changed);
    colors_.set_colors({{"quickInputList.focusBackground", QColor("#aa0000")},
                        {"quickInput.background", QColor("#102030")},
                        {"widget.border", QColor("#00cc00")},
                        {"input.background", QColor("#334455")},
                        {"input.foreground", QColor("#ffeedd")}});
    EXPECT_EQ(changed.count(), 1);

    // The widgets that draw from their own palette follow without being painted.
    EXPECT_EQ(pick_->list()->palette().color(QPalette::Base), QColor("#102030"));
    EXPECT_EQ(pick_->filter_field()->palette().color(QPalette::Base), QColor("#334455"));
    EXPECT_EQ(pick_->filter_field()->palette().color(QPalette::Text), QColor("#ffeedd"));

    const QImage after = grab();
    EXPECT_EQ(after.pixelColor(x, first_row_y), QColor("#aa0000"));
    EXPECT_EQ(after.pixelColor(x, second_row_y), QColor("#102030"));
    EXPECT_EQ(after.pixelColor(0, border_y), QColor("#00cc00"));
}

TEST_F(QuickPickTest, AnItemCanBeMadeTheChosenRowByItsPosition) {
    pick_->set_items({separator("First"),
                      {"One", {}, {}, {}},
                      {"Two", {}, {}, {}},
                      separator("Second"),
                      {"Three", {}, {}, {}}});
    pick_->open();
    pick_->set_active_item(4);
    EXPECT_EQ(current_row(), 2);
    pick_->set_active_item(2);
    EXPECT_EQ(current_row(), 1);

    QSignalSpy accepted(pick_, &QuickPick::accepted);
    press(Qt::Key_Return);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.takeFirst().at(0).toULongLong(), 2U);
}

TEST_F(QuickPickTest, AnItemThatIsAPartOfNoRowLeavesTheChosenRowAlone) {
    pick_->set_items({separator("First"), {"One", {}, {}, {}}, {"Two", {}, {}, {}}});
    pick_->open();
    pick_->set_active_item(2);
    ASSERT_EQ(current_row(), 1);
    pick_->set_active_item(0);
    pick_->set_active_item(9);
    EXPECT_EQ(current_row(), 1);
}

TEST_F(QuickPickTest, MovingTheChosenRowTellsWhichItemItIsOn) {
    pick_->set_items({separator("First"),
                      {"One", {}, {}, {}},
                      {"Two", {}, {}, {}},
                      separator("Second"),
                      {"Three", {}, {}, {}}});
    pick_->open();
    QSignalSpy active(pick_, &QuickPick::active_changed);
    press(Qt::Key_Down);
    press(Qt::Key_Down);
    press(Qt::Key_Down);
    ASSERT_EQ(active.count(), 3);
    EXPECT_EQ(active.at(0).at(0).toULongLong(), 2U);
    EXPECT_EQ(active.at(1).at(0).toULongLong(), 4U);
    EXPECT_EQ(active.at(2).at(0).toULongLong(), 1U);
}

TEST_F(QuickPickTest, TypingTellsWhichItemTheFilterLeavesChosen) {
    pick_->set_items(items_of({"Alpha", "Beta", "Gamma"}));
    pick_->open();
    QSignalSpy active(pick_, &QuickPick::active_changed);
    type("gam");
    ASSERT_GE(active.count(), 1);
    EXPECT_EQ(active.last().at(0).toULongLong(), 2U);
}

TEST_F(QuickPickTest, AListThatTheCallerFiltersIsDrawnWithTheCallersHighlights) {
    QuickPickItem item{.label = "main.cpp", .description = "src", .detail = {}, .keybinding = {}};
    item.label_match = {{0, 2}};
    pick_->set_filtering(false);
    pick_->set_items({item});
    pick_->open();
    type("zz");
    ASSERT_EQ(pick_->shown_items(), std::vector<std::size_t>{0});
    EXPECT_EQ(pick_->label_highlights(0), item.label_match);
    // A pick that filters computes its own.
    pick_->filter_field()->clear();
    pick_->set_filtering(true);
    EXPECT_TRUE(pick_->label_highlights(0).empty());
}

TEST_F(QuickPickTest, EnterIsHeldWhileTheItemsAreAwaitedAndThenChooses) {
    QSignalSpy accepted(pick_, &QuickPick::accepted);
    pick_->open();
    pick_->set_awaiting_items(true);
    QTest::keyClick(pick_->filter_field(), Qt::Key_Return);
    EXPECT_EQ(accepted.count(), 0);
    EXPECT_TRUE(pick_->isVisible());
    pick_->set_items(items_of({"new first", "new second"}));
    pick_->set_awaiting_items(false);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_EQ(accepted.at(0).at(0).value<std::size_t>(), 0U);
    EXPECT_FALSE(pick_->isVisible());

    // Without a held Enter nothing happens, and one is not remembered over an opening.
    pick_->open();
    pick_->set_awaiting_items(true);
    pick_->set_awaiting_items(false);
    pick_->set_awaiting_items(true);
    QTest::keyClick(pick_->filter_field(), Qt::Key_Return);
    pick_->cancel();
    pick_->open();
    pick_->set_awaiting_items(false);
    EXPECT_EQ(accepted.count(), 1);
}

TEST_F(QuickPickTest, TheNextRowIsChosenAndTheActiveItemTold) {
    pick_->set_items(items_of({"a", "b"}));
    pick_->open();
    EXPECT_EQ(pick_->active_item(), 0U);
    pick_->select_next();
    EXPECT_EQ(pick_->active_item(), 1U);
    pick_->select_next();
    EXPECT_EQ(pick_->active_item(), 0U);
    pick_->set_items({});
    EXPECT_EQ(pick_->active_item(), std::nullopt);
    pick_->select_next();
}

TEST_F(QuickPickTest, TheListHasASlimScrollBarOnTheColorOfThePopup) {
    colors_.set_colors({{"quickInput.background", QColor("#102030")},
                        {"scrollbarSlider.background", QColor("#ff0000")}});
    std::vector<QString> labels;
    for (int i = 0; i < 80; ++i) {
        labels.push_back(QString("Item %1").arg(i));
    }
    pick_->set_items(items_of(labels));
    pick_->open();
    QApplication::processEvents();
    auto* bar = qobject_cast<anvil::ui::ScrollBar*>(pick_->list()->verticalScrollBar());
    ASSERT_NE(bar, nullptr);
    ASSERT_TRUE(bar->isVisible());
    EXPECT_EQ(bar->width(), anvil::ui::ScrollBar::list_width);
    QEvent left(QEvent::Leave);
    QApplication::sendEvent(bar, &left);
    const QImage image = bar->grab().toImage();
    EXPECT_EQ(image.pixelColor(5, bar->slider_rect().center().y()), QColor("#ff0000"));
    EXPECT_EQ(image.pixelColor(5, bar->height() - 1), QColor("#102030"));
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        anvil::ui::ThemeStore store(colors_, anvil::ui::themes_directory(),
                                    std::make_shared<anvil::core::RecordingLogger>());
        for (const auto* theme : {"Dark 2026", "Light+"}) {
            ASSERT_TRUE(store.select(theme));
            QApplication::processEvents();
            host_->grab().save(QString::fromUtf8(directory) + "/scroll_bar_pick_" + theme + ".png");
        }
    }
}

namespace {

// A list whose first item has a solid icon, red on a dark theme and blue on a light one, and whose
// second has none, with green text on black.
class QuickPickIconTest : public QuickPickTest {
protected:
    void SetUp() override {
        QuickPickTest::SetUp();
        theme(anvil::core::ThemeKind::dark);
        std::vector<QuickPickItem> items = items_of({"HHHH", "HHHH"});
        items[0].icon = anvil::ui::QuickPickIcon{
            .font_family = anvil::ui::codicon_font_family(),
            .dark = {*anvil::ui::codicon("circle-filled"), QColor(255, 0, 0)},
            .light = {*anvil::ui::codicon("circle-filled"), QColor(0, 0, 255)},
        };
        pick_->set_items(std::move(items));
        pick_->open();
    }

    void theme(anvil::core::ThemeKind kind) {
        anvil::ui::WorkbenchColors::Colors colors;
        for (const char* key : {"quickInput.background", "quickInputList.focusBackground"}) {
            colors[key] = QColor(0, 0, 0);
        }
        for (const char* key : {"quickInput.foreground", "quickInputList.focusForeground"}) {
            colors[key] = QColor(0, 255, 0);
        }
        colors_.set_theme(colors, nullptr, kind);
    }

    // The leftmost column of the row with a pixel of the color of one channel, or -1.
    int leftmost(int row, int channel) const {
        QCoreApplication::processEvents();
        const QImage image = pick_->list()->viewport()->grab().toImage();
        const QRect rect = pick_->list()->visualRect(pick_->list()->model()->index(row, 0));
        const qreal ratio = image.devicePixelRatio();
        for (int x = 0; x < image.width(); ++x) {
            for (int y = static_cast<int>(rect.top() * ratio);
                 y < static_cast<int>(rect.bottom() * ratio) && y < image.height(); ++y) {
                const QColor color = image.pixelColor(x, y);
                const int channels[] = {color.red(), color.green(), color.blue()};
                const int others = channels[0] + channels[1] + channels[2] - channels[channel];
                if (channels[channel] > 150 && others < 80) {
                    return static_cast<int>(x / ratio);
                }
            }
        }
        return -1;
    }
    static constexpr int red = 0;
    static constexpr int green = 1;
    static constexpr int blue = 2;
};

} // namespace

TEST_F(QuickPickIconTest, TheIconIsInFrontOfTheLabelInItsColor) {
    const int icon = leftmost(0, red);
    ASSERT_GE(icon, 0);
    EXPECT_LT(icon, leftmost(0, green));
    EXPECT_EQ(leftmost(0, blue), -1);
    EXPECT_EQ(leftmost(1, red), -1);
}

TEST_F(QuickPickIconTest, OnlyTheLabelOfAnItemWithAnIconMakesRoomForIt) {
    const int with_icon = leftmost(0, green);
    const int without_icon = leftmost(1, green);
    ASSERT_GE(without_icon, 0);
    EXPECT_EQ(with_icon - without_icon, 22);

    pick_->set_items(items_of({"HHHH"}));
    EXPECT_EQ(leftmost(0, green), without_icon);
}

TEST_F(QuickPickIconTest, TheIconFollowsAChangeOfTheKindOfTheTheme) {
    ASSERT_GE(leftmost(0, red), 0);
    theme(anvil::core::ThemeKind::light);
    EXPECT_GE(leftmost(0, blue), 0);
    EXPECT_EQ(leftmost(0, red), -1);
    // A high contrast light theme shows the dark icon, as VS Code does.
    theme(anvil::core::ThemeKind::high_contrast_light);
    EXPECT_GE(leftmost(0, red), 0);
    EXPECT_EQ(leftmost(0, blue), -1);
}

TEST_F(QuickPickIconTest, TheRoomForAnIconGrowsWithTheZoom) {
    anvil::ui::Zoom zoom;
    zoom.set_level(4);
    pick_->set_zoom(&zoom);
    // The column at level 0 scaled, to a pixel or so of rounding.
    EXPECT_NEAR(leftmost(0, green) - leftmost(1, green), 22 * zoom.factor(), 1.5);
    pick_->set_zoom(nullptr);
}
