/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>

#include "Enums.h"
#include "LookupTables.h"
#include "Parser.h"
#include "TreeParser.h"

namespace Video::VP9 {

template<typename T>
ErrorOr<T> TreeParser::parse_tree(SyntaxElementType type)
{
    auto tree_selection = select_tree(type);
    int value;
    if (tree_selection.is_single_value()) {
        value = tree_selection.single_value();
    } else {
        auto tree = tree_selection.tree();
        int n = 0;
        do {
            n = tree[n + TRY(m_decoder.m_bit_stream->read_bool(select_tree_probability(type, n >> 1)))];
        } while (n > 0);
        value = -n;
    }
    count_syntax_element(type, value);
    return static_cast<T>(value);
}

template ErrorOr<int> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<bool> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<u8> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<u32> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<PredictionMode> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<TXSize> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<InterpolationFilter> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<ReferenceMode> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<Token> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<MvClass> TreeParser::parse_tree(SyntaxElementType);
template ErrorOr<MvJoint> TreeParser::parse_tree(SyntaxElementType);

template<typename OutputType>
inline ErrorOr<OutputType> parse_tree_new(BitStream& bit_stream, TreeParser::TreeSelection tree_selection, Function<u8(u8)> const& probability_getter)
{
    if (tree_selection.is_single_value())
        return static_cast<OutputType>(tree_selection.single_value());

    int const* tree = tree_selection.tree();
    int n = 0;
    do {
        u8 node = n >> 1;
        n = tree[n + TRY(bit_stream.read_bool(probability_getter(node)))];
    } while (n > 0);

    return static_cast<OutputType>(-n);
}

inline void increment_counter(u8& counter)
{
    counter = min(static_cast<u32>(counter) + 1, 255);
}

ErrorOr<Partition> TreeParser::parse_partition(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, bool has_rows, bool has_columns, BlockSubsize block_subsize, u8 num_8x8, Vector<u8> const& above_partition_context, Vector<u8> const& left_partition_context, u32 row, u32 column, bool frame_is_intra)
{
    // Tree array
    TreeParser::TreeSelection tree = { PartitionSplit };
    if (has_rows && has_columns)
        tree = { partition_tree };
    else if (has_rows)
        tree = { rows_partition_tree };
    else if (has_columns)
        tree = { cols_partition_tree };

    // Probability array
    u32 above = 0;
    u32 left = 0;
    auto bsl = mi_width_log2_lookup[block_subsize];
    auto block_offset = mi_width_log2_lookup[Block_64x64] - bsl;
    for (auto i = 0; i < num_8x8; i++) {
        above |= above_partition_context[column + i];
        left |= left_partition_context[row + i];
    }
    above = (above & (1 << block_offset)) > 0;
    left = (left & (1 << block_offset)) > 0;
    auto context = bsl * 4 + left * 2 + above;
    u8 const* probabilities = frame_is_intra ? probability_table.kf_partition_probs()[context] : probability_table.partition_probs()[context];

    Function<u8(u8)> probability_getter = [&](u8 node) {
        if (has_rows && has_columns)
            return probabilities[node];
        if (has_columns)
            return probabilities[1];
        return probabilities[2];
    };

    auto value = TRY(parse_tree_new<Partition>(bit_stream, tree, probability_getter));
    increment_counter(counter.m_counts_partition[context][value]);
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_default_intra_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, BlockSubsize mi_size, Optional<Array<PredictionMode, 4> const&> above_context, Optional<Array<PredictionMode, 4> const&> left_context, PredictionMode block_sub_modes[4], u8 index_x, u8 index_y)
{
    // FIXME: This should use a struct for the above and left contexts.

    // Tree
    TreeParser::TreeSelection tree = { intra_mode_tree };

    // Probabilities
    PredictionMode above_mode, left_mode;
    if (mi_size >= Block_8x8) {
        above_mode = above_context.has_value() ? above_context.value()[2] : PredictionMode::DcPred;
        left_mode = left_context.has_value() ? left_context.value()[1] : PredictionMode::DcPred;
    } else {
        if (index_y > 0)
            above_mode = block_sub_modes[index_x];
        else
            above_mode = above_context.has_value() ? above_context.value()[2 + index_x] : PredictionMode::DcPred;

        if (index_x > 0)
            left_mode = block_sub_modes[index_y << 1];
        else
            left_mode = left_context.has_value() ? left_context.value()[1 + (index_y << 1)] : PredictionMode::DcPred;
    }
    u8 const* probabilities = probability_table.kf_y_mode_probs()[to_underlying(above_mode)][to_underlying(left_mode)];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    // Default intra mode is not counted.
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_default_uv_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, PredictionMode y_mode)
{
    // Tree
    TreeParser::TreeSelection tree = { intra_mode_tree };

    // Probabilities
    u8 const* probabilities = probability_table.kf_uv_mode_prob()[to_underlying(y_mode)];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    // Default UV mode is not counted.
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_intra_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, BlockSubsize mi_size)
{
    // Tree
    TreeParser::TreeSelection tree = { intra_mode_tree };

    // Probabilities
    auto context = size_group_lookup[mi_size];
    u8 const* probabilities = probability_table.y_mode_probs()[context];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_intra_mode[context][to_underlying(value)]);
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_sub_intra_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter)
{
    // Tree
    TreeParser::TreeSelection tree = { intra_mode_tree };

    // Probabilities
    u8 const* probabilities = probability_table.y_mode_probs()[0];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_intra_mode[0][to_underlying(value)]);
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_uv_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, PredictionMode y_mode)
{
    // Tree
    TreeParser::TreeSelection tree = { intra_mode_tree };

    // Probabilities
    u8 const* probabilities = probability_table.uv_mode_probs()[to_underlying(y_mode)];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_uv_mode[to_underlying(y_mode)][to_underlying(value)]);
    return value;
}

