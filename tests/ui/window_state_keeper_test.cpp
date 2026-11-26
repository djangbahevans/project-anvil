#include "anvil/platform/file.hpp"
#include "anvil/ui/panel.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/window_state_keeper.hpp"
#include "anvil/ui/workbench.hpp"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QSplitterHandle>
#include <QTest>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <unistd.h>

using anvil::core::RecordingLogger;
using anvil::ui::fit_to_screens;
using anvil::ui::Panel;
using anvil::ui::PanelPosition;
using anvil::ui::Part;
using anvil::ui::Sidebar;
using anvil::ui::ViewContainerLocation;
using anvil::ui::ViewContainerRegistry;
using anvil::ui::WindowStateKeeper;
using anvil::ui::Workbench;
using anvil::ui::WorkbenchColors;
using anvil::ui::Zoom;
using anvil::workbench::StateScope;
using anvil::workbench::StateStore;

namespace {

QIcon blank_icon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::black);
    return QIcon(pixmap);
}

// A window as the main window builds it, with two containers for the sidebar and two for the
// panel, and the keeper of its state.
struct Window {
    Window(const std::filesystem::path& directory, const std::optional<std::string>& workspace,
           int zoom_level = 0)
        : logger(std::make_shared<RecordingLogger>()), store(directory, workspace, logger) {
        zoom.set_level(zoom_level);
        workbench = new Workbench(colors);
        workbench->set_zoom(&zoom);
        workbench->set_editor_area(new QLabel("editor"));
        window.setCentralWidget(workbench);
        window.resize(1000, 700);
        sidebar = std::make_unique<Sidebar>(*workbench, registry, colors);
        panel = std::make_unique<Panel>(*workbench, registry, colors);
        for (const char* id : {"files", "search"}) {
            add(id, ViewContainerLocation::sidebar);
        }
        for (const char* id : {"terminal", "output"}) {
            add(id, ViewContainerLocation::panel);
        }
        keeper = std::make_unique<WindowStateKeeper>(store, window, *workbench, *sidebar, *panel);
    }

    void add(const std::string& id, ViewContainerLocation location) {
        registry.add({.id = id,
                      .title = QString::fromStdString(id),
                      .icon = blank_icon(),
                      .location = location,
                      .create = [this, id] {
                          ++made[id];
                          return new QLabel(QString::fromStdString(id));
                      }});
    }

    // Restores the state and shows the window, in the order of the main window.
    void open() {
        keeper->restore();
        show();
    }
    void show() {
        window.show();
        QApplication::processEvents();
    }

    QSplitterHandle* sash_beside_sidebar() const {
        for (QSplitterHandle* handle : workbench->findChildren<QSplitterHandle*>()) {
            if (handle->isVisible() && handle->orientation() == Qt::Horizontal) {
                return handle;
            }
        }
        return nullptr;
    }

    std::shared_ptr<RecordingLogger> logger;
    StateStore store;
    Zoom zoom;
    WorkbenchColors colors{QApplication::palette()};
    ViewContainerRegistry registry;
    std::map<std::string, int> made;
    QMainWindow window;
    Workbench* workbench = nullptr;
    std::unique_ptr<Sidebar> sidebar;
    std::unique_ptr<Panel> panel;
    std::unique_ptr<WindowStateKeeper> keeper;
};

class WindowStateKeeperTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("anvil-keeper-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    std::unique_ptr<Window> make(const std::optional<std::string>& workspace = "1111111111111111",
                                 int zoom_level = 0) {
        return std::make_unique<Window>(directory_, workspace, zoom_level);
    }
    std::unique_ptr<Window> open(const std::optional<std::string>& workspace = "1111111111111111",
                                 int zoom_level = 0) {
        auto window = make(workspace, zoom_level);
        window->open();
        return window;
    }

    void write_state(const std::string& file, const std::string& text) {
        std::filesystem::create_directories((directory_ / file).parent_path());
        ASSERT_TRUE(anvil::platform::write_file(directory_ / file, text).has_value());
    }
    nlohmann::json read_state(const std::string& file) const {
        const auto text = anvil::platform::read_file(directory_ / file);
        return text ? nlohmann::json::parse(*text) : nlohmann::json::object();
    }
    nlohmann::json global() const {
        return read_state("global.json");
    }
    nlohmann::json workspace_state(const std::string& id = "1111111111111111") const {
        return read_state("workspaces/" + id + ".json");
    }

    std::filesystem::path directory_;
};

} // namespace

