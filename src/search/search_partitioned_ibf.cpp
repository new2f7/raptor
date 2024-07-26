// SPDX-FileCopyrightText: 2006-2024 Knut Reinert & Freie Universität Berlin
// SPDX-FileCopyrightText: 2016-2024 Knut Reinert & MPI für molekulare Genetik
// SPDX-License-Identifier: BSD-3-Clause

/*!\file
 * \brief Implements raptor::search_partitioned_ibf.
 * \author Enrico Seiler <enrico.seiler AT fu-berlin.de>
 */

#include <future>
#include <random>

#include <seqan3/search/views/minimiser_hash.hpp>

#include <hibf/contrib/std/chunk_view.hpp>

#include <raptor/adjust_seed.hpp>
#include <raptor/build/partition_config.hpp>
#include <raptor/dna4_traits.hpp>
#include <raptor/search/load_index.hpp>
#include <raptor/search/search_partitioned_ibf.hpp>
#include <raptor/search/sync_out.hpp>
#include <raptor/threshold/threshold.hpp>

namespace raptor
{

void search_partitioned_ibf(search_arguments const & arguments)
{
    auto index = raptor_index<index_structure::ibf>{};
    partition_config const cfg{arguments.parts};

    seqan3::sequence_file_input<dna4_traits, seqan3::fields<seqan3::field::id, seqan3::field::seq>> fin{
        arguments.query_file};
    using record_type = typename decltype(fin)::record_type;
    std::vector<record_type> records{};

    sync_out synced_out{arguments};

    auto write_header = [&]()
    {
        return synced_out.write_header(arguments, index.ibf().hash_function_count());
    };

    raptor::threshold::threshold const thresholder{arguments.make_threshold_parameters()};

    for (auto && chunked_records : fin | seqan::stl::views::chunk((1ULL << 20) * 10))
    {
        auto cereal_future = std::async(std::launch::async,
                                        [&]() // GCOVR_EXCL_LINE
                                        {
                                            load_index(index, arguments, 0);
                                        });

        records.clear();
        arguments.query_file_io_timer.start();
        std::ranges::move(chunked_records, std::back_inserter(records));
        // Very fast, improves parallel processing when chunks of the query belong to the same bin.
        std::ranges::shuffle(records, std::mt19937_64{0u});
        arguments.query_file_io_timer.stop();

        cereal_future.get();
        [[maybe_unused]] static bool header_written = write_header(); // called exactly once

        std::vector<seqan::hibf::counting_vector<uint16_t>> counts(
            records.size(),
            seqan::hibf::counting_vector<uint16_t>(index.ibf().bin_count(), 0));

        size_t part{};

        auto count_task = [&]()
        {
            seqan::hibf::serial_timer local_compute_minimiser_timer{};
            seqan::hibf::serial_timer local_query_ibf_timer{};

            auto & ibf = index.ibf();
            auto counter = ibf.template counting_agent<uint16_t>();
            size_t counter_id = 0;
            std::vector<uint64_t> minimiser;

            auto hash_view = seqan3::views::minimiser_hash(arguments.shape,
                                                           seqan3::window_size{arguments.window_size},
                                                           seqan3::seed{adjust_seed(arguments.shape_weight)});

#pragma omp parallel for schedule(guided) num_threads(arguments.threads)
            for (size_t i = 0; i < records.size(); ++i)
            {
                auto && [id, seq] = records[i];

                auto minimiser_view = seq | hash_view | std::views::common;
                local_compute_minimiser_timer.start();
                minimiser.assign(minimiser_view.begin(), minimiser_view.end());
                local_compute_minimiser_timer.stop();

                // GCOVR_EXCL_START
                auto filtered = minimiser
                              | std::views::filter(
                                    [&](auto && hash)
                                    {
                                        return cfg.hash_partition(hash) == part;
                                    });
                // GCOVR_EXCL_STOP

                local_query_ibf_timer.start();
                counts[counter_id++] += counter.bulk_count(filtered);
                local_query_ibf_timer.stop();
            }

            arguments.compute_minimiser_timer += local_compute_minimiser_timer;
            arguments.query_ibf_timer += local_query_ibf_timer;
        };

        arguments.parallel_search_timer.start();
        count_task();
        arguments.parallel_search_timer.stop();
        ++part;

        for (; part < arguments.parts - 1u; ++part)
        {
            load_index(index, arguments, part);
            arguments.parallel_search_timer.start();
            count_task();
            arguments.parallel_search_timer.stop();
        }

        assert(part == arguments.parts - 1u);
        load_index(index, arguments, part);

        auto output_task = [&]()
        {
            seqan::hibf::serial_timer local_compute_minimiser_timer{};
            seqan::hibf::serial_timer local_query_ibf_timer{};
            seqan::hibf::serial_timer local_generate_results_timer{};

            auto & ibf = index.ibf();
            auto counter = ibf.template counting_agent<uint16_t>();
            size_t counter_id = 0;
            std::string result_string{};
            std::vector<uint64_t> minimiser;

            auto hash_adaptor = seqan3::views::minimiser_hash(arguments.shape,
                                                              seqan3::window_size{arguments.window_size},
                                                              seqan3::seed{adjust_seed(arguments.shape_weight)});

#pragma omp parallel for schedule(guided) num_threads(arguments.threads)
            for (size_t i = 0; i < records.size(); ++i)
            {
                auto && [id, seq] = records[i];

                result_string.clear();
                result_string += id;
                result_string += '\t';

                auto minimiser_view = seq | hash_adaptor | std::views::common;
                local_compute_minimiser_timer.start();
                minimiser.assign(minimiser_view.begin(), minimiser_view.end());
                local_compute_minimiser_timer.stop();

                // GCOVR_EXCL_START
                auto filtered = minimiser
                              | std::views::filter(
                                    [&](auto && hash)
                                    {
                                        return cfg.hash_partition(hash) == part;
                                    });
                // GCOVR_EXCL_STOP
                local_query_ibf_timer.start();
                counts[counter_id] += counter.bulk_count(filtered);
                local_query_ibf_timer.stop();

                size_t const minimiser_count{minimiser.size()};
                size_t current_bin{0};

                size_t const threshold = thresholder.get(minimiser_count);
                local_generate_results_timer.start();
                for (auto && count : counts[counter_id++])
                {
                    if (count >= threshold)
                    {
                        result_string += std::to_string(current_bin);
                        result_string += ',';
                    }
                    ++current_bin;
                }
                if (auto & last_char = result_string.back(); last_char == ',')
                    last_char = '\n';
                else
                    result_string += '\n';

                synced_out.write(result_string);
                local_generate_results_timer.stop();
            }

            arguments.compute_minimiser_timer += local_compute_minimiser_timer;
            arguments.query_ibf_timer += local_query_ibf_timer;
            arguments.generate_results_timer += local_generate_results_timer;
        };

        arguments.parallel_search_timer.start();
        output_task();
        arguments.parallel_search_timer.stop();
    }
}

} // namespace raptor
