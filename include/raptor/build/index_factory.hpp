// SPDX-FileCopyrightText: 2006-2024 Knut Reinert & Freie Universität Berlin
// SPDX-FileCopyrightText: 2016-2024 Knut Reinert & MPI für molekulare Genetik
// SPDX-License-Identifier: BSD-3-Clause

/*!\file
 * \brief Provides raptor::index_factory.
 * \author Enrico Seiler <enrico.seiler AT fu-berlin.de>
 */

#pragma once

#include <seqan3/search/views/minimiser_hash.hpp>

#include <raptor/adjust_seed.hpp>
#include <raptor/build/emplace_iterator.hpp>
#include <raptor/build/partition_config.hpp>
#include <raptor/call_parallel_on_bins.hpp>
#include <raptor/dna4_traits.hpp>
#include <raptor/file_reader.hpp>
#include <raptor/index.hpp>

namespace raptor
{

class index_factory
{
public:
    index_factory() = default;
    index_factory(index_factory const &) = default;
    index_factory(index_factory &&) = default;
    index_factory & operator=(index_factory const &) = delete; // const member
    index_factory & operator=(index_factory &&) = delete;      // const member
    ~index_factory() = default;

    explicit index_factory(build_arguments const & args) : arguments{std::addressof(args)}
    {
        if (arguments->input_is_minimiser)
            reader = file_reader<file_types::minimiser>{};
        else
            reader = file_reader<file_types::sequence>{arguments->shape, arguments->window_size};
    }

    explicit index_factory(build_arguments const & args, partition_config const & cfg) :
        arguments{std::addressof(args)},
        config{std::addressof(cfg)}
    {
        if (arguments->input_is_minimiser)
            reader = file_reader<file_types::minimiser>{}; // GCOVR_EXCL_LINE
        else
            reader = file_reader<file_types::sequence>{arguments->shape, arguments->window_size};
    }

    [[nodiscard]] raptor_index<> operator()(size_t const part = 0u) const
    {
        return construct(part);
    }

private:
    build_arguments const * const arguments{nullptr};
    partition_config const * const config{nullptr};
    std::variant<file_reader<file_types::sequence>, file_reader<file_types::minimiser>> reader;

    raptor_index<> construct(size_t const part) const
    {
        assert(arguments != nullptr);

        arguments->index_allocation_timer.start();
        raptor_index<> index{*arguments};
        arguments->index_allocation_timer.stop();

        auto worker = [&](auto && zipped)
        {
            seqan::hibf::serial_timer local_timer{};
            auto & ibf = index.ibf();
            local_timer.start();
            // https://godbolt.org/z/PeKnxzjn1
            //{
                std::visit(
                    [&](auto const & reader)
                    {
                        auto && [file_names, bin_number] = zipped;

                        if (config == nullptr)
                            reader.hash_into(file_names, emplacer(ibf, seqan::hibf::bin_index{bin_number}));
                        else
                            reader.hash_into_if(file_names,
                                                emplacer(ibf, seqan::hibf::bin_index{bin_number}),
                                                [&](uint64_t const hash)
                                                {
                                                    return config->hash_partition(hash) == part;
                                                });
                    },
                    reader);
            //}
            local_timer.stop();
            arguments->user_bin_io_timer += local_timer;
            arguments->fill_ibf_timer += local_timer;
        };

        call_parallel_on_bins(worker, arguments->bin_path, arguments->threads);

        return index;
    }
};

} // namespace raptor