ErrorOr<u8> TreeParser::parse_segment_id(BitStream& bit_stream, u8 const probabilities[7])
{
    auto value = TRY(parse_tree_new<u8>(bit_stream, { segment_tree }, [&](u8 node) { return probabilities[node]; }));
    // Segment ID is not counted.
    return value;
}

ErrorOr<bool> TreeParser::parse_segment_id_predicted(BitStream& bit_stream, u8 const probabilities[3], u8 above_seg_pred_context, u8 left_seg_pred_context)
{
    auto context = left_seg_pred_context + above_seg_pred_context;
    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probabilities[context]; }));
    // Segment ID prediction is not counted.
    return value;
}

ErrorOr<PredictionMode> TreeParser::parse_inter_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, u8 mode_context_for_ref_frame_0)
{
    // Tree
    TreeParser::TreeSelection tree = { inter_mode_tree };

    // Probabilities
    u8 const* probabilities = probability_table.inter_mode_probs()[mode_context_for_ref_frame_0];

    auto value = TRY(parse_tree_new<PredictionMode>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_inter_mode[mode_context_for_ref_frame_0][to_underlying(value) - to_underlying(PredictionMode::NearestMv)]);
    return value;
}

ErrorOr<InterpolationFilter> TreeParser::parse_interpolation_filter(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, Optional<ReferenceFrameType> above_ref_frame, Optional<ReferenceFrameType> left_ref_frame, Optional<InterpolationFilter> above_interpolation_filter, Optional<InterpolationFilter> left_interpolation_filter)
{
    // FIXME: Above and left context should be provided by a struct.

    // Tree
    TreeParser::TreeSelection tree = { interp_filter_tree };

    // Probabilities
    // NOTE: SWITCHABLE_FILTERS is not used in the spec for this function. Therefore, the number
    //       was demystified by referencing the reference codec libvpx:
    //       https://github.com/webmproject/libvpx/blob/705bf9de8c96cfe5301451f1d7e5c90a41c64e5f/vp9/common/vp9_pred_common.h#L69
    u8 left_interp = (left_ref_frame.has_value() && left_ref_frame.value() > IntraFrame)
        ? left_interpolation_filter.value()
        : SWITCHABLE_FILTERS;
    u8 above_interp = (above_ref_frame.has_value() && above_ref_frame.value() > IntraFrame)
        ? above_interpolation_filter.value()
        : SWITCHABLE_FILTERS;
    u8 context = SWITCHABLE_FILTERS;
    if (above_interp == left_interp || above_interp == SWITCHABLE_FILTERS)
        context = left_interp;
    else if (left_interp == SWITCHABLE_FILTERS)
        context = above_interp;
    u8 const* probabilities = probability_table.interp_filter_probs()[context];

    auto value = TRY(parse_tree_new<InterpolationFilter>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_interp_filter[context][to_underlying(value)]);
    return value;
}