TEST_F(WindowStateKeeperTest, WithNoStateAndNoFolderTheWindowKeepsItsDefaults) {
    const auto window = open(std::nullopt);
    EXPECT_FALSE(window->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::bottom);
    EXPECT_EQ(window->window.size(), QSize(1000, 700));
    EXPECT_TRUE(window->made.empty());
}

TEST_F(WindowStateKeeperTest, WithNoStateAFolderShowsTheSidebarAndNotThePanel) {
    const auto window = open();
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_EQ(window->sidebar->active_container(), "files");
    EXPECT_EQ(window->made, (std::map<std::string, int>{{"files", 1}}));
}

TEST_F(WindowStateKeeperTest, TheSidebarThatShowsByDefaultHasTheSavedContainer) {
    write_state("workspaces/1111111111111111.json",
                R"({"layout.sidebar.activeContainer": "search"})");
    const auto window = open();
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_EQ(window->made, (std::map<std::string, int>{{"search", 1}}));
}

TEST_F(WindowStateKeeperTest, ASavedVisibilityIsUsedInPlaceOfTheDefault) {
    write_state("workspaces/1111111111111111.json", R"({"layout.sidebar.visible": false})");
    write_state("no-folder.json", R"({"layout.sidebar.visible": true})");
    const auto folder = open();
    EXPECT_FALSE(folder->workbench->is_visible(Part::sidebar));
    EXPECT_TRUE(folder->made.empty());
    const auto none = open(std::nullopt);
    EXPECT_TRUE(none->workbench->is_visible(Part::sidebar));
}

TEST_F(WindowStateKeeperTest, TheLayoutOfTheLastWindowIsRestored) {
    int sidebar_width = 0;
    int panel_width = 0;
    {
        const auto window = open();
        window->sidebar->activate("search");
        window->workbench->set_size(Part::sidebar, 260);
        window->panel->activate("output");
        window->workbench->set_panel_size_at(PanelPosition::top, 150);
        window->workbench->set_panel_size_at(PanelPosition::left, 320);
        window->workbench->set_panel_position(PanelPosition::right);
        window->workbench->set_size(Part::panel, 340);
        QApplication::processEvents();
        sidebar_width = window->workbench->sidebar()->width();
        panel_width = window->workbench->panel()->width();
        EXPECT_EQ(sidebar_width, 260);
        window->keeper->save();
    }
    const auto window = make();
    window->keeper->restore();
    // Before the window is shown.
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_TRUE(window->workbench->is_visible(Part::panel));
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::right);
    EXPECT_EQ(window->sidebar->active_container(), "search");
    EXPECT_EQ(window->panel->active_container(), "output");

    window->show();
    EXPECT_EQ(window->workbench->sidebar()->width(), sidebar_width);
    EXPECT_EQ(window->workbench->panel()->width(), panel_width);
    EXPECT_EQ(window->workbench->panel_size_at(PanelPosition::top), 150);
    EXPECT_EQ(window->workbench->panel_size_at(PanelPosition::left), 320);
    EXPECT_EQ(window->workbench->panel_size_at(PanelPosition::bottom), Workbench::default_size);
    EXPECT_EQ(window->made, (std::map<std::string, int>{{"output", 1}, {"search", 1}}));
    EXPECT_EQ(window->sidebar->title(), "SEARCH");
}

