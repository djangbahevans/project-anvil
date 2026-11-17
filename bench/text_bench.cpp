// Benchmarks of the rope-backed Text against a std::string baseline.
//   cmake --preset linux-clang-release -DANVIL_BUILD_BENCHMARKS=ON
//   build/linux-clang-release/bin/anvil_text_bench
#include "anvil/core/text.hpp"

#include <algorithm>
#include <benchmark/benchmark.h>
#include <malloc.h>
#include <random>
#include <string>
#include <vector>

using anvil::core::Position;
using anvil::core::Text;

namespace {

constexpr std::int64_t MiB = 1024 * 1024;

// Source-like text: lines of 20 to 80 characters, mostly ASCII with some multi-byte characters.
std::string make_source(std::size_t bytes, bool multibyte) {
    std::mt19937 random(42);
    std::string text;
    text.reserve(bytes + 100);
    while (text.size() < bytes) {
        const std::size_t length = 20 + random() % 60;
        for (std::size_t i = 0; i < length; ++i) {
            if (multibyte && random() % 50 == 0) {
                text += (random() % 2) ? "\xC3\xA9" : "\xE2\x82\xAC";
            } else {
                text += static_cast<char>('a' + random() % 26);
            }
        }
        text += '\n';
    }
    return text;
}

Text load(const std::string& source) {
    return *Text::from_utf8(source);
}

// Bytes currently allocated from the heap (glibc), unlike resident memory unaffected by pages that
// earlier allocations left behind.
std::size_t heap_bytes() {
    return mallinfo2().uordblks;
}

// --- loading and snapshots ---

void BM_Rope_Load(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    for (auto _ : state) {
        benchmark::DoNotOptimize(load(source));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
}

void BM_String_Copy(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    for (auto _ : state) {
        std::string copy = source;
        benchmark::DoNotOptimize(copy);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
}

void BM_Rope_Snapshot(benchmark::State& state) {
    const Text text = load(make_source(static_cast<std::size_t>(state.range(0)), true));
    for (auto _ : state) {
        Text snapshot = text;
        benchmark::DoNotOptimize(snapshot);
    }
}

// Heap bytes of a loaded document relative to its size.
void BM_Rope_Memory(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    for (auto _ : state) {
        state.PauseTiming();
        const std::size_t before = heap_bytes();
        state.ResumeTiming();
        Text text = load(source);
        state.PauseTiming();
        const std::size_t used = heap_bytes() - before;
        state.counters["overhead_pct"] =
            100.0 * (static_cast<double>(used) - static_cast<double>(source.size())) /
            static_cast<double>(source.size());
        state.counters["chunks"] = static_cast<double>(text.chunk_count());
        benchmark::DoNotOptimize(text);
        state.ResumeTiming();
    }
}

// --- edits ---

void BM_Rope_RandomEdit(benchmark::State& state) {
    Text text = load(make_source(static_cast<std::size_t>(state.range(0)), false));
    std::mt19937_64 random(1);
    const std::size_t size = text.size();
    for (auto _ : state) {
        const std::size_t offset = random() % size;
        text = *text.replaced(offset, offset, "abcd");
    }
}

void BM_String_RandomEdit(benchmark::State& state) {
    std::string text = make_source(static_cast<std::size_t>(state.range(0)), false);
    std::mt19937_64 random(1);
    const std::size_t size = text.size();
    for (auto _ : state) {
        const std::size_t offset = random() % size;
        text.insert(offset, "abcd");
        benchmark::ClobberMemory();
    }
}

// Typing: single-character inserts at a cursor that moves forward.
void BM_Rope_Typing(benchmark::State& state) {
    Text text = load(make_source(static_cast<std::size_t>(state.range(0)), false));
    std::size_t cursor = text.size() / 2;
    for (auto _ : state) {
        text = *text.replaced(cursor, cursor, "x");
        ++cursor;
    }
}

void BM_String_Typing(benchmark::State& state) {
    std::string text = make_source(static_cast<std::size_t>(state.range(0)), false);
    std::size_t cursor = text.size() / 2;
    for (auto _ : state) {
        text.insert(cursor, 1, 'x');
        ++cursor;
        benchmark::ClobberMemory();
    }
}

// Edit while a snapshot is held, as when a background job reads the previous version.
void BM_Rope_EditWithLiveSnapshot(benchmark::State& state) {
    Text text = load(make_source(static_cast<std::size_t>(state.range(0)), false));
    const Text held = text;
    std::mt19937_64 random(1);
    const std::size_t size = text.size();
    for (auto _ : state) {
        const std::size_t offset = random() % size;
        text = *text.replaced(offset, offset, "abcd");
    }
    benchmark::DoNotOptimize(held.size());
}

// --- queries ---

struct LineIndex {
    explicit LineIndex(const std::string& source) {
        starts.push_back(0);
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\n') {
                starts.push_back(i + 1);
            }
        }
    }
    std::vector<std::size_t> starts;
};

void BM_Rope_PositionAt(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    const Text text = load(source);
    std::vector<std::size_t> offsets;
    std::mt19937_64 random(2);
    for (int i = 0; i < 4096; ++i) {
        std::size_t offset = random() % source.size();
        while (!text.is_char_boundary(offset)) {
            ++offset;
        }
        offsets.push_back(offset);
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(text.position_at(offsets[i++ % offsets.size()]));
    }
}

// The fair baseline for a flat string: a line-start index and a UTF-16 scan of the line.
void BM_String_PositionAt(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    const LineIndex index(source);
    std::vector<std::size_t> offsets;
    std::mt19937_64 random(2);
    for (int i = 0; i < 4096; ++i) {
        std::size_t offset = random() % source.size();
        while ((static_cast<unsigned char>(source[offset]) & 0xC0u) == 0x80u) {
            ++offset;
        }
        offsets.push_back(offset);
    }
    std::size_t i = 0;
    for (auto _ : state) {
        const std::size_t offset = offsets[i++ % offsets.size()];
        const auto line = static_cast<std::size_t>(
            std::upper_bound(index.starts.begin(), index.starts.end(), offset) -
            index.starts.begin() - 1);
        std::size_t units = 0;
        for (std::size_t p = index.starts[line]; p < offset; ++p) {
            const auto byte = static_cast<unsigned char>(source[p]);
            if ((byte & 0xC0u) != 0x80u) {
                units += byte >= 0xF0 ? 2 : 1;
            }
        }
        benchmark::DoNotOptimize(Position{line, units});
    }
}

void BM_Rope_OffsetAt(benchmark::State& state) {
    const std::string source = make_source(static_cast<std::size_t>(state.range(0)), true);
    const Text text = load(source);
    std::vector<Position> positions;
    std::mt19937_64 random(3);
    for (int i = 0; i < 4096; ++i) {
        positions.push_back({random() % text.line_count(), random() % 60});
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(text.offset_at(positions[i++ % positions.size()]));
    }
}

void BM_Rope_Line(benchmark::State& state) {
    const Text text = load(make_source(static_cast<std::size_t>(state.range(0)), true));
    std::vector<std::size_t> lines;
    std::mt19937_64 random(4);
    for (int i = 0; i < 4096; ++i) {
        lines.push_back(random() % text.line_count());
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(text.line(lines[i++ % lines.size()]));
    }
}

} // namespace

#define SIZES ->Arg(1 * MiB)->Arg(10 * MiB)->Arg(100 * MiB)

BENCHMARK(BM_Rope_Load) SIZES->Unit(benchmark::kMillisecond);
BENCHMARK(BM_String_Copy) SIZES->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Rope_Snapshot) SIZES;
BENCHMARK(BM_Rope_Memory) SIZES->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Rope_RandomEdit) SIZES;
BENCHMARK(BM_String_RandomEdit) SIZES;
BENCHMARK(BM_Rope_Typing) SIZES;
BENCHMARK(BM_String_Typing) SIZES;
BENCHMARK(BM_Rope_EditWithLiveSnapshot) SIZES;
BENCHMARK(BM_Rope_PositionAt) SIZES;
BENCHMARK(BM_String_PositionAt) SIZES;
BENCHMARK(BM_Rope_OffsetAt) SIZES;
BENCHMARK(BM_Rope_Line) SIZES;