ErrorOr<bool> TreeParser::parse_skip(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, Optional<bool> const& above_skip, Optional<bool> const& left_skip)
{
    // Probabilities
    u8 context = 0;
    context += static_cast<u8>(above_skip.value_or(false));
    context += static_cast<u8>(left_skip.value_or(false));
    u8 probability = probability_table.skip_prob()[context];

    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_skip[context][value]);
    return value;
}

ErrorOr<TXSize> TreeParser::parse_tx_size(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, TXSize max_tx_size, Optional<bool> above_skip, Optional<bool> left_skip, Optional<TXSize> above_tx_size, Optional<TXSize> left_tx_size)
{
    // FIXME: Above and left contexts should be in structs.

    // Tree
    TreeParser::TreeSelection tree { tx_size_8_tree };
    if (max_tx_size == TX_16x16)
        tree = { tx_size_16_tree };
    if (max_tx_size == TX_32x32)
        tree = { tx_size_32_tree };

    // Probabilities
    auto above = max_tx_size;
    auto left = max_tx_size;
    if (above_skip.has_value() && !above_skip.value()) {
        above = above_tx_size.value();
    }
    if (left_skip.has_value() && !left_skip.value()) {
        left = left_tx_size.value();
    }
    if (!left_skip.has_value())
        left = above;
    if (!above_skip.has_value())
        above = left;
    auto context = (above + left) > max_tx_size;
    u8 const* probabilities = probability_table.tx_probs()[max_tx_size][context];

    auto value = TRY(parse_tree_new<TXSize>(bit_stream, tree, [&](u8 node) { return probabilities[node]; }));
    increment_counter(counter.m_counts_tx_size[max_tx_size][context][value]);
    return value;
}

ErrorOr<bool> TreeParser::parse_is_inter(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, Optional<bool> above_intra, Optional<bool> left_intra)
{
    // FIXME: Above and left contexts should be in structs.

    // Probabilities
    u8 context = 0;
    if (above_intra.has_value() && left_intra.has_value())
        context = (left_intra.value() && above_intra.value()) ? 3 : static_cast<u8>(above_intra.value() || left_intra.value());
    else if (above_intra.has_value() || left_intra.has_value())
        context = 2 * static_cast<u8>(above_intra.has_value() ? above_intra.value() : left_intra.value());
    u8 probability = probability_table.is_inter_prob()[context];

    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_is_inter[context][value]);
    return value;
}

