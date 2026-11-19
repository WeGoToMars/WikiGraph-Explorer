#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <locale>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Graph/Budget.h"
#include "Graph/GraphBuilder.h"
#include "Graph/GraphStore.h"
#include "Graph/Platform.h"
#include "Log/FileLog.h"
#include "UI/UI.h"
#include "UI/UIBase.h"
#include "Utils/PathUtils.h"
#include "Utils/StringUtils.h"
#include "spdlog/spdlog.h"

namespace {

constexpr const char* kUsage = R"(WikiGraph Explorer

Usage:
  wikigraph [options]                                  interactive terminal UI
  wikigraph --wiki <name> [--query <from> <to>] [--bench <n>] [options]
                                                       headless: build or open one wiki, print the results

Options:
  --ram-budget <size>   RAM the graph may use: a size in GB ("1.5", "800M", "2G"), "all" (every list in RAM,
                        no disk reads during search) or "auto" (default: all if it fits in half of physical
                        memory, otherwise half of physical memory). Below the full size, neighbour lists are read
                        from the SSD and the budget holds the most-read blocks.
  --rebuild             rebuild the graph from the dumps even if an up-to-date one exists
  --data-dir <folder>   where dumps and graphs are stored (also WIKIGRAPH_DATA_DIR). Default: resources/data
                        next to the build folder, or the per-user data folder if that cannot be created
  --wiki <name>         dump name in the data directory, e.g. simplewiki-20261101, or a path prefix
                        such as /dumps/enwiki-20261101
  --query <from> <to>   print the shortest paths between two article titles
  --bench <n>           time n searches between random articles
  --paths <n>           paths to print for --query (default 10)
  -h, --help            show this help
)";

struct Options {
    uint64_t budget = wg::kBudgetAuto;
    bool rebuild = false;
    std::string wiki;
    std::string data_dir;
    std::string from, to;
    size_t bench = 0;
    size_t paths = 10;
};

// ------------------------------------------------------------------------------------------------ arguments

/// The whole of `text` as a number, or nothing.
template <class T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

/// Set `out` from an option's value; reports a bad value and returns false.
template <class T>
bool set_number(T& out, std::string_view option, std::string_view text) {
    const auto v = parse_number<T>(text);
    if (!v)
        std::println(stderr, "bad {} value: {}", option, text);
    else
        out = *v;
    return v.has_value();
}

bool set_budget(uint64_t& out, std::string_view text) {
    const auto v = wg::parse_budget(text);
    if (!v)
        std::println(stderr, "bad --ram-budget value: {}", text);
    else
        out = *v;
    return v.has_value();
}

using Values = std::span<const std::string_view>;

/// A command-line option: its name, how many values follow it, and how they set the options (false: a bad value,
/// already reported). An option with one value also takes it as --name=value.
struct OptionSpec {
    std::string_view name;
    size_t values;
    bool (*apply)(Options&, Values);
};

constexpr std::array kOptions = {
    OptionSpec{"--ram-budget", 1, [](Options& o, Values v) { return set_budget(o.budget, v[0]); }},
    OptionSpec{"--rebuild", 0,
               [](Options& o, Values) {
                   o.rebuild = true;
                   return true;
               }},
    OptionSpec{"--data-dir", 1,
               [](Options& o, Values v) {
                   o.data_dir = v[0];
                   return true;
               }},
    OptionSpec{"--wiki", 1,
               [](Options& o, Values v) {
                   o.wiki = v[0];
                   return true;
               }},
    OptionSpec{"--query", 2,
               [](Options& o, Values v) {
                   o.from = v[0];
                   o.to = v[1];
                   return true;
               }},
    OptionSpec{"--bench", 1, [](Options& o, Values v) { return set_number(o.bench, "--bench", v[0]); }},
    OptionSpec{"--paths", 1, [](Options& o, Values v) { return set_number(o.paths, "--paths", v[0]); }},
};

/// The values of the option at args[i], which may be attached (--name=value); moves `i` past them. Reports and
/// returns nothing if they are missing.
std::optional<std::vector<std::string_view>> take_values(std::span<char* const> args, size_t& i, const OptionSpec& spec,
                                                         std::optional<std::string_view> attached) {
    if (attached) return std::vector{*attached};
    if (i + spec.values >= args.size()) {
        std::println(stderr, "{} needs {} argument{}", spec.name, spec.values, spec.values == 1 ? "" : "s");
        return std::nullopt;
    }
    auto values = args.subspan(i + 1, spec.values) | std::ranges::to<std::vector<std::string_view>>();
    i += spec.values;
    return values;
}

