// (c) 2023 Pascal Mehnert
// This code is licensed under BSD 2-Clause License (see LICENSE for details)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>

#include <kamping/collectives/barrier.hpp>
#include <kamping/communicator.hpp>
#include <kamping/environment.hpp>
#include <kamping/named_parameters.hpp>
#include <tlx/cmdline_parser.hpp>
#include <tlx/die.hpp>
#include <tlx/die/core.hpp>
#include <tlx/math/div_ceil.hpp>
#include <tlx/sort/strings/string_ptr.hpp>

#include "executables/common_cli.hpp"
#include "mpi/communicator.hpp"
#include "mpi/is_sorted.hpp"
#include "sorter/distributed/space_efficient.hpp"
#include "strings/stringset.hpp"
#include "util/measuringTool.hpp"
#include "util/string_generator.hpp"

enum class CombinedGenerator { none = 0, dn_ratio, sentinel };

enum class CharGenerator { random = 0, file, file_segment, sentinel };

enum class StringGenerator { suffix = 0, window, difference_cover, sentinel };

enum class Permutation { simple = 0, multi_level, non_unique, sentinel };

struct SorterArgs : public CommonArgs {
    SamplerArgs quantile_sampler;
    size_t combined_gen = static_cast<size_t>(CombinedGenerator::none);
    size_t char_gen = static_cast<size_t>(CharGenerator::random);
    size_t string_gen = static_cast<size_t>(StringGenerator::suffix);
    bool use_proper_dc = false;
    size_t step = 1;
    size_t num_chars = 100000;
    size_t num_strings = 10000;
    size_t len_strings = 500;
    size_t difference_cover = 3;
    double dn_ratio = 0.5;
    bool shuffle = false;
    std::string path;
    size_t permutation = static_cast<size_t>(Permutation::multi_level);
    size_t quantile_size = 100 * 1024 * 1024;
    size_t iteration = 0;
    std::vector<size_t> levels;

    // todo print config

    std::string get_prefix(dss_mehnert::Communicator const& comm) const {
        // clang-format off
        return CommonArgs::get_prefix(comm)
               + " quantile_chars="   + std::to_string(quantile_sampler.sample_chars)
               + " quantile_indexed=" + std::to_string(quantile_sampler.sample_indexed)
               + " quantile_random="  + std::to_string(quantile_sampler.sample_random)
               + " quantile_factor="  + std::to_string(quantile_sampler.sampling_factor)
               + " num_chars="        + std::to_string(num_chars)
               + " num_strings="      + std::to_string(num_strings)
               + " len_strings="      + std::to_string(len_strings)
               + " step="             + std::to_string(step)
               + " dn_ratio="         + std::to_string(dn_ratio)
               + " difference_cover=" + std::to_string(difference_cover)
               + " num_levels="       + std::to_string(levels.size())
               + " quantile_size="    + std::to_string(quantile_size)
               + " iteration="        + std::to_string(iteration);
        // clang-format on
    }
};