ErrorOr<ReferenceMode> TreeParser::parse_comp_mode(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, ReferenceFrameType comp_fixed_ref, Optional<bool> above_single, Optional<bool> left_single, Optional<bool> above_intra, Optional<bool> left_intra, Optional<ReferenceFrameType> above_ref_frame_0, Optional<ReferenceFrameType> left_ref_frame_0)
{
    // FIXME: Above and left contexts should be in structs.

    // Probabilities
    u8 context;
    if (above_single.has_value() && left_single.has_value()) {
        if (above_single.value() && left_single.value()) {
            auto is_above_fixed = above_ref_frame_0.value() == comp_fixed_ref;
            auto is_left_fixed = left_ref_frame_0.value() == comp_fixed_ref;
            context = is_above_fixed ^ is_left_fixed;
        } else if (above_single.value()) {
            auto is_above_fixed = above_ref_frame_0.value() == comp_fixed_ref;
            context = 2 + static_cast<u8>(is_above_fixed || above_intra.value());
        } else if (left_single.value()) {
            auto is_left_fixed = left_ref_frame_0.value() == comp_fixed_ref;
            context = 2 + static_cast<u8>(is_left_fixed || left_intra.value());
        } else {
            context = 4;
        }
    } else if (above_single.has_value()) {
        if (above_single.value())
            context = above_ref_frame_0.value() == comp_fixed_ref;
        else
            context = 3;
    } else if (left_single.has_value()) {
        if (left_single.value())
            context = static_cast<u8>(left_ref_frame_0.value() == comp_fixed_ref);
        else
            context = 3;
    } else {
        context = 1;
    }
    u8 probability = probability_table.comp_mode_prob()[context];

    auto value = TRY(parse_tree_new<ReferenceMode>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_comp_mode[context][value]);
    return value;
}

ErrorOr<bool> TreeParser::parse_comp_ref(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, ReferenceFrameType comp_fixed_ref, ReferenceFramePair comp_var_ref, Optional<bool> above_single, Optional<bool> left_single, Optional<bool> above_intra, Optional<bool> left_intra, Optional<ReferenceFrameType> above_ref_frame_0, Optional<ReferenceFrameType> left_ref_frame_0, Optional<ReferenceFrameType> above_ref_frame_biased, Optional<ReferenceFrameType> left_ref_frame_biased)
{
    // FIXME: Above and left contexts should be in structs.

    // Probabilities
    u8 context;
    if (above_intra.has_value() && left_intra.has_value()) {
        if (above_intra.value() && left_intra.value()) {
            context = 2;
        } else if (left_intra.value()) {
            if (above_single.value()) {
                context = 1 + 2 * (above_ref_frame_0.value() != comp_var_ref[1]);
            } else {
                context = 1 + 2 * (above_ref_frame_biased.value() != comp_var_ref[1]);
            }
        } else if (above_intra.value()) {
            if (left_single.value()) {
                context = 1 + 2 * (left_ref_frame_0.value() != comp_var_ref[1]);
            } else {
                context = 1 + 2 * (left_ref_frame_biased != comp_var_ref[1]);
            }
        } else {
            auto var_ref_above = above_single.value() ? above_ref_frame_0 : above_ref_frame_biased;
            auto var_ref_left = left_single.value() ? left_ref_frame_0 : left_ref_frame_biased;
            if (var_ref_above == var_ref_left && comp_var_ref[1] == var_ref_above) {
                context = 0;
            } else if (left_single.value() && above_single.value()) {
                if ((var_ref_above == comp_fixed_ref && var_ref_left == comp_var_ref[0])
                    || (var_ref_left == comp_fixed_ref && var_ref_above == comp_var_ref[0])) {
                    context = 4;
                } else if (var_ref_above == var_ref_left) {
                    context = 3;
                } else {
                    context = 1;
                }
            } else if (left_single.value() || above_single.value()) {
                auto vrfc = left_single.value() ? var_ref_above : var_ref_left;
                auto rfs = above_single.value() ? var_ref_above : var_ref_left;
                if (vrfc == comp_var_ref[1] && rfs != comp_var_ref[1]) {
                    context = 1;
                } else if (rfs == comp_var_ref[1] && vrfc != comp_var_ref[1]) {
                    context = 2;
                } else {
                    context = 4;
                }
            } else if (var_ref_above == var_ref_left) {
                context = 4;
            } else {
                context = 2;
            }
        }
    } else if (above_intra.has_value()) {
        if (above_intra.value()) {
            context = 2;
        } else {
            if (above_single.value()) {
                context = 3 * static_cast<u8>(above_ref_frame_0.value() != comp_var_ref[1]);
            } else {
                context = 4 * static_cast<u8>(above_ref_frame_biased.value() != comp_var_ref[1]);
            }
        }
    } else if (left_intra.has_value()) {
        if (left_intra.value()) {
            context = 2;
        } else {
            if (left_single.value()) {
                context = 3 * static_cast<u8>(left_ref_frame_0.value() != comp_var_ref[1]);
            } else {
                context = 4 * static_cast<u8>(left_ref_frame_biased != comp_var_ref[1]);
            }
        }
    } else {
        context = 2;
    }

    u8 probability = probability_table.comp_ref_prob()[context];

    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_comp_ref[context][value]);
    return value;
}

