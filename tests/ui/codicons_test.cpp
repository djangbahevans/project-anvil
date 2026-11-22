#include "anvil/ui/codicons.hpp"

#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QImage>
#include <QScreen>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace anvil::ui;

namespace {

int highest_alpha(const QImage& image) {
    int highest = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            highest = std::max(highest, qAlpha(image.pixel(x, y)));
        }
    }
    return highest;
}

QImage render(const QIcon& icon, QIcon::Mode mode, int size) {
    return icon.pixmap(QSize(size, size), mode).toImage().convertToFormat(QImage::Format_ARGB32);
}

std::filesystem::path codicons_package() {
    return std::filesystem::path(ANVIL_VSCODE_DIR) / "node_modules" / "@vscode" / "codicons";
}

LabelSegment text(const char* value) {
    return {.text = QString::fromUtf8(value)};
}
LabelSegment icon(const char* name, const char* modifier = "") {
    return {.icon = name, .modifier = modifier};
}

using Segments = std::vector<LabelSegment>;

} // namespace

TEST(CodiconsTest, TheTableIsOrderedByNameWithoutDuplicates) {
    const auto table = codicon_table();
    ASSERT_GT(table.size(), 600U);
    for (std::size_t i = 1; i < table.size(); ++i) {
        EXPECT_LT(table[i - 1].name, table[i].name) << table[i].name;
    }
}

TEST(CodiconsTest, ANameGivesItsCharacterInTheFont) {
    EXPECT_EQ(codicon("add"), char16_t(0xEA60));
    EXPECT_EQ(codicon("account"), char16_t(0xEB99));
    EXPECT_EQ(codicon("zoom-out"), char16_t(0xEB82));
}

TEST(CodiconsTest, EveryNameOfTheTableResolvesToItsCode) {
    for (const CodiconEntry& entry : codicon_table()) {
        EXPECT_EQ(codicon(entry.name), entry.code) << entry.name;
    }
}

TEST(CodiconsTest, AnUnknownNameGivesNothing) {
    EXPECT_EQ(codicon("no-such-icon"), std::nullopt);
    EXPECT_EQ(codicon(""), std::nullopt);
    EXPECT_EQ(codicon("ADD"), std::nullopt);
    EXPECT_EQ(codicon("add "), std::nullopt);
    EXPECT_EQ(codicon("a"), std::nullopt);
    EXPECT_EQ(codicon("zzzz"), std::nullopt);
}

TEST(CodiconsTest, TheFontLoadsAndHasTheGlyphs) {
    const QString family = codicon_font_family();
    ASSERT_FALSE(family.isEmpty());
    QFont font(family);
    font.setPixelSize(16);
    const QFontMetrics metrics(font);
    for (const CodiconEntry& entry : codicon_table()) {
        EXPECT_TRUE(metrics.inFontUcs4(entry.code)) << entry.name;
    }
}

TEST(CodiconsTest, EveryNameOfTheTableDrawsSomethingExceptTheBlankIcon) {
    for (const CodiconEntry& entry : codicon_table()) {
        const QIcon drawn = codicon_icon(entry.name, Qt::white, 16);
        ASSERT_FALSE(drawn.isNull()) << entry.name;
        const int alpha = highest_alpha(render(drawn, QIcon::Normal, 16));
        if (entry.name == "blank") {
            EXPECT_EQ(alpha, 0);
        } else {
            EXPECT_GT(alpha, 0) << entry.name;
        }
    }
}

TEST(CodiconsTest, AnUnknownNameGivesANullIconAndNoCrash) {
    EXPECT_TRUE(codicon_icon("no-such-icon", Qt::white, 16).isNull());
    EXPECT_TRUE(codicon_icon("", Qt::white, 16).isNull());
}

TEST(CodiconsTest, ASizeThatIsNotPositiveGivesANullIcon) {
    EXPECT_TRUE(codicon_icon("add", Qt::white, 0).isNull());
    EXPECT_TRUE(codicon_icon("add", Qt::white, -4).isNull());
}