template <typename StringSet>
auto generate_compressed_strings(SorterArgs const& args, dss_mehnert::Communicator const& comm) {
    using namespace dss_mehnert;

    auto& measuring_tool = measurement::MeasuringTool::measuringTool();

    comm.barrier();
    measuring_tool.start("generate_strings");

    auto input_chars = [&]() -> std::vector<typename StringSet::Char> {
        if (args.combined_gen != static_cast<size_t>(CombinedGenerator::none)) {
            return {};
        }

        switch (clamp_enum_value<CharGenerator>(args.char_gen)) {
            case CharGenerator::random: {
                return RandomCharGenerator<StringSet>{args.num_chars};
            }
            case CharGenerator::file: {
                return FileCharGenerator<StringSet>{args.path, comm};
            }
            case CharGenerator::file_segment: {
                return FileSegmentCharGenerator<StringSet>{args.path, args.num_chars, comm};
            }
            case CharGenerator::sentinel: {
                break;
            }
        }
        tlx_die("invalid chararcter generator");
    }();

    auto input_strings = [&]() -> std::vector<typename StringSet::String> {
        if (args.combined_gen != static_cast<size_t>(CombinedGenerator::none)) {
            return {};
        }

        switch (clamp_enum_value<StringGenerator>(args.string_gen)) {
            case StringGenerator::suffix: {
                return CompressedSuffixGenerator<StringSet>{input_chars, args.step};
            }
            case StringGenerator::window: {
                return CompressedWindowGenerator<StringSet>{input_chars,
                                                            args.len_strings,
                                                            args.step};
            }
            case StringGenerator::difference_cover: {
                return CompressedDifferenceCoverGenerator<StringSet>{input_chars,
                                                                     args.difference_cover,
                                                                     args.use_proper_dc,
                                                                     comm};
            }
            case StringGenerator::sentinel: {
                break;
            }
        }
        tlx_die("invalid string generator");
    }();

    auto input_container = [&]() -> StringLcpContainer<StringSet> {
        switch (clamp_enum_value<CombinedGenerator>(args.combined_gen)) {
            case CombinedGenerator::none: {
                return {std::move(input_chars), std::move(input_strings)};
            }
            case CombinedGenerator::dn_ratio: {
                return CompressedDNRatioGenerator<StringSet>{args.num_strings,
                                                             args.len_strings,
                                                             args.dn_ratio,
                                                             comm};
            }
            case CombinedGenerator::sentinel: {
                break;
            }
        }
        tlx_die("invalid combined generator");
    }();

    if (args.shuffle) {
        std::random_device rd;
        std::mt19937 gen{rd()};
        auto& strings = input_container.get_strings();
        std::shuffle(strings.begin(), strings.end(), gen);
    }

    measuring_tool.stop("generate_strings");

    comm.barrier();

    measuring_tool.add(input_container.size(), "input_strings");
    measuring_tool.add(input_container.char_size(), "input_chars");

    auto const num_uncompressed_chars = input_container.make_string_set().get_sum_length();
    measuring_tool.add(num_uncompressed_chars, "uncompressed_input_chars");

    return input_container;
}

inline std::vector<size_t> distribute_ranks(std::vector<size_t> const& global_ranks,
                                            dss_mehnert::Communicator const& comm) {
    namespace kmp = kamping;

    auto const begin = global_ranks.begin(), end = global_ranks.end();
    auto const local_max = std::max_element(begin, end);
    auto const upper_bound =
        comm.allreduce_single(kmp::send_buf(local_max == end ? 0 : *local_max + 1),
                              kmp::op(kmp::ops::max<>{}));
    auto const interval_size = tlx::div_ceil(upper_bound, comm.size());
    auto const dest = [=](auto const rank) { return rank / interval_size; };

    std::vector<int> counts(comm.size()), offsets(comm.size());
    std::for_each(begin, end, [&](auto const x) { ++counts[dest(x)]; });
    std::exclusive_scan(counts.begin(), counts.end(), offsets.begin(), 0);

    std::vector<size_t> send_buf(global_ranks.size()), recv_buf;
    std::for_each(begin, end, [&](auto const x) { send_buf[offsets[dest(x)]++] = x; });
    comm.alltoallv(kmp::send_buf(send_buf), kmp::send_counts(counts), kmp::recv_buf(recv_buf));
    return recv_buf;
}

inline void count_duplicate_ranks(std::vector<size_t> const& global_ranks,
                                  dss_mehnert::Communicator const& comm) {
    auto dist_ranks = distribute_ranks(global_ranks, comm);
    size_t total_ranks = dist_ranks.size(), distinct_ranks = 0, duplicate_ranks = 0;

    if (!dist_ranks.empty()) {
        auto const begin = dist_ranks.begin(), end = dist_ranks.end();
        std::sort(begin, end);

        std::adjacent_difference(begin, end, begin);
        *begin = 1;
        distinct_ranks = std::count(begin, end, size_t{1});

        std::adjacent_difference(begin, end, begin, [](auto const& lhs, auto const& rhs) {
            return lhs == 1 && rhs == 0;
        });
        *begin = 0;
        duplicate_ranks = std::count(begin, end, size_t{1});
    }

    using dss_mehnert::measurement::MeasuringTool;
    auto& measuring_tool = MeasuringTool::measuringTool();

    measuring_tool.add(total_ranks, "total_ranks");
    measuring_tool.add(distinct_ranks, "distinct_ranks");
    measuring_tool.add(duplicate_ranks, "duplicate_ranks");

    // first occurrence of repeated ranks plus each additional occurrence
    size_t const non_unique_ranks = total_ranks - distinct_ranks;
    measuring_tool.add(non_unique_ranks + duplicate_ranks, "total_duplicates");
}