ErrorOr<bool> TreeParser::parse_single_ref_part_1(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, Optional<bool> above_single, Optional<bool> left_single, Optional<bool> above_intra, Optional<bool> left_intra, Optional<ReferenceFramePair> above_ref_frame, Optional<ReferenceFramePair> left_ref_frame)
{
    // FIXME: Above and left contexts should be in structs.

    // Probabilities
    u8 context;
    if (above_single.has_value() && left_single.has_value()) {
        if (above_intra.value() && left_intra.value()) {
            context = 2;
        } else if (left_intra.value()) {
            if (above_single.value()) {
                context = 4 * (above_ref_frame.value()[0] == LastFrame);
            } else {
                context = 1 + (above_ref_frame.value()[0] == LastFrame || above_ref_frame.value()[1] == LastFrame);
            }
        } else if (above_intra.value()) {
            if (left_single.value()) {
                context = 4 * (left_ref_frame.value()[0] == LastFrame);
            } else {
                context = 1 + (left_ref_frame.value()[0] == LastFrame || left_ref_frame.value()[1] == LastFrame);
            }
        } else {
            if (left_single.value() && above_single.value()) {
                context = 2 * (above_ref_frame.value()[0] == LastFrame) + 2 * (left_ref_frame.value()[0] == LastFrame);
            } else if (!left_single.value() && !above_single.value()) {
                auto above_is_last = above_ref_frame.value()[0] == LastFrame || above_ref_frame.value()[1] == LastFrame;
                auto left_is_last = left_ref_frame.value()[0] == LastFrame || left_ref_frame.value()[1] == LastFrame;
                context = 1 + (above_is_last || left_is_last);
            } else {
                auto rfs = above_single.value() ? above_ref_frame.value()[0] : left_ref_frame.value()[0];
                auto crf1 = above_single.value() ? left_ref_frame.value()[0] : above_ref_frame.value()[0];
                auto crf2 = above_single.value() ? left_ref_frame.value()[1] : above_ref_frame.value()[1];
                context = crf1 == LastFrame || crf2 == LastFrame;
                if (rfs == LastFrame)
                    context += 3;
            }
        }
    } else if (above_single.has_value()) {
        if (above_intra.value()) {
            context = 2;
        } else {
            if (above_single.value()) {
                context = 4 * (above_ref_frame.value()[0] == LastFrame);
            } else {
                context = 1 + (above_ref_frame.value()[0] == LastFrame || above_ref_frame.value()[1] == LastFrame);
            }
        }
    } else if (left_single.has_value()) {
        if (left_intra.value()) {
            context = 2;
        } else {
            if (left_single.value()) {
                context = 4 * (left_ref_frame.value()[0] == LastFrame);
            } else {
                context = 1 + (left_ref_frame.value()[0] == LastFrame || left_ref_frame.value()[1] == LastFrame);
            }
        }
    } else {
        context = 2;
    }
    u8 probability = probability_table.single_ref_prob()[context][0];

    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_single_ref[context][0][value]);
    return value;
}

