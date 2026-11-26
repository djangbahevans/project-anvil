#include "anvil/ui/workbench_colors.hpp"

#include <QApplication>
#include <QSignalSpy>
#include <algorithm>
#include <gtest/gtest.h>

using anvil::ui::WorkbenchColorKey;
using anvil::ui::WorkbenchColors;

namespace {

// A palette whose roles are all different, so that a fallback shows which role it came from.
QPalette distinct_palette() {
    QPalette palette;
    for (int role = 0; role < QPalette::NColorRoles; ++role) {
        palette.setColor(static_cast<QPalette::ColorRole>(role),
                         QColor(10 + 9 * role, 220 - 7 * role, 30 + 5 * role));
    }
    return palette;
}

} // namespace

TEST(WorkbenchColorsTest, TheKnownKeysAreOrderedUniqueAndDescribed) {
    const auto keys = WorkbenchColors::known_keys();
    ASSERT_FALSE(keys.empty());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_FALSE(keys[i].description.empty()) << keys[i].key;
        if (i > 0) {
            EXPECT_LT(keys[i - 1].key, keys[i].key);
        }
    }
}

TEST(WorkbenchColorsTest, FallbackKeysAreKnownKeysThatEndInAPaletteColor) {
    const auto keys = WorkbenchColors::known_keys();
    const auto is_known = [&](std::string_view key) {
        return std::ranges::any_of(keys, [&](const WorkbenchColorKey& k) { return k.key == key; });
    };
    for (const WorkbenchColorKey& entry : keys) {
        std::string_view key = entry.key;
        int hops = 0;
        for (const WorkbenchColorKey* current = &entry; !current->fallback_key.empty(); ++hops) {
            ASSERT_TRUE(is_known(current->fallback_key)) << key << " -> " << current->fallback_key;
            current = &*std::ranges::find(keys, current->fallback_key, &WorkbenchColorKey::key);
            ASSERT_LT(hops, 4) << key << " does not end";
        }
    }
}

TEST(WorkbenchColorsTest, EveryKeyFallsBackToItsPaletteRoleWithNoTheme) {
    const QPalette palette = distinct_palette();
    const WorkbenchColors colors(palette);
    for (const WorkbenchColorKey& entry : WorkbenchColors::known_keys()) {
        const QColor shown = colors.color(entry.key);
        ASSERT_TRUE(shown.isValid()) << entry.key;
        EXPECT_FALSE(colors.is_themed(entry.key)) << entry.key;
        if (!entry.fallback_key.empty()) {
            EXPECT_EQ(shown, colors.color(entry.fallback_key)) << entry.key;
        } else if (entry.role == QPalette::NoRole) {
            EXPECT_EQ(shown, QColor::fromRgba(entry.fixed)) << entry.key;
        } else {
            EXPECT_EQ(shown, palette.color(entry.role)) << entry.key;
        }
    }
}

TEST(WorkbenchColorsTest, TheStatusBarAndTheEditorTakeTheirColorsFromThePalette) {
    const QPalette palette = distinct_palette();
    const WorkbenchColors colors(palette);
    EXPECT_EQ(colors.color("statusBar.background"), palette.color(QPalette::Window));
    EXPECT_EQ(colors.color("statusBar.foreground"), palette.color(QPalette::WindowText));
    EXPECT_EQ(colors.color("editor.background"), palette.color(QPalette::Base));
    EXPECT_EQ(colors.color("editor.foreground"), palette.color(QPalette::Text));
    EXPECT_EQ(colors.color("editor.selectionBackground"), palette.color(QPalette::Highlight));
}

TEST(WorkbenchColorsTest, AnUnknownKeyGivesAVisibleColorNotAnInvalidOne) {
    const WorkbenchColors colors(QApplication::palette());
    const QColor unknown = colors.color("no.such.key");
    EXPECT_TRUE(unknown.isValid());
    EXPECT_EQ(unknown, QColor(255, 0, 255));
    EXPECT_FALSE(colors.is_themed("no.such.key"));
    EXPECT_EQ(colors.color(""), unknown);
}

TEST(WorkbenchColorsTest, TheThemeReplacesTheFallbackAndKeepsItsAlpha) {
    WorkbenchColors colors(distinct_palette());
    colors.set_colors({{"statusBar.background", QColor("#11223344")}});
    EXPECT_EQ(colors.color("statusBar.background"), QColor("#11223344"));
    EXPECT_EQ(colors.color("statusBar.background").alpha(), 0x11);
    EXPECT_TRUE(colors.is_themed("statusBar.background"));
    EXPECT_FALSE(colors.is_themed("statusBar.foreground"));
}