/// Prints the problem and returns nothing on bad arguments; exits after --help.
std::optional<Options> parse_args(std::span<char* const> args) {
    Options o;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            std::print("{}", kUsage);
            std::exit(0);
        }
        std::optional<std::string_view> attached;  // --name=value
        if (const size_t eq = arg.find('='); arg.starts_with("--") && eq != std::string_view::npos) {
            attached = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        }
        const auto spec = std::ranges::find(kOptions, arg, &OptionSpec::name);
        if (spec == kOptions.end() || (attached && spec->values != 1)) {
            std::print(stderr, "unknown option: {}\n\n{}", args[i], kUsage);
            return std::nullopt;
        }
        const auto values = take_values(args, i, *spec, attached);
        if (!values || !spec->apply(o, *values)) return std::nullopt;
    }
    if (o.wiki.empty() && (!o.from.empty() || o.bench != 0)) {
        std::println(stderr, "--query and --bench need --wiki");
        return std::nullopt;
    }
    return o;
}

// ------------------------------------------------------------------------------------------------ headless run

double gb(uint64_t b) { return static_cast<double>(b) / wg::kBytesPerGB; }

/// The dumps of `wiki` (a name in the data folder, or a path prefix); prints what is missing.
std::optional<wg::DumpFiles> find_dumps(const std::string& wiki) {
    std::filesystem::path prefix = wiki;
    if (wiki.find('/') == std::string::npos && wiki.find('\\') == std::string::npos)
        prefix = wg::paths::data_dir() / wiki;
    const std::string p = prefix.string();
    wg::DumpFiles dumps{p + "-page.sql.gz", p + "-linktarget.sql.gz", p + "-pagelinks.sql.gz", p + "-redirect.sql.gz"};
    for (const auto* f : {&dumps.page, &dumps.linktarget, &dumps.pagelinks})
        if (!std::filesystem::exists(*f)) {
            std::println(stderr, "missing dump file: {}", f->string());
            return std::nullopt;
        }
    if (!std::filesystem::exists(dumps.redirect)) {
        std::println(stderr, "note: no redirect dump ({}); links through redirects will be dropped",
                     dumps.redirect.string());
        dumps.redirect.clear();
    }
    return dumps;
}

/// Build the graph unless an up-to-date one exists (or `rebuild`), printing the steps to stderr.
void build_if_needed(const wg::DumpFiles& dumps, bool rebuild) {
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::optional<wg::BuildStep> last_step;
    auto on_progress = [&](const wg::BuildProgress& pr) {
        if (pr.step == last_step) return;
        last_step = pr.step;
        std::println(stderr, "[build] {}...", wg::build_step_name(pr.step));
    };
    wg::BuildReport rep;
    if (!wg::ensure_graph(dumps, rebuild, threads, on_progress, &rep)) return;
    std::println(
        "built: {} articles, {} links ({} duplicates merged), {} redirects ({} unresolved) in {:.1f} s, "
        "peak memory {:.2f} GB",
        rep.articles, rep.edges, rep.duplicate_links, rep.redirects, rep.redirects_unresolved, rep.seconds,
        gb(rep.peak_footprint));
}

void print_open_info(const wg::GraphStore& g, uint64_t budget) {
    const auto& info = g.info();
    std::println(
        "opened in {:.2f} s | {} articles, {} links | budget {} -> {} | fixed {:.3f} GB + lists cached {:.3f} of "
        "{:.3f} GB | process {:.2f} GB",
        info.open_seconds, info.nodes, info.edges, wg::budget_to_string(budget),
        info.all_in_ram ? "all in RAM" : "SSD + hot set", gb(info.fixed_bytes), gb(info.cached_bytes),
        gb(info.list_bytes), gb(wg::process_footprint_bytes()));
}