ErrorOr<bool> TreeParser::parse_single_ref_part_2(BitStream& bit_stream, ProbabilityTables const& probability_table, SyntaxElementCounter& counter, Optional<bool> above_single, Optional<bool> left_single, Optional<bool> above_intra, Optional<bool> left_intra, Optional<ReferenceFramePair> above_ref_frame, Optional<ReferenceFramePair> left_ref_frame)
{
    // FIXME: Above and left contexts should be in structs.

    // Probabilities
    u8 context;
    if (above_single.has_value() && left_single.has_value()) {
        if (above_intra.value() && left_intra.value()) {
            context = 2;
        } else if (left_intra.value()) {
            if (above_single.value()) {
                if (above_ref_frame.value()[0] == LastFrame) {
                    context = 3;
                } else {
                    context = 4 * (above_ref_frame.value()[0] == GoldenFrame);
                }
            } else {
                context = 1 + 2 * (above_ref_frame.value()[0] == GoldenFrame || above_ref_frame.value()[1] == GoldenFrame);
            }
        } else if (above_intra.value()) {
            if (left_single.value()) {
                if (left_ref_frame.value()[0] == LastFrame) {
                    context = 3;
                } else {
                    context = 4 * (left_ref_frame.value()[0] == GoldenFrame);
                }
            } else {
                context = 1 + 2 * (left_ref_frame.value()[0] == GoldenFrame || left_ref_frame.value()[1] == GoldenFrame);
            }
        } else {
            if (left_single.value() && above_single.value()) {
                auto above_last = above_ref_frame.value()[0] == LastFrame;
                auto left_last = left_ref_frame.value()[0] == LastFrame;
                if (above_last && left_last) {
                    context = 3;
                } else if (above_last) {
                    context = 4 * (left_ref_frame.value()[0] == GoldenFrame);
                } else if (left_last) {
                    context = 4 * (above_ref_frame.value()[0] == GoldenFrame);
                } else {
                    context = 2 * (above_ref_frame.value()[0] == GoldenFrame) + 2 * (left_ref_frame.value()[0] == GoldenFrame);
                }
            } else if (!left_single.value() && !above_single.value()) {
                if (above_ref_frame.value()[0] == left_ref_frame.value()[0] && above_ref_frame.value()[1] == left_ref_frame.value()[1]) {
                    context = 3 * (above_ref_frame.value()[0] == GoldenFrame || above_ref_frame.value()[1] == GoldenFrame);
                } else {
                    context = 2;
                }
            } else {
                auto rfs = above_single.value() ? above_ref_frame.value()[0] : left_ref_frame.value()[0];
                auto crf1 = above_single.value() ? left_ref_frame.value()[0] : above_ref_frame.value()[0];
                auto crf2 = above_single.value() ? left_ref_frame.value()[1] : above_ref_frame.value()[1];
                context = crf1 == GoldenFrame || crf2 == GoldenFrame;
                if (rfs == GoldenFrame) {
                    context += 3;
                } else if (rfs != AltRefFrame) {
                    context = 1 + (2 * context);
                }
            }
        }
    } else if (above_single.has_value()) {
        if (above_intra.value() || (above_ref_frame.value()[0] == LastFrame && above_single.value())) {
            context = 2;
        } else if (above_single.value()) {
            context = 4 * (above_ref_frame.value()[0] == GoldenFrame);
        } else {
            context = 3 * (above_ref_frame.value()[0] == GoldenFrame || above_ref_frame.value()[1] == GoldenFrame);
        }
    } else if (left_single.has_value()) {
        if (left_intra.value() || (left_ref_frame.value()[0] == LastFrame && left_single.value())) {
            context = 2;
        } else if (left_single.value()) {
            context = 4 * (left_ref_frame.value()[0] == GoldenFrame);
        } else {
            context = 3 * (left_ref_frame.value()[0] == GoldenFrame || left_ref_frame.value()[1] == GoldenFrame);
        }
    } else {
        context = 2;
    }
    u8 probability = probability_table.single_ref_prob()[context][1];

    auto value = TRY(parse_tree_new<bool>(bit_stream, { binary_tree }, [&](u8) { return probability; }));
    increment_counter(counter.m_counts_single_ref[context][1][value]);
    return value;
}

/*
 * Select a tree value based on the type of syntax element being parsed, as well as some parser state, as specified in section 9.3.1
 */
