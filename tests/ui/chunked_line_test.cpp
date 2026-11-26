#include "anvil/ui/chunked_line.hpp"

#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextOption>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using anvil::ui::ChunkedLine;

namespace {

ChunkedLine::Style style(const QString& family = QStringLiteral("monospace")) {
    QFont font(family);
    font.setPixelSize(14);
    const QFontMetricsF metrics(font);
    return {font, 4 * static_cast<qreal>(static_cast<int>(metrics.horizontalAdvance(u'M'))),
            metrics.horizontalAdvance(u'M')};
}

// The line laid out in one piece, which the chunks have to agree with.
class WholeLine {
public:
    WholeLine(const QString& text, const ChunkedLine::Style& style,
              const QList<QTextLayout::FormatRange>& formats = {})
        : layout_(text, style.font) {
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setTabStopDistance(style.tab_stop);
        layout_.setTextOption(option);
        layout_.setFormats(formats);
        layout_.beginLayout();
        QTextLine line = layout_.createLine();
        line.setLineWidth(1 << 24);
        layout_.endLayout();
    }
    qreal x_of(std::size_t column) const {
        return layout_.lineAt(0).cursorToX(static_cast<int>(column));
    }
    std::size_t column_at(qreal x) const {
        return static_cast<std::size_t>(layout_.lineAt(0).xToCursor(x));
    }
    qreal width() const {
        return layout_.lineAt(0).horizontalAdvance();
    }
    QTextLayout& layout() {
        return layout_;
    }

private:
    QTextLayout layout_;
};

const QStringList& samples() {
    static const QStringList lines{
        QStringLiteral("the quick brown fox jumps over the lazy dog, again and again"),
        QStringLiteral("a\tbc\tdef\t\tg\th\tijklmnop\tq\t\t\tr\tstuvwxyz\t0123456789\tend"),
        QString::fromUtf8("int 中文 = 1;\t// 漢字かな交じり文\t\"全角\" abc\t中中中中中中中中\tx"),
        QString::fromUtf8(
            "🙂🙂 a🙂b éé é\t🚀🚀🚀🚀 ñññññ 👩‍🚀👩‍🚀 end"),
        QString(),
        QStringLiteral("x"),
    };
    return lines;
}

constexpr qreal everything = 1e9;

// The pixels whose brightness differs by more than the smoothing of an edge does.
int pixels_that_differ(const QImage& a, const QImage& b) {
    int differ = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            differ += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y))) > 48 ? 1 : 0;
        }
    }
    return differ;
}

} // namespace

TEST(ChunkedLineTest, EveryColumnIsWhereASingleLayoutPutsIt) {
    for (const QString& family : {QStringLiteral("monospace"), QStringLiteral("Sans Serif")}) {
        for (const QString& text : samples()) {
            const WholeLine whole(text, style(family));
            // A proportional font can kern the two characters at the end of a chunk apart.
            const qreal kerning = family == QStringLiteral("monospace") ? 0.5 : 1.0;
            for (const std::size_t length : {6U, 8U, 13U, 21U}) {
                ChunkedLine line(text, {}, style(family), nullptr, length);
                line.columns_between(0, everything);
                for (std::size_t column = 0; column <= line.size(); ++column) {
                    if (column < line.size() &&
                        text[static_cast<qsizetype>(column)].isLowSurrogate()) {
                        continue;
                    }
                    EXPECT_NEAR(line.x_of(column), whole.x_of(column), kerning)
                        << family.toStdString() << " '" << text.toStdString() << "' length "
                        << length << " column " << column;
                }
                EXPECT_NEAR(line.width(), whole.width(), kerning) << text.toStdString();
            }
        }
    }
}

