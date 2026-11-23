#include "anvil/ui/window_frame.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <gtest/gtest.h>
#include <memory>

using anvil::ui::FrameBorder;
using anvil::ui::resize_cursor;
using anvil::ui::resize_edges_at;
using anvil::ui::WindowFrame;
using anvil::ui::WorkbenchColors;

namespace {

TEST(ResizeEdges, TheOuterPixelsAreTheHandles) {
    const QSize size(200, 100);
    EXPECT_EQ(resize_edges_at(size, QPoint(0, 50), 5), Qt::LeftEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(4, 50), 5), Qt::LeftEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(5, 50), 5), Qt::Edges());
    EXPECT_EQ(resize_edges_at(size, QPoint(199, 50), 5), Qt::RightEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(195, 50), 5), Qt::RightEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(194, 50), 5), Qt::Edges());
    EXPECT_EQ(resize_edges_at(size, QPoint(100, 0), 5), Qt::TopEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(100, 99), 5), Qt::BottomEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(100, 50), 5), Qt::Edges());
}

TEST(ResizeEdges, TheCornersAreTwoEdges) {
    const QSize size(200, 100);
    EXPECT_EQ(resize_edges_at(size, QPoint(0, 0), 5), Qt::LeftEdge | Qt::TopEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(199, 0), 5), Qt::RightEdge | Qt::TopEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(0, 99), 5), Qt::LeftEdge | Qt::BottomEdge);
    EXPECT_EQ(resize_edges_at(size, QPoint(199, 99), 5), Qt::RightEdge | Qt::BottomEdge);
}

TEST(ResizeEdges, APointOutsideTheWindowIsOnNoEdge) {
    const QSize size(200, 100);
    EXPECT_EQ(resize_edges_at(size, QPoint(-1, 50), 5), Qt::Edges());
    EXPECT_EQ(resize_edges_at(size, QPoint(200, 50), 5), Qt::Edges());
    EXPECT_EQ(resize_edges_at(size, QPoint(50, -1), 5), Qt::Edges());
    EXPECT_EQ(resize_edges_at(size, QPoint(50, 100), 5), Qt::Edges());
}

TEST(ResizeEdges, TheCursorShowsTheDirection) {
    EXPECT_EQ(resize_cursor({}), Qt::ArrowCursor);
    EXPECT_EQ(resize_cursor(Qt::LeftEdge), Qt::SizeHorCursor);
    EXPECT_EQ(resize_cursor(Qt::RightEdge), Qt::SizeHorCursor);
    EXPECT_EQ(resize_cursor(Qt::TopEdge), Qt::SizeVerCursor);
    EXPECT_EQ(resize_cursor(Qt::BottomEdge), Qt::SizeVerCursor);
    EXPECT_EQ(resize_cursor(Qt::LeftEdge | Qt::TopEdge), Qt::SizeFDiagCursor);
    EXPECT_EQ(resize_cursor(Qt::RightEdge | Qt::BottomEdge), Qt::SizeFDiagCursor);
    EXPECT_EQ(resize_cursor(Qt::RightEdge | Qt::TopEdge), Qt::SizeBDiagCursor);
    EXPECT_EQ(resize_cursor(Qt::LeftEdge | Qt::BottomEdge), Qt::SizeBDiagCursor);
}

// A child that fills the window and counts the presses that reach it.
class Content : public QWidget {
public:
    using QWidget::QWidget;
    int presses = 0;

protected:
    void mousePressEvent(QMouseEvent*) override {
        ++presses;
    }
};

class WindowFrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        window_ = std::make_unique<QWidget>();
        window_->setWindowFlag(Qt::FramelessWindowHint);
        window_->resize(300, 200);
        auto* layout = new QVBoxLayout(window_.get());
        layout->setContentsMargins(0, 0, 0, 0);
        content_ = new Content;
        layout->addWidget(content_);
        frame_ = new WindowFrame(window_.get());
        window_->show();
        QApplication::processEvents();
    }
    void TearDown() override {
        while (QGuiApplication::overrideCursor()) {
            QGuiApplication::restoreOverrideCursor();
        }
    }

    void send(QWidget* widget, QEvent::Type type, QPoint in_window,
              Qt::MouseButton button = Qt::NoButton) {
        const QPoint at = widget->mapFrom(window_.get(), in_window);
        QMouseEvent event(type, at, window_->mapToGlobal(in_window), button, button,
                          Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
    std::optional<Qt::CursorShape> cursor() const {
        const QCursor* current = QGuiApplication::overrideCursor();
        return current ? std::optional(current->shape()) : std::nullopt;
    }

    std::unique_ptr<QWidget> window_;
    Content* content_ = nullptr;
    WindowFrame* frame_ = nullptr;
};

TEST_F(WindowFrameTest, TheResizeCursorShowsOverAHandleWhateverLiesUnderIt) {
    send(content_, QEvent::MouseMove, QPoint(150, 100));
    EXPECT_EQ(cursor(), std::nullopt);
    send(content_, QEvent::MouseMove, QPoint(0, 100));
    EXPECT_EQ(cursor(), Qt::SizeHorCursor);
    send(content_, QEvent::MouseMove, QPoint(299, 199));
    EXPECT_EQ(cursor(), Qt::SizeFDiagCursor);
    send(content_, QEvent::MouseMove, QPoint(150, 100));
    EXPECT_EQ(cursor(), std::nullopt);
}

TEST_F(WindowFrameTest, LeavingTheWindowFromAHandleRestoresTheCursor) {
    send(content_, QEvent::MouseMove, QPoint(150, 199));
    ASSERT_EQ(cursor(), Qt::SizeVerCursor);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(window_.get(), &leave);
    EXPECT_EQ(cursor(), std::nullopt);
}

TEST_F(WindowFrameTest, APressOnAHandleStartsAResizeAndDoesNotReachTheWidgetUnderIt) {
    QSignalSpy failed(frame_, &WindowFrame::resize_failed);
    send(content_, QEvent::MouseButtonPress, QPoint(2, 100), Qt::LeftButton);
    EXPECT_EQ(content_->presses, 0);
    // The offscreen platform cannot start a resize, and says so.
    EXPECT_EQ(failed.count(), 1);

    send(content_, QEvent::MouseButtonPress, QPoint(150, 100), Qt::LeftButton);
    EXPECT_EQ(content_->presses, 1);
    EXPECT_EQ(failed.count(), 1);
}

TEST_F(WindowFrameTest, OnlyTheLeftButtonStartsAResize) {
    QSignalSpy failed(frame_, &WindowFrame::resize_failed);
    send(content_, QEvent::MouseButtonPress, QPoint(2, 100), Qt::RightButton);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(content_->presses, 1);
}

TEST_F(WindowFrameTest, DisablingTheHandlesGivesTheEdgeBackToTheWidgets) {
    QSignalSpy failed(frame_, &WindowFrame::resize_failed);
    send(content_, QEvent::MouseMove, QPoint(0, 100));
    ASSERT_EQ(cursor(), Qt::SizeHorCursor);

    frame_->set_enabled(false);
    EXPECT_EQ(cursor(), std::nullopt);
    send(content_, QEvent::MouseMove, QPoint(0, 100));
    EXPECT_EQ(cursor(), std::nullopt);
    send(content_, QEvent::MouseButtonPress, QPoint(2, 100), Qt::LeftButton);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(content_->presses, 1);
}

TEST_F(WindowFrameTest, AMaximizedWindowHasNoHandles) {
    window_->showMaximized();
    QApplication::processEvents();
    ASSERT_TRUE(window_->isMaximized());
    QSignalSpy failed(frame_, &WindowFrame::resize_failed);
    send(content_, QEvent::MouseMove, QPoint(0, 100));
    EXPECT_EQ(cursor(), std::nullopt);
    send(content_, QEvent::MouseButtonPress, QPoint(0, 100), Qt::LeftButton);
    EXPECT_EQ(failed.count(), 0);
}

TEST_F(WindowFrameTest, AFullscreenWindowReportsWhereThePointerIsAndHasNoHandles) {
    window_->showFullScreen();
    QApplication::processEvents();
    ASSERT_TRUE(window_->isFullScreen());
    QSignalSpy moved(frame_, &WindowFrame::fullscreen_pointer_moved);
    send(content_, QEvent::MouseMove, QPoint(0, 0));
    send(content_, QEvent::MouseMove, QPoint(50, 80));
    ASSERT_EQ(moved.count(), 2);
    EXPECT_EQ(moved.at(0).at(0).toInt(), 0);
    EXPECT_EQ(moved.at(1).at(0).toInt(), 80);
    EXPECT_EQ(cursor(), std::nullopt);
}

TEST_F(WindowFrameTest, AWidgetAddedLaterTracksTheMouseToo) {
    auto* late = new QWidget(content_);
    EXPECT_TRUE(late->hasMouseTracking());
    auto* nested = new QWidget(late);
    EXPECT_TRUE(nested->hasMouseTracking());
    EXPECT_TRUE(content_->hasMouseTracking());
}

TEST_F(WindowFrameTest, DestroyingTheFrameRestoresTheCursor) {
    send(content_, QEvent::MouseMove, QPoint(0, 100));
    ASSERT_EQ(cursor(), Qt::SizeHorCursor);
    delete frame_;
    EXPECT_EQ(cursor(), std::nullopt);
}

TEST(FrameBorderTest, DrawsOneLineAroundTheWindowAboveItsChildren) {
    WorkbenchColors colors{QApplication::palette()};
    colors.set_colors({{"titleBar.border", QColor("#2b2b2b")}});
    QWidget window;
    window.resize(120, 80);
    window.setStyleSheet("QWidget { background: #101010; }");
    auto* border = new FrameBorder(colors, &window);
    window.show();
    QApplication::processEvents();

    auto* later = new QWidget(&window);
    later->setGeometry(0, 0, 120, 80);
    later->setStyleSheet("background: #ff0000;");
    later->show();
    QApplication::processEvents();

    const QImage image = window.grab().toImage();
    EXPECT_EQ(border->geometry(), window.rect());
    EXPECT_TRUE(border->testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_EQ(image.pixelColor(0, 0), QColor("#2b2b2b"));
    EXPECT_EQ(image.pixelColor(119, 79), QColor("#2b2b2b"));
    EXPECT_EQ(image.pixelColor(60, 0), QColor("#2b2b2b"));
    EXPECT_EQ(image.pixelColor(0, 40), QColor("#2b2b2b"));
    EXPECT_EQ(image.pixelColor(1, 1), QColor("#ff0000"));
    EXPECT_EQ(image.pixelColor(60, 40), QColor("#ff0000"));

    window.resize(200, 100);
    QApplication::processEvents();
    EXPECT_EQ(border->geometry(), window.rect());
}

} // namespace
