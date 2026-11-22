// Benchmarks of the style lookup for the scope stack of a token, over the rules of Dark+.
//   cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON
//   cmake --build --preset linux-clang-release --target anvil_token_theme_bench
//   build/linux-clang-release/bin/anvil_token_theme_bench
#include "anvil/core/token_theme.hpp"

#include <benchmark/benchmark.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace anvil::core;

namespace {

TokenTheme dark_plus() {
    const std::filesystem::path directory =
        std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions" / "theme-defaults" / "themes";
    const auto theme = ColorTheme::load(
        directory / "dark_plus.json", ThemeKind::dark,
        [](const std::filesystem::path& path) -> std::expected<std::string, anvil::Error> {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::unexpected(anvil::Error{anvil::ErrorCode::io_error, "cannot open"});
            }
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        });
    if (!theme) {
        std::fprintf(stderr, "cannot load Dark+: %s\n", theme.error().message.c_str());
        std::exit(1);
    }
    return TokenTheme::from_json(theme->token_colors());
}

// Stacks like the ones a highlighter produces for TypeScript.
const std::vector<std::vector<std::string>>& stacks() {
    static const std::vector<std::vector<std::string>> all = {
        {"source.ts"},
        {"source.ts", "meta.function.ts", "entity.name.function.ts"},
        {"source.ts", "meta.function.ts", "meta.parameters.ts", "variable.parameter.ts"},
        {"source.ts", "meta.block.ts", "meta.var.expr.ts", "keyword.control.flow.ts"},
        {"source.ts", "string.quoted.double.ts", "punctuation.definition.string.begin.ts"},
        {"source.ts", "string.quoted.double.ts", "constant.character.escape.ts"},
        {"source.ts", "comment.line.double-slash.ts", "punctuation.definition.comment.ts"},
        {"source.ts", "meta.block.ts", "meta.function-call.ts", "support.function.console.ts"},
        {"source.ts", "meta.class.ts", "meta.method.declaration.ts", "storage.type.function.ts"},
        {"source.ts", "meta.block.ts", "meta.brace.round.ts"},
        {"source.ts", "meta.block.ts", "constant.numeric.decimal.ts"},
        {"source.ts", "meta.type.annotation.ts", "entity.name.type.ts"},
    };
    return all;
}

void BM_StyleForScopeStack(benchmark::State& state) {
    static const TokenTheme theme = dark_plus();
    std::vector<std::vector<std::string_view>> views;
    for (const auto& stack : stacks()) {
        views.emplace_back(stack.begin(), stack.end());
    }
    std::size_t next = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(theme.style_for(views[next++ % views.size()]));
    }
    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_StyleForScopeStack)->Iterations(1'000'000);