TEST(ChunkedLineTest, TheColumnNearestToAPointIsThatOfASingleLayout) {
    for (const QString& text : samples()) {
        // For a point in a joined emoji, a single layout can answer with the end of the line.
        if (text.contains(QChar(0x200D))) {
            continue;
        }
        const WholeLine whole(text, style());
        for (const std::size_t length : {6U, 8U, 21U}) {
            ChunkedLine line(text, {}, style(), nullptr, length);
            line.columns_between(0, everything);
            for (qreal x = -5; x < whole.width() + 20; x += 1.3) {
                // Characters without a width share their x with a neighbor, so the places are
                // compared.
                EXPECT_NEAR(whole.x_of(line.column_at(x)), whole.x_of(whole.column_at(x)), 0.5)
                    << "'" << text.toStdString() << "' length " << length << " x " << x;
            }
        }
    }
}

TEST(ChunkedLineTest, AMonospaceLineWithoutWideCharactersIsPlacedBeforeItIsLaidOut) {
    QString text;
    for (int i = 0; i < 400; ++i) {
        text += QStringLiteral("ab\tcde\tf ");
    }
    const WholeLine whole(text, style());
    ChunkedLine line(text, {}, style(), nullptr, 100);
    const std::size_t column = line.size() - 30;
    EXPECT_NEAR(line.x_of(column), whole.x_of(column), 1.0);
    EXPECT_NEAR(line.width(), whole.width(), 1.0);
}

TEST(ChunkedLineTest, TheWidthOfALineOfOneChunkIsMeasured) {
    const QString text(40, QChar(0x4E2D));
    ChunkedLine line(text, {}, style());
    EXPECT_NEAR(line.width(), WholeLine(text, style()).width(), 0.5);
}

TEST(ChunkedLineTest, APointInALineThatIsWiderThanEstimatedFindsItsColumn) {
    const QString text(200, QChar(0x4E2D));
    const WholeLine whole(text, style());
    ASSERT_GT(whole.width(), 1.3 * 200 * style().advance);
    ChunkedLine line(text, {}, style(), nullptr, 10);
    line.columns_between(0, everything);
    EXPECT_EQ(line.column_at(whole.x_of(150) + 1), 150U);

    // Nothing is laid out here, so the point is beyond the estimate of the chunk it starts in.
    ChunkedLine fresh(text, {}, style(), nullptr, 10);
    const qreal x = fresh.x_of(20) + 1;
    EXPECT_EQ(fresh.column_at(x), 20U);
}

TEST(ChunkedLineTest, APointPastAChunkThatIsNarrowerThanEstimatedIsLeftToALaterOne) {
    const ChunkedLine::Style narrow{style().font, style().tab_stop, 2 * style().advance};
    ChunkedLine line(QString(100, u'a'), {}, narrow, nullptr, 10);
    // Estimated at twice its width, the second chunk covers a point that is past it once laid out.
    const qreal point = 35 * style().advance;
    const std::size_t column = line.column_at(point);
    EXPECT_GT(column, 20U);
    EXPECT_NEAR(line.x_of(column), point, style().advance);
}

TEST(ChunkedLineTest, AChunkNeverSplitsASurrogatePairOrAGraphemeCluster) {
    const QString text = QString::fromUtf8(
        "a🙂🙂🙂éééééé👩‍🚀👩‍🚀🇬🇭🇬🇭 plain words");
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    for (const std::size_t length : {2U, 3U, 4U, 7U, 11U}) {
        ChunkedLine line(text, {}, style(), nullptr, length);
        ASSERT_GT(line.chunk_count(), 1U);
        for (std::size_t chunk = 1; chunk < line.chunk_count(); ++chunk) {
            const std::size_t begin = line.chunk_begin(chunk);
            ASSERT_GT(begin, line.chunk_begin(chunk - 1));
            EXPECT_FALSE(text[static_cast<qsizetype>(begin)].isLowSurrogate())
                << "length " << length;
            if (length >= 11) {
                finder.setPosition(static_cast<qsizetype>(begin));
                EXPECT_TRUE(finder.isAtBoundary()) << "column " << begin;
            }
        }
    }
}