TEST(CodiconsTest, TheIconHasTheRequestedSizeAndColor) {
    const QIcon drawn = codicon_icon("circle-filled", QColor(10, 200, 30), 24);
    const QImage image = render(drawn, QIcon::Normal, 24);
    EXPECT_EQ(image.size(), QSize(24, 24) * QApplication::primaryScreen()->devicePixelRatio());
    const QColor center(image.pixel(image.width() / 2, image.height() / 2));
    EXPECT_EQ(center.alpha(), 255);
    EXPECT_EQ(center.red(), 10);
    EXPECT_EQ(center.green(), 200);
    EXPECT_EQ(center.blue(), 30);
}

TEST(CodiconsTest, ABiggerSizeDrawsAGlyphThatCoversMore) {
    const auto covered = [](int size) {
        const QImage image =
            render(codicon_icon("circle-filled", Qt::white, size), QIcon::Normal, size);
        int pixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                pixels += qAlpha(image.pixel(x, y)) > 128;
            }
        }
        return pixels;
    };
    EXPECT_GT(covered(32), covered(16) * 2);
}

TEST(CodiconsTest, TheActiveAndSelectedStatesDrawLikeTheNormalOneAndTheDisabledStateIsFainter) {
    const QIcon drawn = codicon_icon("circle-filled", Qt::white, 16);
    const QImage normal = render(drawn, QIcon::Normal, 16);
    EXPECT_EQ(render(drawn, QIcon::Active, 16), normal);
    EXPECT_EQ(render(drawn, QIcon::Selected, 16), normal);
    const QImage disabled = render(drawn, QIcon::Disabled, 16);
    EXPECT_NE(disabled, normal);
    EXPECT_EQ(highest_alpha(normal), 255);
    EXPECT_NEAR(highest_alpha(disabled), 102, 2);
}

TEST(CodiconsTest, TheFontIsTheOneInTheCheckout) {
    const auto package = codicons_package();
    if (!std::filesystem::exists(package / "dist" / "codicon.ttf")) {
        GTEST_SKIP() << "needs the pinned VS Code checkout (scripts/fetch-vscode.sh)";
    }
    std::ifstream file(package / "dist" / "codicon.ttf", std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    QFile resource(":/anvil/codicons/codicon.ttf");
    ASSERT_TRUE(resource.open(QIODevice::ReadOnly));
    const QByteArray embedded = resource.readAll();
    EXPECT_EQ(std::string(embedded.constData(), static_cast<std::size_t>(embedded.size())),
              content.str());
}

TEST(CodiconsTest, TheTableIsTheOneInTheCheckout) {
    const auto csv = codicons_package() / "dist" / "codicon.csv";
    if (!std::filesystem::exists(csv)) {
        GTEST_SKIP() << "needs the pinned VS Code checkout (scripts/fetch-vscode.sh)";
    }
    std::ifstream file(csv);
    std::string line;
    std::getline(file, line);
    ASSERT_EQ(line.substr(0, 26), "short_name,character,unico");
    std::size_t rows = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        ++rows;
        const std::size_t comma = line.find(',');
        const std::string name = line.substr(0, comma);
        const char16_t code =
            static_cast<char16_t>(std::stoul(line.substr(line.rfind(',') + 1), nullptr, 16));
        EXPECT_EQ(codicon(name), code) << name;
    }
    EXPECT_EQ(rows, codicon_table().size());
}

TEST(CodiconLabelsTest, TextWithoutMarkersIsOneSegment) {
    EXPECT_EQ(split_label_with_icons("Ln 1, Col 2"), (Segments{text("Ln 1, Col 2")}));
    EXPECT_EQ(split_label_with_icons(""), Segments{});
}

TEST(CodiconLabelsTest, AMarkerBecomesAnIconSegmentBetweenTheText) {
    EXPECT_EQ(split_label_with_icons("a $(sync) b"),
              (Segments{text("a "), icon("sync"), text(" b")}));
    EXPECT_EQ(split_label_with_icons("$(sync)"), (Segments{icon("sync")}));
    EXPECT_EQ(split_label_with_icons("$(sync) Syncing"),
              (Segments{icon("sync"), text(" Syncing")}));
    EXPECT_EQ(split_label_with_icons("Branch $(git-branch)"),
              (Segments{text("Branch "), icon("git-branch")}));
}

