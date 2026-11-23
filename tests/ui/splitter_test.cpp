#include "anvil/ui/splitter.hpp"

#include <QApplication>
#include <QEnterEvent>
#include <QLabel>
#include <QSplitterHandle>
#include <gtest/gtest.h>
#include <memory>

using namespace anvil::ui;

namespace {

class SplitterTest : public ::testing::Test {
protected:
    void SetUp() override {
        colors_.set_colors({{"editor.background", QColor("#101010")},
                            {"panel.border", QColor("#ff0000")},
                            {"sash.hoverBorder", QColor("#00ff00")}});
        splitter_ = std::make_unique<Splitter>(Qt::Horizontal, colors_, "panel.border");
        splitter_->addWidget(new QLabel("one"));
        splitter_->addWidget(new QLabel("two"));
        splitter_->resize(400, 100);
        splitter_->show();
        QApplication::processEvents();
    }

    QImage grab_handle() {
        return splitter_->handle(1)->grab().toImage();
    }

    WorkbenchColors colors_{QApplication::palette()};
    std::unique_ptr<Splitter> splitter_;
};

} // namespace

TEST_F(SplitterTest, TheHandleIsAFourPixelSashThatCannotCollapseItsParts) {
    EXPECT_EQ(splitter_->handleWidth(), 4);
    EXPECT_EQ(splitter_->handle(1)->width(), 4);
    EXPECT_FALSE(splitter_->childrenCollapsible());
}

TEST_F(SplitterTest, TheHandleShowsAOnePixelLineOfTheBorderColorOnTheEditorBackground) {
    const QImage image = grab_handle();
    int line = 0;
    int background = 0;
    for (int x = 0; x < image.width(); ++x) {
        const QColor c(image.pixel(x, 50));
        line += c == QColor("#ff0000");
        background += c == QColor("#101010");
    }
    EXPECT_EQ(line, 1);
    EXPECT_EQ(background, image.width() - 1);
}

TEST_F(SplitterTest, AHandleThatIsHoveredIsFilledWithTheSashColor) {
    QSplitterHandle* handle = splitter_->handle(1);
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QApplication::sendEvent(handle, &enter);
    QImage image = grab_handle();
    EXPECT_EQ(QColor(image.pixel(0, 50)), QColor("#00ff00"));
    EXPECT_EQ(QColor(image.pixel(image.width() - 1, 50)), QColor("#00ff00"));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(handle, &leave);
    image = grab_handle();
    EXPECT_NE(QColor(image.pixel(0, 50)), QColor("#00ff00"));
}

TEST_F(SplitterTest, TheLineFollowsTheColorsOfTheTheme) {
    colors_.set_colors(
        {{"editor.background", QColor("#101010")}, {"panel.border", QColor("#0000ff")}});
    const QImage image = grab_handle();
    int line = 0;
    for (int x = 0; x < image.width(); ++x) {
        line += QColor(image.pixel(x, 50)) == QColor("#0000ff");
    }
    EXPECT_EQ(line, 1);
}

TEST_F(SplitterTest, AVerticalSplitterDrawsAHorizontalLine) {
    auto vertical = std::make_unique<Splitter>(Qt::Vertical, colors_, "panel.border");
    vertical->addWidget(new QLabel("one"));
    vertical->addWidget(new QLabel("two"));
    vertical->resize(100, 400);
    vertical->show();
    QApplication::processEvents();
    const QImage image = vertical->handle(1)->grab().toImage();
    int line = 0;
    for (int y = 0; y < image.height(); ++y) {
        line += QColor(image.pixel(50, y)) == QColor("#ff0000");
    }
    EXPECT_EQ(line, 1);
    EXPECT_EQ(image.height(), 4);
}