TEST_F(WindowStateKeeperTest, AHiddenPartKeepsItsContainerAndSizeWithoutMakingAView) {
    {
        const auto window = open();
        window->sidebar->activate("search");
        window->workbench->set_size(Part::sidebar, 270);
        window->panel->activate("output");
        window->workbench->set_size(Part::panel, 190);
        QApplication::processEvents();
        window->workbench->set_visible(Part::sidebar, false);
        window->workbench->set_visible(Part::panel, false);
        window->keeper->save();
    }
    EXPECT_EQ(global().at("layout.sidebar.width"), 270);
    EXPECT_EQ(global().at("layout.panel.size.bottom"), 190);

    const auto window = open();
    EXPECT_FALSE(window->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_TRUE(window->made.empty());

    window->workbench->set_visible(Part::sidebar, true);
    window->workbench->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_EQ(window->made, (std::map<std::string, int>{{"output", 1}, {"search", 1}}));
    ASSERT_NE(window->sidebar->container_widget("search"), nullptr);
    ASSERT_NE(window->panel->container_widget("output"), nullptr);
    EXPECT_TRUE(window->sidebar->container_widget("search")->isVisible());
    EXPECT_TRUE(window->panel->container_widget("output")->isVisible());
    EXPECT_EQ(window->workbench->sidebar()->width(), 270);
    EXPECT_EQ(window->workbench->panel()->height(), 190);
}

TEST_F(WindowStateKeeperTest, AContainerThatIsNotRegisteredForThePartIsIgnored) {
    write_state("workspaces/1111111111111111.json",
                R"({"layout.sidebar.visible": true, "layout.sidebar.activeContainer": "terminal",)"
                R"( "layout.panel.visible": true, "layout.panel.activeContainer": "gone"})");
    const auto window = open();
    EXPECT_EQ(window->sidebar->active_container(), "files");
    EXPECT_EQ(window->panel->active_container(), "terminal");
}

TEST_F(WindowStateKeeperTest, AMaximizedPanelSavesTheSizeItIsRestoredTo) {
    {
        const auto window = open();
        window->panel->activate("terminal");
        window->workbench->set_size(Part::panel, 200);
        QApplication::processEvents();
        window->workbench->set_maximized(Part::panel, true);
        QApplication::processEvents();
        EXPECT_GT(window->workbench->panel()->height(), 600);
        window->keeper->save();
    }
    EXPECT_EQ(global().at("layout.panel.size.bottom"), 200);

    const auto window = open();
    EXPECT_TRUE(window->workbench->is_maximized(Part::panel));
    EXPECT_GT(window->workbench->panel()->height(), 600);
    window->workbench->set_maximized(Part::panel, false);
    QApplication::processEvents();
    EXPECT_EQ(window->workbench->panel()->height(), 200);
}

TEST_F(WindowStateKeeperTest, AHiddenPanelIsNotRestoredAsMaximized) {
    write_state("workspaces/1111111111111111.json",
                R"({"layout.panel.visible": false, "layout.panel.maximized": true})");
    const auto window = open();
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_FALSE(window->workbench->is_maximized(Part::panel));
}

TEST_F(WindowStateKeeperTest, SizesBelowTheMinimumAreRaisedToIt) {
    write_state("global.json", R"({"layout.sidebar.width": 3, "layout.panel.size.bottom": 2,)"
                               R"( "layout.panel.size.left": 5})");
    write_state("workspaces/1111111111111111.json",
                R"({"layout.sidebar.visible": true, "layout.panel.visible": true})");
    const auto window = open();
    EXPECT_EQ(window->workbench->sidebar()->width(), Workbench::sidebar_minimum_width);
    EXPECT_EQ(window->workbench->panel()->height(), Workbench::panel_minimum_height);
    EXPECT_EQ(window->workbench->panel_size_at(PanelPosition::left),
              Workbench::panel_minimum_width);
}

TEST_F(WindowStateKeeperTest, ValuesThatMakeNoSenseGiveTheDefaults) {
    write_state("global.json",
                R"({"layout.sidebar.width": -40, "layout.panel.size.bottom": "tall",)"
                R"( "window.x": 10, "window.y": 10, "window.width": 99999999999,)"
                R"( "window.height": 500, "window.maximized": "yes"})");
    write_state("workspaces/1111111111111111.json",
                R"({"layout.sidebar.visible": 1, "layout.panel.position": "middle",)"
                R"( "layout.panel.visible": true, "layout.sidebar.activeContainer": 7})");
    const auto window = open();
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_EQ(window->workbench->size(Part::sidebar), Workbench::default_size);
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::bottom);
    EXPECT_EQ(window->workbench->panel()->height(), Workbench::default_size);
    EXPECT_EQ(window->window.size(), QSize(1000, 700));
    EXPECT_FALSE(window->window.isMaximized());
}