template <typename CharType, typename AlltoallConfig, typename BloomFilter, typename Permutation>
void run_space_efficient_sort(SorterArgs const& args,
                              std::string prefix,
                              dss_mehnert::Communicator const& comm) {
    namespace sems = dss_mehnert::sorter::space_efficient;

    using dss_mehnert::IntLength;
    using dss_mehnert::SpaceEfficientPartitionPolicy;

    constexpr bool is_unique = Permutation::is_unique;

    constexpr auto config = AlltoallConfig();
    using PartitionPolicy = SpaceEfficientPartitionPolicy<CharType, IntLength, Permutation>;
    using StringSet = dss_mehnert::CompressedStringSet<CharType, IntLength>;

    auto run_sorter = [&]<typename BloomFilterPolicy>(BloomFilterPolicy bloom_filter) {
        using Subcommunicators = BloomFilterPolicy::Subcommunicators;
        using Sorter = sems::SpaceEfficientSort<PartitionPolicy, BloomFilterPolicy, Permutation>;

        using dss_mehnert::measurement::MeasuringTool;
        auto& measuring_tool = MeasuringTool::measuringTool();
        measuring_tool.setPrefix(prefix);
        measuring_tool.setVerbose(args.verbose);

        measuring_tool.disableCommVolume();
        auto input_container = generate_compressed_strings<StringSet>(args, comm);
        measuring_tool.enableCommVolume();

        dss_mehnert::SpaceEfficientChecker<is_unique, StringSet> checker;
        if (args.check_sorted || args.check_complete) {
            checker.store_container(input_container);
        }

        comm.barrier();

        measuring_tool.start("none", "create_communicators");
        auto const first_level = get_first_level(args.levels, comm);
        Subcommunicators comms{first_level, args.levels.end(), comm};
        measuring_tool.stop("none", "create_communicators", comm);

        measuring_tool.start("none", "sorting_overall");
        Sorter merge_sort{std::move(bloom_filter),
                          dss_mehnert::init_partition_policy<CharType, PartitionPolicy>(
                              args.quantile_sampler,
                              args.get_splitter_sorter()),
                          args.quantile_size};
        auto global_ranks = merge_sort.sort(std::move(input_container), comms);
        measuring_tool.stop("none", "sorting_overall", comm);

        measuring_tool.disableCommVolume();
        count_duplicate_ranks(global_ranks, comm);

        measuring_tool.disable();

        if (args.check_sorted) {
            auto const is_sorted = checker.is_sorted(global_ranks, comm);
            die_verbose_unless(is_sorted, "output permutation is not sorted");
        }
        if (args.check_complete) {
            auto const is_complete = checker.is_complete(global_ranks, comm);
            die_verbose_unless(is_complete, "output permutation is not complete");
        }

        measuring_tool.write_on_root(std::cout, comm);
        measuring_tool.reset();
    };

    auto dispatch = [&]<typename RedistributionPolicy>(RedistributionPolicy redistribution) {
        if (args.prefix_doubling) {
            using BloomFilterPolicy =
                sems::BloomFilterFirst<config, RedistributionPolicy, PartitionPolicy, BloomFilter>;
            run_sorter(
                BloomFilterPolicy{dss_mehnert::init_partition_policy<CharType, PartitionPolicy>(
                                      args.sampler,
                                      args.get_splitter_sorter()),
                                  std::move(redistribution)});
        } else {
            // todo maybe add cmake flag for this
            using BloomFilterPolicy =
                sems::NoBloomFilter<config, RedistributionPolicy, PartitionPolicy>;
            run_sorter(
                BloomFilterPolicy{dss_mehnert::init_partition_policy<CharType, PartitionPolicy>(
                                      args.sampler,
                                      args.get_splitter_sorter()),
                                  std::move(redistribution)});
        }
    };

    using AugmentedStringSet = dss_mehnert::sorter::AugmentedStringSet<StringSet, Permutation>;
    dss_mehnert::dispatch_redistribution<AugmentedStringSet>(dispatch, args);
}