TEST(WorkbenchColorsTest, SettingColorsReplacesTheWholeTable) {
    const QPalette palette = distinct_palette();
    WorkbenchColors colors(palette);
    colors.set_colors({{"statusBar.background", QColor("#ff0000")}});
    colors.set_colors({{"tab.border", QColor("#0000ff")}});
    EXPECT_EQ(colors.color("tab.border"), QColor("#0000ff"));
    EXPECT_EQ(colors.color("statusBar.background"), palette.color(QPalette::Window));
    EXPECT_FALSE(colors.is_themed("statusBar.background"));

    colors.set_colors({});
    EXPECT_FALSE(colors.is_themed("tab.border"));
}

TEST(WorkbenchColorsTest, AKeyTakesTheThemesValueOfItsFallbackKey) {
    WorkbenchColors colors(distinct_palette());
    colors.set_colors({{"focusBorder", QColor("#123456")}});
    EXPECT_EQ(colors.color("sash.hoverBorder"), QColor("#123456"));
    EXPECT_FALSE(colors.is_themed("sash.hoverBorder"));

    colors.set_colors(
        {{"focusBorder", QColor("#123456")}, {"sash.hoverBorder", QColor("#abcdef")}});
    EXPECT_EQ(colors.color("sash.hoverBorder"), QColor("#abcdef"));
}

TEST(WorkbenchColorsTest, ChangedIsEmittedOnceForAReplacementAndNotWhenNothingDiffers) {
    WorkbenchColors colors(QApplication::palette());
    QSignalSpy changed(&colors, &WorkbenchColors::changed);

    WorkbenchColors::Colors everything;
    for (const WorkbenchColorKey& entry : WorkbenchColors::known_keys()) {
        everything[std::string(entry.key)] = QColor("#336699");
    }
    colors.set_colors(everything);
    EXPECT_EQ(changed.count(), 1);

    colors.set_colors(everything);
    EXPECT_EQ(changed.count(), 1);

    everything["statusBar.background"] = QColor("#996633");
    colors.set_colors(everything);
    EXPECT_EQ(changed.count(), 2);

    // Keys no part asks for are not kept, so a theme that only differs there changes nothing.
    everything["some.other.key"] = QColor("#000000");
    colors.set_colors(everything);
    EXPECT_EQ(changed.count(), 2);

    colors.set_colors({{"invalid.color", QColor()}});
    EXPECT_EQ(changed.count(), 3);
}

TEST(WorkbenchColorsTest, AnInvalidColorInTheThemeIsTreatedAsNotSet) {
    const QPalette palette = distinct_palette();
    WorkbenchColors colors(palette);
    colors.set_colors({{"statusBar.background", QColor()}});
    EXPECT_FALSE(colors.is_themed("statusBar.background"));
    EXPECT_EQ(colors.color("statusBar.background"), palette.color(QPalette::Window));
}

TEST(WorkbenchColorsTest, TheFallbacksFollowThePaletteAndChangedIsEmittedForANewOne) {
    QPalette first = distinct_palette();
    WorkbenchColors colors(first);
    QSignalSpy changed(&colors, &WorkbenchColors::changed);

    colors.set_palette(first);
    EXPECT_EQ(changed.count(), 0);

    QPalette second = first;
    second.setColor(QPalette::Window, QColor("#abcdef"));
    colors.set_palette(second);
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(colors.color("statusBar.background"), QColor("#abcdef"));

    colors.set_colors({{"statusBar.background", QColor("#000001")}});
    colors.set_palette(first);
    EXPECT_EQ(colors.color("statusBar.background"), QColor("#000001"));
}

TEST(WorkbenchColorsTest, TheKindIsThatOfTheThemeAndWithoutOneThatOfThePalette) {
    QPalette palette = distinct_palette();
    palette.setColor(QPalette::Base, QColor("#ffffff"));
    WorkbenchColors colors(palette);
    EXPECT_EQ(colors.kind(), anvil::core::ThemeKind::light);
    palette.setColor(QPalette::Base, QColor("#101010"));
    colors.set_palette(palette);
    EXPECT_EQ(colors.kind(), anvil::core::ThemeKind::dark);

    QSignalSpy changed(&colors, &WorkbenchColors::changed);
    colors.set_theme({}, nullptr, anvil::core::ThemeKind::light);
    EXPECT_EQ(colors.kind(), anvil::core::ThemeKind::light);
    EXPECT_EQ(changed.count(), 1);
    colors.set_theme({}, nullptr, anvil::core::ThemeKind::light);
    EXPECT_EQ(changed.count(), 1);
    // Another kind alone is a change.
    colors.set_theme({}, nullptr, anvil::core::ThemeKind::high_contrast_light);
    EXPECT_EQ(colors.kind(), anvil::core::ThemeKind::high_contrast_light);
    EXPECT_EQ(changed.count(), 2);
}