TEST_F(WindowStateKeeperTest, ASavedSizeIsRestoredAsSavedAtAnyZoomLevel) {
    write_state("global.json", R"({"layout.sidebar.width": 300, "layout.panel.size.bottom": 150})");
    write_state("workspaces/1111111111111111.json",
                R"({"layout.sidebar.visible": true, "layout.panel.visible": true})");
    const auto window = open("1111111111111111", 2);
    EXPECT_EQ(window->workbench->sidebar()->width(), 300);
    EXPECT_EQ(window->workbench->panel()->height(), 150);
}

TEST_F(WindowStateKeeperTest, ChangesAreWrittenAfterADelayAndNotAtOnce) {
    const auto window = open();
    window->sidebar->activate("files");
    EXPECT_FALSE(std::filesystem::exists(directory_ / "global.json"));
    const auto written = [&](const std::string& key, const nlohmann::json& value, bool in_global) {
        return QTest::qWaitFor(
            [&] {
                const nlohmann::json state = in_global ? global() : workspace_state();
                return state.contains(key) && state.at(key) == value;
            },
            5000);
    };
    EXPECT_TRUE(written("layout.sidebar.visible", true, false));

    QSplitterHandle* sash = window->sash_beside_sidebar();
    ASSERT_NE(sash, nullptr);
    const int before = window->workbench->sidebar()->width();
    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(40, 0));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(40, 0));
    QApplication::processEvents();
    const int dragged = window->workbench->sidebar()->width();
    ASSERT_NE(dragged, before);
    EXPECT_TRUE(written("layout.sidebar.width", dragged, true));

    window->window.resize(900, 650);
    EXPECT_TRUE(written("window.width", 900, true));
    window->window.setGeometry(40, 30, 900, 650);
    EXPECT_TRUE(written("window.x", 40, true));
    window->sidebar->activate("search");
    EXPECT_TRUE(written("layout.sidebar.activeContainer", "search", false));
    window->panel->activate("output");
    EXPECT_TRUE(written("layout.panel.activeContainer", "output", false));
    window->workbench->set_maximized(Part::panel, true);
    EXPECT_TRUE(written("layout.panel.maximized", true, false));
    window->workbench->set_panel_position(PanelPosition::left);
    EXPECT_TRUE(written("layout.panel.position", "left", false));
}

TEST_F(WindowStateKeeperTest, ADraggedSizeIsKeptByAPartThatIsHiddenAfterwards) {
    const auto window = open();
    window->sidebar->activate("files");
    QApplication::processEvents();
    QSplitterHandle* sash = window->sash_beside_sidebar();
    ASSERT_NE(sash, nullptr);
    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(40, 0));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(40, 0));
    QApplication::processEvents();
    const int dragged = window->workbench->sidebar()->width();
    window->window.hide();
    EXPECT_EQ(window->workbench->size(Part::sidebar), dragged);
}