template <typename CharType, typename... Args>
void dispatch_permutation(SorterArgs const& args) {
    using namespace dss_mehnert;

    static_assert(!CliOptions::use_shared_memory_sort);

    dss_mehnert::Communicator comm;
    auto prefix = args.get_prefix(comm);

    switch (clamp_enum_value<Permutation>(args.permutation)) {
        case Permutation::simple: {
            using Permutation_ = SimplePermutation;
            run_space_efficient_sort<CharType, Args..., Permutation_>(args, prefix, comm);
            return;
        }
        case Permutation::multi_level: {
            using Permutation_ = MultiLevelPermutation;
            run_space_efficient_sort<CharType, Args..., Permutation_>(args, prefix, comm);
            return;
        }
        case Permutation::non_unique: {
            using Permutation_ = NonUniquePermutation;
            run_space_efficient_sort<CharType, Args..., Permutation_>(args, prefix, comm);
            return;
        }
        case Permutation::sentinel: {
            break;
        }
    }
    tlx_die("invalid permutation");
}

int main(int argc, char* argv[]) {
    SorterArgs args;

    tlx::CmdlineParser cp;
    cp.set_description("a space efficient distributed string sorter");
    cp.set_author("Pascal Mehnert");

    add_common_args(args, cp);

    bool use_quantile_sampler = false;
    cp.add_flag("use-quantile-sampler",
                use_quantile_sampler,
                "use separate quantile sampling policy");
    cp.add_flag("quantile-chars",
                args.quantile_sampler.sample_chars,
                "use character based sampling for quantiles");
    cp.add_flag("quantile-indexed",
                args.quantile_sampler.sample_indexed,
                "use indexed sampling for quantiles");
    cp.add_flag("quantile-random",
                args.quantile_sampler.sample_random,
                "use random sampling for quantiles");
    cp.add_size_t("quantile-factor",
                  args.quantile_sampler.sampling_factor,
                  "use the given oversampling factor for quantiles");

    cp.add_size_t('b',
                  "combined-generator",
                  args.combined_gen,
                  "combined char/string generator to use"
                  "([0]=none, 1=dn-ratio)");
    cp.add_size_t('c',
                  "char-generator",
                  args.char_gen,
                  "char generator to use "
                  "([0]=random, 1=file, 2=file-segment)");
    cp.add_size_t('s',
                  "string-generator",
                  args.string_gen,
                  "string generator to use "
                  "([0]=suffix, 1=window, 2=difference_cover)");
    cp.add_flag("use-proper-dc", args.use_proper_dc, "use proper difference cover strings");
    cp.add_size_t('n', "num-strings", args.num_strings, "number of strings per PE");
    cp.add_size_t('m', "len-strings", args.len_strings, "number of characters per string");
    cp.add_bytes('N', "num-chars", args.num_chars, "number of chars per rank");
    cp.add_double('r', "dn-ratio", args.dn_ratio, "D/N ratio of generated strings");
    cp.add_size_t('T', "step", args.step, "characters to skip between strings");
    cp.add_size_t('D', "difference-cover", args.difference_cover, "size of difference cover");
    cp.add_flag("shuffle", args.shuffle, "shuffle the generated strings");
    cp.add_string('y', "path", args.path, "path to input file");
    cp.add_size_t('o',
                  "permutation",
                  args.permutation,
                  "type of permutation to use for SEMS"
                  "(0=simple, [1]=multi-level, 2=non-unique)");
    cp.add_bytes('q',
                 "quantile-size",
                 args.quantile_size,
                 "work on quantiles of the given size [default: 100MiB]");

    std::vector<std::string> levels_param;
    cp.add_opt_param_stringlist("group-size",
                                levels_param,
                                "size of groups for multi-level merge sort");

    if (!cp.process(argc, argv)) {
        return EXIT_FAILURE;
    }

    if (!use_quantile_sampler) {
        args.quantile_sampler = args.sampler;
    }
    parse_level_arg(levels_param, args.levels);

    kamping::Environment env{argc, argv};

    if constexpr (CliOptions::use_shared_memory_sort) {
        using CharType = unsigned char;
        using StringSet = dss_mehnert::CompressedStringSet<CharType, dss_mehnert::Length>;
        run_shared_memory(args, kamping::comm_world(), generate_compressed_strings<StringSet>);
    } else {
        for (size_t i = 0; i < args.num_iterations; ++i) {
            args.iteration = i;
            dispatch_common_args(
                [&]<typename... T>() { dispatch_permutation<T...>(args); },
                args);
        }
    }
    return EXIT_SUCCESS;
}
