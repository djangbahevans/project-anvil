#include "anvil/ui/view_container_registry.hpp"

#include <QSignalSpy>
#include <gtest/gtest.h>

using anvil::ui::ViewContainer;
using anvil::ui::ViewContainerLocation;
using anvil::ui::ViewContainerRegistry;

namespace {

ViewContainer container(std::string id, int order = 0,
                        ViewContainerLocation location = ViewContainerLocation::sidebar) {
    ViewContainer result;
    result.id = std::move(id);
    result.title = QString::fromStdString(result.id);
    result.order = order;
    result.location = location;
    return result;
}

std::vector<std::string> ids(const std::vector<const ViewContainer*>& containers) {
    std::vector<std::string> result;
    for (const ViewContainer* c : containers) {
        result.push_back(c->id);
    }
    return result;
}

} // namespace

TEST(ViewContainerRegistryTest, FindsAContainerById) {
    ViewContainerRegistry registry;
    registry.add(container("explorer"));
    ASSERT_NE(registry.find("explorer"), nullptr);
    EXPECT_EQ(registry.find("explorer")->title, "explorer");
    EXPECT_EQ(registry.find("search"), nullptr);
}

TEST(ViewContainerRegistryTest, ListsTheContainersOfALocationByOrderThenRegistration) {
    ViewContainerRegistry registry;
    registry.add(container("c", 2));
    registry.add(container("a", 1));
    registry.add(container("b", 1));
    registry.add(container("terminal", 0, ViewContainerLocation::panel));

    EXPECT_EQ(ids(registry.containers(ViewContainerLocation::sidebar)),
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(ids(registry.containers(ViewContainerLocation::panel)),
              (std::vector<std::string>{"terminal"}));
}

TEST(ViewContainerRegistryTest, AddingAnExistingIdReplacesTheContainer) {
    ViewContainerRegistry registry;
    registry.add(container("a", 1));
    registry.add(container("b", 2));
    ViewContainer replacement = container("a", 3);
    replacement.title = "Renamed";
    registry.add(std::move(replacement));

    EXPECT_EQ(registry.find("a")->title, "Renamed");
    EXPECT_EQ(ids(registry.containers(ViewContainerLocation::sidebar)),
              (std::vector<std::string>{"b", "a"}));
}

TEST(ViewContainerRegistryTest, RemovingAContainer) {
    ViewContainerRegistry registry;
    registry.add(container("a"));
    EXPECT_TRUE(registry.remove("a"));
    EXPECT_FALSE(registry.remove("a"));
    EXPECT_EQ(registry.find("a"), nullptr);
}

TEST(ViewContainerRegistryTest, ChangedIsEmittedForEveryChange) {
    ViewContainerRegistry registry;
    QSignalSpy changed(&registry, &ViewContainerRegistry::changed);
    registry.add(container("a"));
    EXPECT_EQ(changed.count(), 1);
    registry.add(container("a"));
    EXPECT_EQ(changed.count(), 2);
    registry.remove("a");
    EXPECT_EQ(changed.count(), 3);
    registry.remove("a");
    EXPECT_EQ(changed.count(), 3);
}

TEST(ViewContainerRegistryTest, RegisteringDoesNotCreateTheWidget) {
    ViewContainerRegistry registry;
    int created = 0;
    ViewContainer lazy = container("lazy");
    lazy.create = [&] {
        ++created;
        return new QWidget;
    };
    registry.add(std::move(lazy));
    registry.containers(ViewContainerLocation::sidebar);
    EXPECT_EQ(created, 0);
}