TEST_F(WindowStateKeeperTest, ADragOfThePanelsSashIsWrittenAndKept) {
    const auto window = open();
    window->panel->activate("terminal");
    QApplication::processEvents();
    window->keeper->save();
    QSplitterHandle* sash = nullptr;
    for (QSplitterHandle* handle : window->workbench->findChildren<QSplitterHandle*>()) {
        if (handle->isVisible() && handle->orientation() == Qt::Vertical) {
            sash = handle;
        }
    }
    ASSERT_NE(sash, nullptr);
    const int before = window->workbench->panel()->height();
    const QPoint start = sash->rect().center();
    QTest::mousePress(sash, Qt::LeftButton, {}, start);
    QTest::mouseMove(sash, start + QPoint(0, -50));
    QTest::mouseRelease(sash, Qt::LeftButton, {}, start + QPoint(0, -50));
    QApplication::processEvents();
    const int dragged = window->workbench->panel()->height();
    ASSERT_NE(dragged, before);
    EXPECT_TRUE(
        QTest::qWaitFor([&] { return global().at("layout.panel.size.bottom") == dragged; }, 5000));
    window->window.hide();
    EXPECT_EQ(window->workbench->size(Part::panel), dragged);
}

TEST_F(WindowStateKeeperTest, TheModeThatHidesThePartsIsNotWhatIsSaved) {
    const auto window = open();
    bool in_mode = true;
    window->keeper->set_parts_outside_mode([&]() -> std::optional<WindowStateKeeper::PartsShown> {
        if (!in_mode) {
            return std::nullopt;
        }
        return WindowStateKeeper::PartsShown{.sidebar = true, .panel = false};
    });
    window->workbench->set_visible(Part::sidebar, false);
    window->panel->activate("terminal");
    window->keeper->save();
    EXPECT_EQ(workspace_state().at("layout.sidebar.visible"), true);
    EXPECT_EQ(workspace_state().at("layout.panel.visible"), false);
    in_mode = false;
    window->keeper->save();
    EXPECT_EQ(workspace_state().at("layout.sidebar.visible"), false);
    EXPECT_EQ(workspace_state().at("layout.panel.visible"), true);
}

TEST_F(WindowStateKeeperTest, EachWorkspaceHasItsLayoutAndTheSizesAreShared) {
    {
        const auto first = open("1111111111111111");
        first->sidebar->activate("search");
        first->workbench->set_size(Part::sidebar, 260);
        first->keeper->save();
        const auto none = open(std::nullopt);
        none->panel->activate("output");
        none->keeper->save();
    }
    const auto other = open("2222222222222222");
    EXPECT_TRUE(other->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(other->workbench->is_visible(Part::panel));
    EXPECT_EQ(other->workbench->size(Part::sidebar), 260);

    const auto none = open(std::nullopt);
    EXPECT_FALSE(none->workbench->is_visible(Part::sidebar));
    EXPECT_TRUE(none->workbench->is_visible(Part::panel));
    const auto first = open("1111111111111111");
    EXPECT_TRUE(first->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(first->workbench->is_visible(Part::panel));
}

TEST_F(WindowStateKeeperTest, ChangingTheWorkspaceSavesTheOneLeftAndAppliesTheLayoutOfTheNewOne) {
    write_state("workspaces/2222222222222222.json",
                R"({"layout.sidebar.visible": false, "layout.sidebar.activeContainer": "search",)"
                R"( "layout.panel.visible": true, "layout.panel.activeContainer": "output",)"
                R"( "layout.panel.position": "left"})");
    const auto window = open("1111111111111111");
    window->keeper->set_workspace("2222222222222222");
    EXPECT_EQ(workspace_state("1111111111111111").at("layout.sidebar.visible"), true);
    EXPECT_EQ(workspace_state("1111111111111111").at("layout.panel.visible"), false);
    EXPECT_FALSE(window->workbench->is_visible(Part::sidebar));
    EXPECT_TRUE(window->workbench->is_visible(Part::panel));
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::left);
    EXPECT_EQ(window->sidebar->active_container(), "search");
    EXPECT_EQ(window->panel->active_container(), "output");
    // No view is made for the container of the sidebar, which does not show.
    EXPECT_EQ(window->made, (std::map<std::string, int>{{"files", 1}, {"output", 1}}));

    window->workbench->set_visible(Part::sidebar, true);
    window->keeper->save();
    EXPECT_EQ(workspace_state("2222222222222222").at("layout.sidebar.visible"), true);
    EXPECT_EQ(workspace_state("1111111111111111").at("layout.sidebar.activeContainer"), "files");

    window->keeper->set_workspace("1111111111111111");
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::bottom);
    EXPECT_EQ(window->sidebar->active_container(), "files");
}