TEST(CodiconLabelsTest, SeveralMarkersGiveSeveralIcons) {
    EXPECT_EQ(split_label_with_icons("$(error) 1 $(warning) 2"),
              (Segments{icon("error"), text(" 1 "), icon("warning"), text(" 2")}));
    EXPECT_EQ(split_label_with_icons("$(a)$(b)"), (Segments{icon("a"), icon("b")}));
}

TEST(CodiconLabelsTest, ANameCanHaveDigitsAndHyphens) {
    EXPECT_EQ(split_label_with_icons("$(chevron-down-2)"), (Segments{icon("chevron-down-2")}));
}

TEST(CodiconLabelsTest, AModifierIsRecognizedAndKeptApart) {
    EXPECT_EQ(split_label_with_icons("$(sync~spin) Syncing"),
              (Segments{icon("sync", "spin"), text(" Syncing")}));
}

TEST(CodiconLabelsTest, AnUnterminatedMarkerIsText) {
    EXPECT_EQ(split_label_with_icons("a $(sync"), (Segments{text("a $(sync")}));
    EXPECT_EQ(split_label_with_icons("$(sync~spin"), (Segments{text("$(sync~spin")}));
    EXPECT_EQ(split_label_with_icons("$("), (Segments{text("$(")}));
    EXPECT_EQ(split_label_with_icons("$(sync $(error)"),
              (Segments{text("$(sync "), icon("error")}));
}

TEST(CodiconLabelsTest, SomethingThatOnlyLooksLikeAMarkerIsText) {
    for (const char* label : {"$()", "$(a b)", "$(a.b)", "$ (sync)", "$(sync~)", "$(sync~1)",
                              "$(sync~spin~x)", "$(ünï)", "$sync)", "(sync)"}) {
        EXPECT_EQ(split_label_with_icons(label), (Segments{text(label)})) << label;
    }
}

TEST(CodiconLabelsTest, ACloseParenthesisAfterAMarkerIsText) {
    EXPECT_EQ(split_label_with_icons("$(sync))"), (Segments{icon("sync"), text(")")}));
    EXPECT_EQ(split_label_with_icons("($(sync))"), (Segments{text("("), icon("sync"), text(")")}));
}

TEST(CodiconLabelsTest, ABackslashBeforeAMarkerMakesItTextWithoutTheBackslash) {
    EXPECT_EQ(split_label_with_icons("\\$(sync)"), (Segments{text("$(sync)")}));
    EXPECT_EQ(split_label_with_icons("a \\$(sync) b $(error)"),
              (Segments{text("a $(sync) b "), icon("error")}));
    EXPECT_EQ(split_label_with_icons("\\$(sync~spin)"), (Segments{text("$(sync~spin)")}));
}

TEST(CodiconLabelsTest, AnEscapedMarkerJoinsTheTextAroundIt) {
    EXPECT_EQ(split_label_with_icons("x\\$(a)y"), (Segments{text("x$(a)y")}));
}

TEST(CodiconLabelsTest, ABackslashThatDoesNotEscapeAMarkerStays) {
    EXPECT_EQ(split_label_with_icons("a\\b"), (Segments{text("a\\b")}));
    EXPECT_EQ(split_label_with_icons("\\$ (a)"), (Segments{text("\\$ (a)")}));
    EXPECT_EQ(split_label_with_icons("\\"), (Segments{text("\\")}));
}

TEST(CodiconLabelsTest, OnlyTheBackslashRightBeforeAMarkerEscapesIt) {
    EXPECT_EQ(split_label_with_icons("\\\\$(sync)"), (Segments{text("\\$(sync)")}));
}

TEST(CodiconLabelsTest, NonAsciiTextIsKept) {
    EXPECT_EQ(split_label_with_icons("héllo $(sync) 日本語 😀"),
              (Segments{text("héllo "), icon("sync"), text(" 日本語 😀")}));
}

TEST(CodiconLabelsTest, AMarkerForAnIconTheFontLacksStillGivesAnIconSegment) {
    const auto segments = split_label_with_icons("$(no-such-icon)");
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_TRUE(segments[0].is_icon());
    EXPECT_EQ(codicon(segments[0].icon), std::nullopt);
}
