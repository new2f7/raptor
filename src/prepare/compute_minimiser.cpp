// SPDX-FileCopyrightText: 2006-2024 Knut Reinert & Freie Universität Berlin
// SPDX-FileCopyrightText: 2016-2024 Knut Reinert & MPI für molekulare Genetik
// SPDX-License-Identifier: BSD-3-Clause

/*!\file
 * \brief Implements raptor::compute_minimiser.
 * \author Enrico Seiler <enrico.seiler AT fu-berlin.de>
 */

#include <omp.h>

#include <seqan3/io/sequence_file/input.hpp>
#include <seqan3/search/views/minimiser_hash.hpp>

#include <hibf/contrib/robin_hood.hpp>
#include <hibf/contrib/std/zip_view.hpp>
#include <hibf/misc/divide_and_ceil.hpp>

#include <raptor/adjust_seed.hpp>
#include <raptor/dna4_traits.hpp>
#include <raptor/file_reader.hpp>
#include <raptor/prepare/compute_minimiser.hpp>
#include <raptor/prepare/cutoff.hpp>

namespace raptor
{

std::filesystem::path get_output_path(std::filesystem::path const & output_dir, std::filesystem::path const & file_name)
{
    std::filesystem::path result{output_dir};
    bool const is_compressed = raptor::cutoff::file_is_compressed(file_name);
    result /= is_compressed ? file_name.stem().stem() : file_name.stem();
    result += ".dummy_extension"; // https://github.com/seqan/raptor/issues/355
    return result;
}

void write_list_file(prepare_arguments const & arguments)
{
    std::filesystem::path list_file = arguments.out_dir;
    list_file /= "minimiser.list";
    std::ofstream file{list_file};

    for (auto && file_names : arguments.bin_path)
    {
        std::filesystem::path file_path = get_output_path(arguments.out_dir, file_names[0]);
        file_path.replace_extension("minimiser");
        file << file_path.c_str() << '\n';
    }
}

void compute_minimiser(prepare_arguments const & arguments)
{
    file_reader<file_types::sequence> const reader{arguments.shape, arguments.window_size};
    raptor::cutoff const cutoffs{arguments};

    seqan::hibf::serial_timer local_compute_minimiser_timer{};
    seqan::hibf::serial_timer local_write_minimiser_timer{};
    seqan::hibf::serial_timer local_write_header_timer{};

#pragma omp parallel for schedule(guided) num_threads(arguments.threads)
    for (size_t i = 0; i < arguments.bin_path.size(); ++i)
    {
        auto file_names = arguments.bin_path[i];

        std::filesystem::path const file_name{file_names[0]};
        std::filesystem::path output_path = get_output_path(arguments.out_dir, file_name);

        std::filesystem::path const minimiser_file = std::filesystem::path{output_path}.replace_extension("minimiser");
        std::filesystem::path const progress_file = std::filesystem::path{output_path}.replace_extension("in_progress");
        std::filesystem::path const header_file = std::filesystem::path{output_path}.replace_extension("header");

        // If we are already done with this file, we can skip it. Otherwise, we create a ".in_progress" file to keep
        // track of whether the minimiser computation was successful.
        bool const already_done = std::filesystem::exists(minimiser_file) && std::filesystem::exists(header_file)
                               && !std::filesystem::exists(progress_file);

        if (already_done)
            continue;
        else
            std::ofstream outfile{progress_file, std::ios::binary};

        // The hash table stores how often a minimiser appears. It does not matter whether a minimiser appears
        // 50 times or 2000 times, it is stored regardless because the biggest cutoff value is 50. Hence,
        // the hash table stores only values up to 254 to save memory.
        robin_hood::unordered_map<uint64_t, uint8_t> minimiser_table{};
        // The map is (re-)constructed for each file. The alternative is to construct it once for each thread
        // and clear+reuse it for every file that a thread works on. However, this dramatically increases
        // memory consumption because the map will stay as big as needed for the biggest encountered file.

        local_compute_minimiser_timer.start();
        reader.for_each_hash(file_names,
                             [&](auto && hash)
                             {
                                 minimiser_table[hash] = std::min<uint8_t>(254u, minimiser_table[hash] + 1);
                             });
        local_compute_minimiser_timer.stop();

        uint8_t const cutoff = cutoffs.get(file_name);
        uint64_t count{};

        local_write_minimiser_timer.start();
        {
            std::ofstream outfile{minimiser_file, std::ios::binary};
            for (auto && [hash, occurrences] : minimiser_table)
            {
                if (occurrences >= cutoff)
                {
                    outfile.write(reinterpret_cast<char const *>(&hash), sizeof(hash));
                    ++count;
                }
            }
        }
        local_write_minimiser_timer.stop();

        local_write_header_timer.start();
        {
            std::ofstream headerfile{header_file};
            headerfile << arguments.shape.to_string() << '\t' << arguments.window_size << '\t'
                       << static_cast<uint16_t>(cutoff) << '\t' << count << '\n';
        }
        local_write_header_timer.stop();

        std::filesystem::remove(progress_file);
    }

    arguments.compute_minimiser_timer += local_compute_minimiser_timer;
    arguments.write_minimiser_timer += local_write_minimiser_timer;
    arguments.write_header_timer += local_write_header_timer;

    write_list_file(arguments);
}

} // namespace raptor
