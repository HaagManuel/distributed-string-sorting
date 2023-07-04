// (c) 2019 Matthias Schimek
// (c) 2023 Pascal Mehnert
// This code is licensed under BSD 2-Clause License (see LICENSE for details)

#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

#include "mpi/communicator.hpp"
#include "sorter/RQuick/RQuick.hpp"
#include "sorter/distributed/misc.hpp"
#include "strings/stringcontainer.hpp"
#include "util/measuringTool.hpp"

namespace dss_mehnert {
namespace partition {

template <typename StringPtr, typename Sampler>
static std::vector<uint64_t> compute_partition(
    StringPtr string_ptr,
    uint64_t global_lcp_avg,
    uint64_t num_partitions,
    uint64_t sampling_factor,
    Communicator const& comm
) {
    using namespace dss_schimek;

    using Comparator = dss_schimek::StringComparator;
    using StringContainer = dss_schimek::StringContainer<UCharLengthStringSet>;
    using StringSet = typename StringPtr::StringSet;

    auto measuring_tool = measurement::MeasuringTool::measuringTool();
    auto ss = string_ptr.active();

    measuring_tool.start("sample_splitters");
    std::vector<unsigned char> raw_splitters =
        Sampler::sample_splitters(ss, 2 * global_lcp_avg, num_partitions, sampling_factor, comm);
    measuring_tool.stop("sample_splitters");

    measuring_tool.start("sort_splitter");
    Comparator comp;
    std::mt19937_64 generator{3469931 + comm.rank()};
    RQuick::Data<StringContainer, StringContainer::isIndexed> sample_data;
    sample_data.rawStrings = std::move(raw_splitters); // TODO
    measuring_tool.disable();
    StringContainer sortedLocalSample = splitterSort(std::move(sample_data), generator, comp);
    measuring_tool.enable();
    measuring_tool.stop("sort_splitter");

    measuring_tool.start("choose_splitters");
    auto rawChosenSplitters = getSplitters(sortedLocalSample, num_partitions, comm);
    StringContainer chosen_splitters_cont(std::move(rawChosenSplitters));
    measuring_tool.stop("choose_splitters");

    const StringSet chosen_splitters_set(
        chosen_splitters_cont.strings(),
        chosen_splitters_cont.strings() + chosen_splitters_cont.size()
    );

    measuring_tool.start("compute_interval_sizes");
    std::vector<std::size_t> interval_sizes =
        compute_interval_binary(ss, chosen_splitters_set, comm);
    measuring_tool.stop("compute_interval_sizes");

    return interval_sizes;
}

} // namespace partition
} // namespace dss_mehnert