TEST_F(WindowStateKeeperTest, AWorkspaceThatHasNothingSavedGetsTheDefaultsAndNotWhatShowed) {
    const auto window = open("1111111111111111");
    window->sidebar->activate("search");
    window->workbench->set_visible(Part::sidebar, false);
    window->panel->activate("output");
    window->workbench->set_panel_position(PanelPosition::right);
    window->workbench->set_maximized(Part::panel, true);

    window->keeper->set_workspace("3333333333333333");
    EXPECT_TRUE(window->workbench->is_visible(Part::sidebar));
    EXPECT_EQ(window->sidebar->active_container(), "files");
    EXPECT_EQ(window->sidebar->title(), "FILES");
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
    EXPECT_FALSE(window->workbench->is_maximized(Part::panel));
    EXPECT_EQ(window->workbench->panel_position(), PanelPosition::bottom);
    window->workbench->set_visible(Part::panel, true);
    EXPECT_EQ(window->panel->active_container(), "terminal");

    window->keeper->set_workspace(std::nullopt);
    EXPECT_FALSE(window->workbench->is_visible(Part::sidebar));
    EXPECT_FALSE(window->workbench->is_visible(Part::panel));
}

TEST_F(WindowStateKeeperTest, AMaximizedPanelIsRestoredWhenTheWorkspaceChanges) {
    write_state("workspaces/2222222222222222.json",
                R"({"layout.panel.visible": true, "layout.panel.maximized": true})");
    const auto window = open("1111111111111111");
    window->keeper->set_workspace("2222222222222222");
    EXPECT_TRUE(window->workbench->is_maximized(Part::panel));
}

TEST_F(WindowStateKeeperTest, TheSizesStayWhenTheWorkspaceChanges) {
    const auto window = open("1111111111111111");
    window->workbench->set_size(Part::sidebar, 260);
    window->workbench->set_visible(Part::panel, true);
    window->workbench->set_size(Part::panel, 180);
    QApplication::processEvents();
    window->keeper->set_workspace("2222222222222222");
    window->workbench->set_visible(Part::panel, true);
    QApplication::processEvents();
    EXPECT_EQ(window->workbench->sidebar()->width(), 260);
    EXPECT_EQ(window->workbench->panel()->height(), 180);
}

TEST_F(WindowStateKeeperTest, TheContentIsRestoredFromTheNewWorkspaceBeforeItsLayoutShows) {
    write_state("workspaces/2222222222222222.json",
                R"({"layout.sidebar.visible": false, "content": "second"})");
    const auto window = open("1111111111111111");
    std::vector<std::pair<std::optional<std::string>, bool>> seen;
    window->keeper->set_content_restore([&] {
        seen.emplace_back(window->store.string(StateScope::workspace, "content"),
                          window->workbench->is_visible(Part::sidebar));
    });
    window->keeper->set_workspace("2222222222222222");
    EXPECT_EQ(seen, (std::vector<std::pair<std::optional<std::string>, bool>>{{"second", true}}));
}

TEST_F(WindowStateKeeperTest, TheSizeAndPlaceOfTheWindowAreRestored) {
    {
        const auto window = open();
        window->window.setGeometry(60, 50, 640, 480);
        QApplication::processEvents();
        window->keeper->save();
    }
    const auto window = make();
    window->keeper->restore();
    EXPECT_EQ(window->window.geometry(), QRect(60, 50, 640, 480));
    EXPECT_FALSE(window->window.isMaximized());
}