TreeParser::TreeSelection TreeParser::select_tree(SyntaxElementType type)
{
    switch (type) {
    case SyntaxElementType::MVSign:
    case SyntaxElementType::MVClass0Bit:
    case SyntaxElementType::MVBit:
    case SyntaxElementType::MoreCoefs:
        return { binary_tree };
    case SyntaxElementType::MVJoint:
        return { mv_joint_tree };
    case SyntaxElementType::MVClass:
        return { mv_class_tree };
    case SyntaxElementType::MVClass0FR:
    case SyntaxElementType::MVFR:
        return { mv_fr_tree };
    case SyntaxElementType::MVClass0HP:
    case SyntaxElementType::MVHP:
        if (m_decoder.m_use_hp)
            return { binary_tree };
        return { 1 };
    case SyntaxElementType::Token:
        return { token_tree };
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

/*
 * Select a probability with which to read a boolean when decoding a tree, as specified in section 9.3.2
 */
u8 TreeParser::select_tree_probability(SyntaxElementType type, u8 node)
{
    switch (type) {
    case SyntaxElementType::MVSign:
        return m_decoder.m_probability_tables->mv_sign_prob()[m_mv_component];
    case SyntaxElementType::MVClass0Bit:
        return m_decoder.m_probability_tables->mv_class0_bit_prob()[m_mv_component];
    case SyntaxElementType::MVBit:
        VERIFY(m_mv_bit < MV_OFFSET_BITS);
        return m_decoder.m_probability_tables->mv_bits_prob()[m_mv_component][m_mv_bit];
    case SyntaxElementType::MVJoint:
        return m_decoder.m_probability_tables->mv_joint_probs()[node];
    case SyntaxElementType::MVClass:
        // Spec doesn't mention node, but the probabilities table has an extra dimension
        // so we will use node for that.
        return m_decoder.m_probability_tables->mv_class_probs()[m_mv_component][node];
    case SyntaxElementType::MVClass0FR:
        VERIFY(m_mv_class0_bit < CLASS0_SIZE);
        return m_decoder.m_probability_tables->mv_class0_fr_probs()[m_mv_component][m_mv_class0_bit][node];
    case SyntaxElementType::MVClass0HP:
        return m_decoder.m_probability_tables->mv_class0_hp_prob()[m_mv_component];
    case SyntaxElementType::MVFR:
        return m_decoder.m_probability_tables->mv_fr_probs()[m_mv_component][node];
    case SyntaxElementType::MVHP:
        return m_decoder.m_probability_tables->mv_hp_prob()[m_mv_component];
    case SyntaxElementType::Token:
        return calculate_token_probability(node);
    case SyntaxElementType::MoreCoefs:
        return calculate_more_coefs_probability();
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

#define ABOVE_FRAME_0 m_decoder.m_above_ref_frame[0]
#define ABOVE_FRAME_1 m_decoder.m_above_ref_frame[1]
#define LEFT_FRAME_0 m_decoder.m_left_ref_frame[0]
#define LEFT_FRAME_1 m_decoder.m_left_ref_frame[1]
#define AVAIL_U m_decoder.m_available_u
#define AVAIL_L m_decoder.m_available_l
#define ABOVE_INTRA m_decoder.m_above_intra
#define LEFT_INTRA m_decoder.m_left_intra
#define ABOVE_SINGLE m_decoder.m_above_single
#define LEFT_SINGLE m_decoder.m_left_single

void TreeParser::set_tokens_variables(u8 band, u32 c, u32 plane, TXSize tx_size, u32 pos)
{
    m_band = band;
    m_c = c;
    m_plane = plane;
    m_tx_size = tx_size;
    m_pos = pos;

    if (m_c == 0) {
        auto sx = m_plane > 0 ? m_decoder.m_subsampling_x : 0;
        auto sy = m_plane > 0 ? m_decoder.m_subsampling_y : 0;
        auto max_x = (2 * m_decoder.m_mi_cols) >> sx;
        auto max_y = (2 * m_decoder.m_mi_rows) >> sy;
        u8 numpts = 1 << m_tx_size;
        auto x4 = m_start_x >> 2;
        auto y4 = m_start_y >> 2;
        u32 above = 0;
        u32 left = 0;
        for (size_t i = 0; i < numpts; i++) {
            if (x4 + i < max_x)
                above |= m_decoder.m_above_nonzero_context[m_plane][x4 + i];
            if (y4 + i < max_y)
                left |= m_decoder.m_left_nonzero_context[m_plane][y4 + i];
        }
        m_ctx = above + left;
    } else {
        u32 neighbor_0, neighbor_1;
        auto n = 4 << m_tx_size;
        auto i = m_pos / n;
        auto j = m_pos % n;
        auto a = i > 0 ? (i - 1) * n + j : 0;
        auto a2 = i * n + j - 1;
        if (i > 0 && j > 0) {
            if (m_decoder.m_tx_type == DCT_ADST) {
                neighbor_0 = a;
                neighbor_1 = a;
            } else if (m_decoder.m_tx_type == ADST_DCT) {
                neighbor_0 = a2;
                neighbor_1 = a2;
            } else {
                neighbor_0 = a;
                neighbor_1 = a2;
            }
        } else if (i > 0) {
            neighbor_0 = a;
            neighbor_1 = a;
        } else {
            neighbor_0 = a2;
            neighbor_1 = a2;
        }
        m_ctx = (1 + m_decoder.m_token_cache[neighbor_0] + m_decoder.m_token_cache[neighbor_1]) >> 1;
    }
}

u8 TreeParser::calculate_more_coefs_probability()
{
    return m_decoder.m_probability_tables->coef_probs()[m_tx_size][m_plane > 0][m_decoder.m_is_inter][m_band][m_ctx][0];
}

u8 TreeParser::calculate_token_probability(u8 node)
{
    auto prob = m_decoder.m_probability_tables->coef_probs()[m_tx_size][m_plane > 0][m_decoder.m_is_inter][m_band][m_ctx][min(2, 1 + node)];
    if (node < 2)
        return prob;
    auto x = (prob - 1) / 2;
    auto& pareto_table = m_decoder.m_probability_tables->pareto_table();
    if (prob & 1)
        return pareto_table[x][node - 2];
    return (pareto_table[x][node - 2] + pareto_table[x + 1][node - 2]) >> 1;
}

void TreeParser::count_syntax_element(SyntaxElementType type, int value)
{
    auto increment = [](u8& count) {
        increment_counter(count);
    };
    switch (type) {
    case SyntaxElementType::MVSign:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_sign[m_mv_component][value]);
        return;
    case SyntaxElementType::MVClass0Bit:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_class0_bit[m_mv_component][value]);
        return;
    case SyntaxElementType::MVBit:
        VERIFY(m_mv_bit < MV_OFFSET_BITS);
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_bits[m_mv_component][m_mv_bit][value]);
        m_mv_bit = 0xFF;
        return;
    case SyntaxElementType::MVJoint:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_joint[value]);
        return;
    case SyntaxElementType::MVClass:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_class[m_mv_component][value]);
        return;
    case SyntaxElementType::MVClass0FR:
        VERIFY(m_mv_class0_bit < CLASS0_SIZE);
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_class0_fr[m_mv_component][m_mv_class0_bit][value]);
        m_mv_class0_bit = 0xFF;
        return;
    case SyntaxElementType::MVClass0HP:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_class0_hp[m_mv_component][value]);
        return;
    case SyntaxElementType::MVFR:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_fr[m_mv_component][value]);
        return;
    case SyntaxElementType::MVHP:
        increment(m_decoder.m_syntax_element_counter->m_counts_mv_hp[m_mv_component][value]);
        return;
    case SyntaxElementType::Token:
        increment(m_decoder.m_syntax_element_counter->m_counts_token[m_tx_size][m_plane > 0][m_decoder.m_is_inter][m_band][m_ctx][min(2, value)]);
        return;
    case SyntaxElementType::MoreCoefs:
        increment(m_decoder.m_syntax_element_counter->m_counts_more_coefs[m_tx_size][m_plane > 0][m_decoder.m_is_inter][m_band][m_ctx][value]);
        return;
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

}
