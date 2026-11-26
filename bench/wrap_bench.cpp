// Benchmarks of the rows of wrapped text.
//   cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON
//   build/linux-clang-release/bin/anvil_wrap_bench
#include "anvil/core/row_map.hpp"

#include <benchmark/benchmark.h>
#include <random>
#include <string>

using namespace anvil::core;

namespace {

// Source-like lines: indented, with words, spaces and punctuation, some longer than 80 columns.
std::string make_code(std::int64_t lines) {
    std::mt19937 random(42);
    const char* const words[] = {
        "const", "auto", "value", "=",        "compute(first,", "second);", "if",
        "(x",    "<",    "y)",    "return",   "result.size();", "//",       "notes",
        "std::", "{",    "}",     "\"text\","};
    std::string text;
    for (std::int64_t line = 0; line < lines; ++line) {
        text.append(4 * (random() % 4), ' ');
        for (std::size_t word = random() % 14; word > 0; --word) {
            text += words[random() % std::size(words)];
            text += ' ';
        }
        text += '\n';
    }
    return text;
}

// Paragraphs that all wrap, of 170 columns.
std::string make_prose(std::int64_t lines) {
    std::string text;
    for (std::int64_t line = 0; line < lines; ++line) {
        for (int i = 0; i < 4; ++i) {
            text += "the quick brown fox jumps over the lazy dog. ";
        }
        text += '\n';
    }
    return text;
}

// Japanese lines of 288 columns, which break between characters.
std::string make_cjk(std::int64_t lines) {
    std::string text;
    for (std::int64_t line = 0; line < lines; ++line) {
        for (int i = 0; i < 16; ++i) {
            text += "\xE3\x81\x93\xE3\x82\x8C\xE3\x81\xAF\xE6\x96\x87\xE7\xAB\xA0\xE3\x80\x82";
        }
        text += '\n';
    }
    return text;
}

void rows_of_a_document(benchmark::State& state, const std::string& source) {
    const Text text = *Text::from_utf8(source);
    std::size_t rows = 0;
    for (auto _ : state) {
        const RowMap map(text, {4, 80, 2});
        rows = map.row_count();
        benchmark::DoNotOptimize(rows);
    }
    state.counters["rows"] = static_cast<double>(rows);
    state.counters["bytes"] = static_cast<double>(source.size());
}

void RowMapOfCode(benchmark::State& state) {
    rows_of_a_document(state, make_code(state.range(0)));
}
BENCHMARK(RowMapOfCode)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

void RowMapOfProse(benchmark::State& state) {
    rows_of_a_document(state, make_prose(state.range(0)));
}
BENCHMARK(RowMapOfProse)->Arg(100'000)->Unit(benchmark::kMillisecond);

void RowMapOfJapanese(benchmark::State& state) {
    rows_of_a_document(state, make_cjk(state.range(0)));
}
BENCHMARK(RowMapOfJapanese)->Arg(100'000)->Unit(benchmark::kMillisecond);

// A character typed and deleted in the middle, with the row of the last line asked for, as a
// scroll bar and a view at the end would.
void RowMapEditInTheMiddle(benchmark::State& state) {
    Document document(*Text::from_utf8(make_code(state.range(0))));
    RowMap map(document.text(), {4, 80, 2});
    const std::size_t offset = document.text().line_start(document.text().line_count() / 2);
    bool inserted = false;
    for (auto _ : state) {
        state.PauseTiming();
        const Edit edit{offset, offset + (inserted ? 1 : 0), inserted ? "" : "\n"};
        const DocumentChange change = *document.apply_edits(std::span(&edit, 1));
        inserted = !inserted;
        state.ResumeTiming();
        map.apply(change, document.text());
        benchmark::DoNotOptimize(map.first_row(map.line_count() - 1));
    }
}
BENCHMARK(RowMapEditInTheMiddle)->Arg(100'000)->Arg(1'000'000)->Unit(benchmark::kMicrosecond);

// The rows of one very long line, which an edit anywhere in it computes again.
void RowsOfOneLongLine(benchmark::State& state) {
    std::string line;
    while (static_cast<std::int64_t>(line.size()) < state.range(0)) {
        line += "{\"key\":\"value\",\"n\":12345}, ";
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(count_rows(line, {4, 80, 2}));
    }
}
BENCHMARK(RowsOfOneLongLine)->Arg(1'000'000)->Arg(10'000'000)->Unit(benchmark::kMillisecond);

} // namespace
