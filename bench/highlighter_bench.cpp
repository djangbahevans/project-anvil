// Benchmarks of the tree-sitter highlighter on 5 MB JSON documents: the first parse, and the
// reparse after an edit of one character. "Nested" is objects within objects, 14 to 70 members
// each; "Flat" is one object with 70,000 members, which tree-sitter reparses much more slowly
// because the edit has to be placed among that many siblings.
//   cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON
//   cmake --build --preset linux-clang-release --target anvil_highlighter_bench
//   build/linux-clang-release/bin/anvil_highlighter_bench
#include "anvil/core/highlight_worker.hpp"
#include "anvil/core/text_buffer.hpp"
#include "anvil/core/tree_sitter_highlighter.hpp"

#include <benchmark/benchmark.h>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

using namespace anvil::core;

namespace {

Text nested_json() {
    std::string out = "{\n";
    for (int a = 0; a < 70; ++a) {
        out += "  \"group" + std::to_string(a) + "\": {\n";
        for (int b = 0; b < 70; ++b) {
            out += "    \"item" + std::to_string(b) + "\": {\n";
            for (int c = 0; c < 14; ++c) {
                out += "      \"key" + std::to_string(c) +
                       "\": [1, 2.5, \"value\", true, null, {\"a\": \"b\"}]";
                out += c + 1 < 14 ? ",\n" : "\n";
            }
            out += b + 1 < 70 ? "    },\n" : "    }\n";
        }
        out += a + 1 < 70 ? "  },\n" : "  }\n";
    }
    out += "}\n";
    return *Text::from_utf8(out);
}

Text flat_json() {
    std::string out = "{\n";
    constexpr std::size_t lines = 5u * 1000 * 1000 / 70;
    for (std::size_t i = 0; i < lines; ++i) {
        out +=
            "  \"key" + std::to_string(i) + "\": [1, 2.5, \"value\", true, null, {\"a\": \"b\"}]";
        out += i + 1 < lines ? ",\n" : "\n";
    }
    out += "}\n";
    auto text = Text::from_utf8(out);
    if (!text) {
        std::fprintf(stderr, "cannot build the document\n");
        std::exit(1);
    }
    return *text;
}

std::unique_ptr<TreeSitterHighlighter> make(const Text& text) {
    auto highlighter = TreeSitterHighlighter::create("json", text);
    if (!highlighter) {
        std::fprintf(stderr, "cannot highlight: %s\n", highlighter.error().message.c_str());
        std::exit(1);
    }
    return std::move(*highlighter);
}

Text document(bool nested) {
    return nested ? nested_json() : flat_json();
}

void FirstParse(benchmark::State& state) {
    const Text text = document(state.range(0) != 0);
    for (auto _ : state) {
        benchmark::DoNotOptimize(make(text));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(FirstParse)->Arg(1)->Arg(0)->ArgName("nested")->Unit(benchmark::kMillisecond);

// Types a character and removes it again, so that every iteration is an edit of one character in
// the middle of the document and the document stays the same.
void ReparseAfterOneCharacter(benchmark::State& state) {
    TextBuffer buffer(document(state.range(0) != 0));
    const auto highlighter = make(buffer.snapshot());
    const std::size_t at = buffer.snapshot().size() / 2;
    bool insert = true;
    for (auto _ : state) {
        const auto change = insert ? buffer.insert(at, " ") : buffer.erase(at, at + 1);
        insert = !insert;
        if (!change || !highlighter->apply(*change, buffer.snapshot())) {
            state.SkipWithError("edit failed");
            break;
        }
    }
}
BENCHMARK(ReparseAfterOneCharacter)
    ->Arg(1)
    ->Arg(0)
    ->ArgName("nested")
    ->Unit(benchmark::kMicrosecond);

// The same edit followed by asking for the spans of the lines in view.
void ReparseAndSpansOfAScreen(benchmark::State& state) {
    TextBuffer buffer(document(state.range(0) != 0));
    const auto highlighter = make(buffer.snapshot());
    const std::size_t line = buffer.snapshot().line_count() / 2;
    const std::size_t at = buffer.snapshot().line_start(line) + 10;
    bool insert = true;
    for (auto _ : state) {
        const auto change = insert ? buffer.insert(at, " ") : buffer.erase(at, at + 1);
        insert = !insert;
        if (!change || !highlighter->apply(*change, buffer.snapshot())) {
            state.SkipWithError("edit failed");
            break;
        }
        for (std::size_t row = line - 30; row < line + 30; ++row) {
            benchmark::DoNotOptimize(highlighter->spans(row));
        }
    }
}
BENCHMARK(ReparseAndSpansOfAScreen)
    ->Arg(1)
    ->Arg(0)
    ->ArgName("nested")
    ->Unit(benchmark::kMicrosecond);

// From an edit of one character to the worker's result for it, with the parse on the worker's
// thread: the time before the editor can show the colors of what was typed.
void WorkerRoundTrip(benchmark::State& state) {
    TextBuffer buffer(document(state.range(0) != 0));
    std::mutex mutex;
    std::condition_variable arrived;
    std::uint64_t delivered = 0;
    auto worker =
        HighlightWorker::create("json", buffer.snapshot(), 1,
                                [&](std::uint64_t version, std::shared_ptr<const Highlighter>) {
                                    {
                                        const std::lock_guard lock(mutex);
                                        delivered = version;
                                    }
                                    arrived.notify_all();
                                });
    if (!worker) {
        state.SkipWithError("no worker");
        return;
    }
    const auto wait_for = [&](std::uint64_t version) {
        std::unique_lock lock(mutex);
        arrived.wait(lock, [&] { return delivered >= version; });
    };
    wait_for(1);
    const std::size_t at = buffer.snapshot().size() / 2;
    std::uint64_t version = 1;
    bool insert = true;
    for (auto _ : state) {
        const auto change = insert ? buffer.insert(at, " ") : buffer.erase(at, at + 1);
        insert = !insert;
        (*worker)->edit(DocumentChange{++version, ChangeCause::edit, {*change}}, buffer.snapshot());
        wait_for(version);
    }
}
BENCHMARK(WorkerRoundTrip)->Arg(1)->Arg(0)->ArgName("nested")->Unit(benchmark::kMicrosecond);

} // namespace