TEST(ChunkedLineTest, OnlyTheChunksThatAreAskedAboutAreLaidOut) {
    std::size_t built = 0;
    ChunkedLine line(QString(100000, u'a'), {}, style(), &built, 1000);
    EXPECT_EQ(line.chunk_count(), 100U);
    line.width();
    EXPECT_EQ(built, 0U);

    line.x_of(50500);
    EXPECT_EQ(built, 1U);
    line.x_of(50600);
    EXPECT_EQ(built, 1U);

    const qreal left = line.x_of(70500);
    const auto [first, last] = line.columns_between(left, left + 800);
    EXPECT_EQ(first, 70000U);
    EXPECT_EQ(last, 71000U);
    EXPECT_EQ(built, 2U);

    const auto none = line.columns_between(line.width() + 100, line.width() + 900);
    EXPECT_EQ(none.first, line.size());
    EXPECT_EQ(none.second, line.size());
    EXPECT_EQ(built, 2U);
}

TEST(ChunkedLineTest, TheLayoutsOfALineAreBounded) {
    std::size_t built = 0;
    ChunkedLine line(QString(10000, u'a'), {}, style(), &built, 100);
    for (std::size_t column = 0; column < 10000; column += 100) {
        line.x_of(column);
    }
    EXPECT_EQ(built, 100U);
    line.x_of(9900);
    EXPECT_EQ(built, 100U);
    line.x_of(0);
    EXPECT_EQ(built, 101U);
}

TEST(ChunkedLineTest, TheWidthsBeforeAnEditAreKept) {
    const QString text(300, QChar(0x4E2D));
    ChunkedLine earlier(text, {}, style(), nullptr, 20);
    earlier.columns_between(0, everything);
    const qreal before_edit = earlier.x_of(100);
    const qreal estimate = ChunkedLine(text, {}, style(), nullptr, 20).x_of(100);
    ASSERT_GT(before_edit, estimate * 1.3);

    QString edited = text;
    edited.insert(205, u'x');
    ChunkedLine kept(edited, {}, style(), nullptr, 20);
    kept.keep_widths(earlier, 205);
    EXPECT_NEAR(kept.x_of(100), before_edit, 0.5);

    ChunkedLine none(edited, {}, style(), nullptr, 20);
    none.keep_widths(earlier, 0);
    EXPECT_NEAR(none.x_of(100), estimate, 0.5);

    // The chunk that the edit is in is not the earlier one.
    ChunkedLine partly(edited, {}, style(), nullptr, 20);
    partly.keep_widths(earlier, 90);
    EXPECT_NEAR(partly.x_of(100) - partly.x_of(80),
                ChunkedLine(text, {}, style(), nullptr, 20).x_of(20), 0.5);
}

TEST(ChunkedLineTest, DrawsWhatASingleLayoutDraws) {
    const QString text = QStringLiteral("alpha\tbeta gamma\tdelta epsilon zeta\teta theta iota");
    QTextCharFormat keyword;
    keyword.setForeground(Qt::red);
    keyword.setFontWeight(QFont::Bold);
    QTextCharFormat background;
    background.setBackground(Qt::blue);
    // Both cross the ends of chunks.
    const QList<QTextLayout::FormatRange> syntax{{3, 15, keyword}, {30, 12, keyword}};
    const QList<QTextLayout::FormatRange> selection{{10, 25, background}};

    const auto render = [](const auto& draw) {
        QImage image(600, 24, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        draw(painter);
        return image;
    };
    WholeLine whole(text, style(), syntax);
    const QImage expected =
        render([&](QPainter& painter) { whole.layout().draw(&painter, QPointF(4, 2), selection); });
    for (const std::size_t length : {4U, 9U, 16U}) {
        ChunkedLine line(text, syntax, style(), nullptr, length);
        const QImage image = render(
            [&](QPainter& painter) { line.draw(painter, QPointF(4, 2), selection, -4, 596); });
        // Glyph edges can round differently where a chunk starts between pixels.
        EXPECT_EQ(pixels_that_differ(image, expected), 0) << "length " << length;
    }
}
