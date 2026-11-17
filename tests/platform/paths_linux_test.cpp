#include "anvil/platform/paths.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

namespace {

class UserConfigDir : public ::testing::Test {
protected:
    void SetUp() override {
        save("XDG_CONFIG_HOME", saved_xdg_);
        save("HOME", saved_home_);
    }

    void TearDown() override {
        restore("XDG_CONFIG_HOME", saved_xdg_);
        restore("HOME", saved_home_);
    }

private:
    struct Saved {
        bool present = false;
        std::string value;
    };

    static void save(const char* name, Saved& out) {
        if (const char* value = std::getenv(name)) {
            out = {true, value};
        }
    }

    static void restore(const char* name, const Saved& saved) {
        if (saved.present) {
            setenv(name, saved.value.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }

    Saved saved_xdg_;
    Saved saved_home_;
};

} // namespace

TEST_F(UserConfigDir, PrefersXdgConfigHome) {
    setenv("XDG_CONFIG_HOME", "/xdg", 1);
    setenv("HOME", "/home/user", 1);
    EXPECT_EQ(anvil::platform::user_config_dir(), std::filesystem::path{"/xdg/anvil"});
}

TEST_F(UserConfigDir, FallsBackToHomeDotConfig) {
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/user", 1);
    EXPECT_EQ(anvil::platform::user_config_dir(),
              std::filesystem::path{"/home/user/.config/anvil"});
}

TEST_F(UserConfigDir, IgnoresRelativeXdgConfigHome) {
    setenv("XDG_CONFIG_HOME", "relative", 1);
    setenv("HOME", "/home/user", 1);
    EXPECT_EQ(anvil::platform::user_config_dir(),
              std::filesystem::path{"/home/user/.config/anvil"});
}

TEST_F(UserConfigDir, FailsWhenNoBaseDirectoryIsKnown) {
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
    const auto result = anvil::platform::user_config_dir();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, anvil::ErrorCode::environment_unavailable);
}
