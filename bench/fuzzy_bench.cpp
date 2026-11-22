// Benchmarks of fuzzy matching over a command-palette sized list.
//   cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON
//   cmake --build --preset linux-clang-release --target anvil_fuzzy_bench
//   build/linux-clang-release/bin/anvil_fuzzy_bench
#include "anvil/core/fuzzy.hpp"

#include <algorithm>
#include <benchmark/benchmark.h>
#include <numeric>
#include <string>
#include <vector>

using namespace anvil::core;

namespace {

// Titles shaped like "View: Toggle Sidebar Visibility", from a fixed vocabulary.
std::vector<std::string> make_titles(std::size_t count) {
    const std::vector<std::string> categories = {"View",  "File",     "Git",       "Preferences",
                                                 "Debug", "Terminal", "Developer", "Go",
                                                 "Edit",  "Search"};
    const std::vector<std::string> verbs = {"Toggle", "Open",   "Close", "Focus", "Move", "Select",
                                            "Delete", "Insert", "Show",  "Hide",  "Run",  "Reload"};
    const std::vector<std::string> nouns = {
        "Sidebar", "Panel",    "Terminal", "Editor",  "Settings", "Workspace", "Folder", "Line",
        "Word",    "Problems", "Output",   "Console", "Window",   "Branch",    "Stash"};
    std::vector<std::string> titles;
    titles.reserve(count);
    for (std::size_t i = 0; titles.size() < count; ++i) {
        titles.push_back(categories[i % categories.size()] + ": " + verbs[(i / 10) % verbs.size()] +
                         " " + nouns[(i / 120) % nouns.size()] + " " + std::to_string(i));
    }
    return titles;
}

void score_all(benchmark::State& state, const char* query_text) {
    const std::vector<std::string> titles = make_titles(static_cast<std::size_t>(state.range(0)));
    const PreparedQuery query = prepare_query(query_text);
    for (auto _ : state) {
        std::size_t matched = 0;
        for (const std::string& title : titles) {
            matched += score_item({title, "", ""}, query).score != 0 ? 1 : 0;
        }
        benchmark::DoNotOptimize(matched);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_ScoreShortQuery(benchmark::State& state) {
    score_all(state, "op");
}
void BM_ScoreWordQuery(benchmark::State& state) {
    score_all(state, "toggle side");
}
void BM_ScoreNoMatch(benchmark::State& state) {
    score_all(state, "zzq");
}

void BM_ScoreAndSort(benchmark::State& state) {
    const std::vector<std::string> titles = make_titles(static_cast<std::size_t>(state.range(0)));
    const PreparedQuery query = prepare_query("tg sd");
    for (auto _ : state) {
        std::vector<ItemScore> scores;
        scores.reserve(titles.size());
        for (const std::string& title : titles) {
            scores.push_back(score_item({title, "", ""}, query));
        }
        std::vector<std::size_t> order(titles.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::erase_if(order, [&](std::size_t i) { return scores[i].score == 0; });
        std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
            return compare_items({titles[a], "", ""}, scores[a], {titles[b], "", ""}, scores[b],
                                 query) < 0;
        });
        benchmark::DoNotOptimize(order);
    }
}

} // namespace

BENCHMARK(BM_ScoreShortQuery)->Arg(5000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ScoreWordQuery)->Arg(5000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ScoreNoMatch)->Arg(5000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ScoreAndSort)->Arg(5000)->Unit(benchmark::kMillisecond);