/// --query: print the shortest paths between two titles. Returns the exit code.
int run_query(wg::GraphStore& g, const Options& o) {
    const auto from = g.find(o.from), to = g.find(o.to);
    if (!from || !to) {
        std::println(stderr, "title not found: {}", !from ? o.from : o.to);
        return 1;
    }
    for (const auto& [typed, m] : {std::pair{&o.from, *from}, std::pair{&o.to, *to}})
        if (m.via_redirect) std::println("{} redirects to {}", *typed, g.display_title(m.node));
    const uint32_t s = from->node, t = to->node;
    const auto r = g.shortest_paths(s, t, o.paths);
    if (r.distance < 0) {
        std::println("no path from {} to {} ({:.3f} ms)", g.display_title(s), g.display_title(t), r.milliseconds);
        return 0;
    }
    std::println("{} -> {}: {} click{}, {} shortest path{} | {:.3f} ms, {} lists, {} blocks from disk",
                 g.display_title(s), g.display_title(t), r.distance, r.distance == 1 ? "" : "s", r.path_count,
                 r.path_count == 1 ? "" : "s", r.milliseconds, r.lists_expanded, r.blocks_read);
    auto title = [&](uint32_t id) { return g.display_title(id); };
    for (const auto& path : r.paths)
        std::println("  {}", string_utils::join(path | std::views::transform(title), " -> "));
    return 0;
}

/// --bench: time `n` searches between random articles (the same pairs every run).
void run_bench(wg::GraphStore& g, size_t n) {
    constexpr uint64_t kBenchSeed = 7;
    std::vector<double> ms;
    double blocks = 0;
    size_t connected = 0;
    for (const auto [s, t] : g.random_pairs(n, kBenchSeed)) {
        const auto r = g.shortest_paths(s, t, 0);
        ms.push_back(r.milliseconds);
        blocks += static_cast<double>(r.blocks_read);
        connected += r.distance >= 0;
    }
    std::ranges::sort(ms);
    auto pct = [&](double q) { return ms[static_cast<size_t>(q * static_cast<double>(ms.size() - 1))]; };
    const auto count = static_cast<double>(ms.size());
    std::println(
        "bench: {} searches ({} connected) | mean {:.3f} ms | p50 {:.3f} | p95 {:.3f} | p99 {:.3f} | max "
        "{:.3f} | blocks from disk per search {:.0f} | process {:.2f} GB",
        ms.size(), connected, std::ranges::fold_left(ms, 0.0, std::plus{}) / count, pct(0.5), pct(0.95), pct(0.99),
        pct(1.0), blocks / count, gb(wg::process_footprint_bytes()));
}

int run_headless(const Options& o) {
    const auto dumps = find_dumps(o.wiki);
    if (!dumps) return 1;
    build_if_needed(*dumps, o.rebuild);
    wg::GraphStore g;
    g.open(wg::graph_dir_for(*dumps), o.budget);
    print_open_info(g, o.budget);
    if (!o.from.empty() && run_query(g, o) != 0) return 1;
    if (o.bench != 0) run_bench(g, o.bench);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto parsed = parse_args(std::span(argv, static_cast<size_t>(argc)));
    if (!parsed) return 2;
    const Options& opts = *parsed;

    wg::paths::init(opts.data_dir);
    if (const std::string log_error = wg::app::init_logfile(); !log_error.empty())
        std::println(stderr, "warning: {}; continuing without a log", log_error);

    // A UTF-8 locale, for thousands separators in numbers ({:L}). en_US.UTF-8 is not installed everywhere (minimal
    // Linux systems and containers often lack it), and constructing a missing locale throws.
    for (const char* name : {"en_US.UTF-8", "C.UTF-8", ""}) {
        try {
            std::locale::global(std::locale(name));
            break;
        } catch (const std::runtime_error&) {
            continue;  // not installed here: try the next one
        }
    }

    if (!opts.wiki.empty()) {
        try {
            return run_headless(opts);
        } catch (const std::exception& e) {
            std::println(stderr, "error: {}", e.what());
            return 1;
        }
    }

    wg::app::UIState state;
    state.ram_budget = opts.budget;
    state.force_rebuild = opts.rebuild;
    auto graph = std::make_unique<wg::GraphStore>();
    state.graph = graph.get();

    // Blocks until the user quits; background work is stopped and joined before it returns
    try {
        wg::app::run_ui(state);
    } catch (const std::exception& e) {
        // The screen has been restored by now (App's destructor). Stop the background work before
        // `graph` and `state` go away, then report.
        state.stop_all();
        state.threads.join_all();
        spdlog::error("The terminal UI failed: {}", e.what());
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
