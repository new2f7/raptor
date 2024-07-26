// SPDX-FileCopyrightText: 2006-2024 Knut Reinert & Freie Universität Berlin
// SPDX-FileCopyrightText: 2016-2024 Knut Reinert & MPI für molekulare Genetik
// SPDX-License-Identifier: BSD-3-Clause

/*!\file
 * \brief Provides raptor::call_parallel_on_bins.
 * \author Enrico Seiler <enrico.seiler AT fu-berlin.de>
 */

#pragma once

#include <algorithm>
#include <bit>
#include <functional>
#include <omp.h>
#include <vector>

#include <hibf/contrib/std/zip_view.hpp>
#include <hibf/misc/divide_and_ceil.hpp>

namespace raptor
{

template <typename algorithm_t>
void call_parallel_on_bins(algorithm_t && worker,
                           std::vector<std::vector<std::string>> const & bin_paths,
                           uint8_t const threads)
{
    size_t const number_of_bins = bin_paths.size();

    auto zipped_view = seqan::stl::views::zip(bin_paths, std::views::iota(0u, number_of_bins));

#pragma omp parallel for schedule(guided) num_threads(threads)
    for (size_t i = 0; i < number_of_bins; ++i)
    {
        std::invoke(worker, zipped_view[i]);
    }
}

} // namespace raptor