TEST_F(WindowStateKeeperTest, AMaximizedWindowIsRestoredWithThePlaceItHasWhenItIsNot) {
    {
        const auto window = open();
        window->window.setGeometry(60, 50, 640, 480);
        QApplication::processEvents();
        window->window.showMaximized();
        QApplication::processEvents();
        window->keeper->save();
    }
    EXPECT_EQ(global().at("window.maximized"), true);
    EXPECT_EQ(global().at("window.width"), 640);

    const auto window = open();
    EXPECT_TRUE(window->window.isMaximized());
    // The frame of the offscreen platform takes a few pixels of the screen.
    const QSize screen = QApplication::primaryScreen()->availableGeometry().size();
    EXPECT_GE(window->window.width(), screen.width() - 8);
    EXPECT_GE(window->window.height(), screen.height() - 8);
    window->window.showNormal();
    QApplication::processEvents();
    EXPECT_EQ(window->window.geometry(), QRect(60, 50, 640, 480));
}

TEST_F(WindowStateKeeperTest, APanelOfAMaximizedWindowKeepsASizeThatTheNormalWindowHasNoRoomFor) {
    write_state("global.json",
                R"({"window.x": 20, "window.y": 20, "window.width": 500, "window.height": 300,)"
                R"( "window.maximized": true, "layout.panel.size.bottom": 450})");
    write_state("workspaces/1111111111111111.json", R"({"layout.panel.visible": true})");
    const auto window = open();
    ASSERT_GT(window->window.height(), 550);
    EXPECT_EQ(window->workbench->panel()->height(), 450);
}

TEST_F(WindowStateKeeperTest, FullScreenIsNotRestoredAndKeepsThePlaceFromBeforeIt) {
    {
        const auto window = open();
        window->window.setGeometry(60, 50, 640, 480);
        QApplication::processEvents();
        window->window.showFullScreen();
        QApplication::processEvents();
        window->keeper->save();
    }
    const auto window = open();
    EXPECT_FALSE(window->window.isFullScreen());
    EXPECT_FALSE(window->window.isMaximized());
    EXPECT_EQ(window->window.geometry(), QRect(60, 50, 640, 480));
}

TEST_F(WindowStateKeeperTest, AWindowThatNoScreenShowsIsMovedOntoOne) {
    write_state("global.json", R"({"window.x": 5000, "window.y": 4000, "window.width": 600,)"
                               R"( "window.height": 400})");
    const auto window = open();
    const QRect screen = QApplication::primaryScreen()->availableGeometry();
    EXPECT_TRUE(screen.contains(window->window.geometry()))
        << window->window.x() << "," << window->window.y();
    EXPECT_EQ(window->window.size(), QSize(600, 400));
}

TEST(FitToScreensTest, AWindowWhoseTopIsOnAScreenStays) {
    const std::vector<QRect> screens{{0, 0, 1920, 1080}, {1920, 0, 1280, 1024}};
    for (const QRect saved :
         {QRect(0, 0, 800, 600), QRect(1800, 100, 800, 600), QRect(-700, 20, 800, 600),
          QRect(2000, 900, 3000, 600), QRect(100, -20, 800, 600), QRect(300, 1060, 800, 600)}) {
        EXPECT_EQ(fit_to_screens(saved, screens), saved) << saved.x() << "," << saved.y();
    }
}

TEST(FitToScreensTest, AWindowWhoseTopCannotBeReachedIsCenteredOnAScreen) {
    const std::vector<QRect> screens{{0, 0, 1920, 1080}, {1920, 0, 1280, 1024}};
    // Far away: the primary screen.
    EXPECT_EQ(fit_to_screens({9000, 9000, 800, 600}, screens), QRect(560, 240, 800, 600));
    // Above the second screen, with the title out of reach: the screen that shows most of it.
    EXPECT_EQ(fit_to_screens({2000, -300, 800, 600}, screens), QRect(2160, 212, 800, 600));
    // Only a sliver of the side shows.
    EXPECT_EQ(fit_to_screens({-750, 100, 800, 600}, screens), QRect(560, 240, 800, 600));
    // Larger than the screen it moves to.
    EXPECT_EQ(fit_to_screens({9000, 9000, 4000, 3000}, screens), QRect(0, 0, 1920, 1080));
    EXPECT_EQ(fit_to_screens({9000, 9000, 40, 30}, {}), QRect(9000, 9000, 40, 30));
}
