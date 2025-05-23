/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACT_GEMV_TILE_TILE_VMAD_HPP
#define ACT_GEMV_TILE_TILE_VMAD_HPP

#include "act/act.hpp"
#include "act/layout/layout.hpp"
#include "act/gemm/gemm_type.hpp"

namespace Act::Gemv::Tile
{

    template <
        /// Tag indicating architecture
        class ArchTag,
        class AType,
        class XType,
        class YType,
        class BiasType = void>

    struct TileVmad
    {
        static_assert(DEPENDENT_FALSE<ArchTag>, "Unsupported TileVmad, can not find the specialization.");
    };
    template <>
    struct TileVmad<Arch::AtlasA2,
                    Gemm::GemmType<half, layout::RowMajor>,
                    Gemm::GemmType<half, layout::VectorLayout>,
                    Gemm::GemmType<half, layout::VectorLayout>,
                    void>
    {
        using ElementA = half;
        using ElementX = half;
        using ElementY = half;
        using ElementAccumulator =
            typename Gemv::helper::ElementAccumulatorSelector<half, half>::ElementAccumulator;

        using LayoutDst = layout::RowMajor;
        using LayoutSrc = layout::RowMajor;
        static constexpr uint32_t ELE_NUM_PER_C0 = BYTE_PER_C0 / sizeof(ElementA);

        // Mehtods

        ACT_DEVICE
        TileVmad() {};

        ACT_DEVICE
        void operator()(
            AscendC::LocalTensor<ElementY> dstTensor,
            AscendC::LocalTensor<ElementX> srcTensor_v,
            AscendC::LocalTensor<ElementA> srcTensor_m,
            AscendC::LocalTensor<ElementAccumulator> temp,
            LayoutDst const &layoutDst, LayoutSrc const &layoutSrc
        )
        {
            uint32_t m_actual = layoutSrc.shape(0);
            uint32_t n_actual = layoutSrc.shape(1);
            uint32_t m_round = layoutDst.shape(0);
            uint32_t n_round = layoutDst.shape(1);
            uint32_t temp_repeat_size = BYTE_PER_C0 * 8 / sizeof(ElementAccumulator);
            uint32_t elem_repeat_size = ELE_NUM_PER_C0 * 8;
            uint32_t mask = temp_repeat_size;
            uint32_t repeattimes = CeilDiv(m_actual, temp_repeat_size);
            AscendC::Duplicate<ElementAccumulator>( 
                temp,
                (ElementAccumulator)0.0,
                temp_repeat_size,                                    
                CeilDiv(m_round * temp_repeat_size, temp_repeat_size), 
                1,                                                     
                8                                                     
            );

            uint32_t repeat_num = n_actual / temp_repeat_size;
            uint32_t remain = n_actual % temp_repeat_size;

            AscendC::PipeBarrier<PIPE_V>();
            AscendC::BinaryRepeatParams params;
            params.dstBlkStride = 1;
            params.src0BlkStride = 1;
            params.src1BlkStride = 1;
            params.dstRepStride = RoundUp(temp_repeat_size, temp_repeat_size) / (BYTE_PER_C0 / sizeof(ElementAccumulator));
            params.src0RepStride = RoundUp(n_round, elem_repeat_size) / ELE_NUM_PER_C0;
            params.src1RepStride = 0;
            AscendC::SetMaskCount();
            AscendC::SetVectorMask<ElementAccumulator, AscendC::MaskMode::COUNTER>(m_actual * temp_repeat_size);
            for (uint32_t i = 0; i < repeat_num; i++)
            {
                uint32_t offset = i * temp_repeat_size; 
                AscendC::MulAddDst<ElementAccumulator, ElementA, false>(
                    temp,
                    srcTensor_m[offset],
                    srcTensor_v[offset],
                    AscendC::MASK_PLACEHOLDER,
                    1,
                    params);

                AscendC::PipeBarrier<PIPE_V>();
            }
            AscendC::SetMaskNorm();
            AscendC::ResetMask(); 

            if (remain > 0)
            {
                uint32_t offset = repeat_num * temp_repeat_size;
                if (offset + remain > n_round)
                {
                    remain = n_round - offset;
                }
                uint64_t remain_mask = remain; 
                AscendC::MulAddDst<ElementAccumulator, ElementA, true>(
                    temp,
                    srcTensor_m[offset],
                    srcTensor_v[offset],
                    remain_mask,
                    m_actual,
                    params);
            }

            uint64_t reduce_mask = (repeat_num == 0) ? remain : temp_repeat_size;
            AscendC::PipeBarrier<PIPE_V>();
            AscendC::WholeReduceSum<ElementAccumulator, true>(
                temp,
                temp,
                reduce_mask,
                m_actual,
                1,
                1,
                8);
            AscendC::PipeBarrier<PIPE_V>();
            AscendC::UnaryRepeatParams castparams;
            castparams.dstBlkStride = 1;
            castparams.srcBlkStride = 1;
            castparams.dstRepStride = 4; 
            castparams.srcRepStride = 8;
            AscendC::Cast<ElementA, ElementAccumulator, true>(
                srcTensor_m,
                temp,
                AscendC::RoundMode::CAST_NONE,
                (uint64_t)mask,
                repeattimes,
                castparams);
            AscendC::PipeBarrier<PIPE_V>();

            uint64_t add_mask = (m_actual < elem_repeat_size) ? m_actual : elem_repeat_size;
            params.dstRepStride = 8;
            params.src0RepStride = 8;
            params.src1RepStride = 8;
            AscendC::Add<ElementA, true>(
                dstTensor,
                srcTensor_m,
                dstTensor,
                (uint64_t)add_mask,
                CeilDiv(m_round, elem_repeat_size),
                params);
        }
    };

    template <
        class ElementA,
        class ElementX,
        class ElementY>

    struct TileVmad<Arch::AtlasA2,
                    Gemm::GemmType<ElementA, layout::RowMajor>,
                    Gemm::GemmType<ElementX, layout::VectorLayout>,
                    Gemm::GemmType<ElementY, layout::VectorLayout>,
                    void>
    {
        using ElementAccumulator =
            typename Gemv::helper::ElementAccumulatorSelector<ElementA, ElementX>::ElementAccumulator;
        using LayoutDst = layout::RowMajor;
        using LayoutSrc = layout::RowMajor;
        static constexpr uint32_t ELE_NUM_PER_C0 = BYTE_PER_C0 / sizeof(ElementA); 

        // Mehtods

        ACT_DEVICE
        TileVmad() {};

        ACT_DEVICE
        void operator()(
            AscendC::LocalTensor<ElementY> dstTensor,
            AscendC::LocalTensor<ElementX> srcTensor_v,
            AscendC::LocalTensor<ElementA> srcTensor_m,
            AscendC::LocalTensor<ElementAccumulator> temp,
            LayoutDst const &layoutDst, LayoutSrc const &layoutSrc
        )
        {
            uint32_t m_actual = layoutSrc.shape(0);
            uint32_t n_actual = layoutSrc.shape(1);
            uint32_t m_round = layoutDst.shape(0);
            uint32_t n_round = layoutDst.shape(1);

            uint32_t repeat_size = ELE_NUM_PER_C0 * 8; 
            uint32_t mask = repeat_size;
            uint32_t repeat_num = n_actual / repeat_size;
            uint32_t remain = n_actual % repeat_size;

            AscendC::BinaryRepeatParams params;
            params.dstBlkStride = 1;
            params.src0BlkStride = 1;
            params.src1BlkStride = 1;
            params.dstRepStride = RoundUp(n_round, repeat_size) / ELE_NUM_PER_C0;
            params.src0RepStride = RoundUp(n_round, repeat_size) / ELE_NUM_PER_C0;
            params.src1RepStride = 0;
            AscendC::SetMaskCount();
            AscendC::SetVectorMask<ElementA, AscendC::MaskMode::COUNTER>(m_actual * repeat_size);
            for (uint32_t i = 0; i < repeat_num; i++)
            {
                uint32_t offset = i * repeat_size;
                if (i == 0)
                {
                    AscendC::Mul<ElementA, false>(
                        srcTensor_m,
                        srcTensor_m,
                        srcTensor_v,
                        AscendC::MASK_PLACEHOLDER,
                        1,
                        params);
                }
                else
                {
                    AscendC::MulAddDst<ElementA, ElementA, false>(
                        srcTensor_m,
                        srcTensor_m[offset],
                        srcTensor_v[offset],
                        AscendC::MASK_PLACEHOLDER,
                        1,
                        params);
                }
                AscendC::PipeBarrier<PIPE_V>();
            }
            AscendC::SetMaskNorm();
            AscendC::ResetMask(); 

            if (remain > 0)
            {
                uint32_t offset = repeat_num * repeat_size;
                if (offset + remain > n_round)
                {
                    remain = n_round - offset;
                }
                uint64_t remain_mask = remain;
                if (repeat_num == 0)
                {
                    AscendC::Mul<ElementA, true>(
                        srcTensor_m,
                        srcTensor_m,
                        srcTensor_v,
                        remain_mask,
                        m_actual,
                        params);
                }
                else
                {
                    AscendC::MulAddDst<ElementA, ElementA, true>(
                        srcTensor_m,
                        srcTensor_m[offset],
                        srcTensor_v[offset],
                        remain_mask,
                        m_actual,
                        params);
                }
            }

            uint64_t reduce_mask = (repeat_num == 0) ? remain : repeat_size;
            AscendC::PipeBarrier<PIPE_V>();
            AscendC::WholeReduceSum<ElementA, true>(
                srcTensor_m,
                srcTensor_m,
                reduce_mask,
                m_actual,
                1,
                1,
                RoundUp(n_round, repeat_size) / ELE_NUM_PER_C0);

            uint64_t add_mask = (m_actual < repeat_size) ? m_actual : repeat_size;
            params.dstRepStride = 8;
            params.src0RepStride = 8;
            params.src1RepStride = 8;

            AscendC::PipeBarrier<PIPE_V>();
            AscendC::Add<ElementA, true>(
                dstTensor,
                srcTensor_m,
                dstTensor,
                add_mask,
                CeilDiv(m_round, repeat_size), 
                params);
        }
    };

    template <>
    struct TileVmad<Arch::AtlasA2,
                    Gemm::GemmType<half, layout::ColumnMajor>,
                    Gemm::GemmType<half, layout::VectorLayout>,
                    Gemm::GemmType<half, layout::VectorLayout>,
                    void>
    {
        using ElementA = half;
        using ElementX = half;
        using ElementY = half;
        using ElementAccumulator =
            typename Gemv::helper::ElementAccumulatorSelector<half, half>::ElementAccumulator;
        using LayoutDst = layout::ColumnMajor;
        using LayoutSrc = layout::ColumnMajor;
        static constexpr uint32_t ELE_NUM_PER_C0 = BYTE_PER_C0 / sizeof(ElementA); 

        // Mehtods

        ACT_DEVICE
        TileVmad() {};

        ACT_DEVICE
        void operator()(
            AscendC::LocalTensor<ElementY> dstTensor,
            AscendC::LocalTensor<ElementX> srcTensor_v,
            AscendC::LocalTensor<ElementA> srcTensor_m,
            AscendC::LocalTensor<ElementAccumulator> temp,
            LayoutDst const &layoutDst, LayoutSrc const &layoutSrc
        )
        {
            uint32_t m_actual = layoutSrc.shape(0);
            uint32_t n_actual = layoutSrc.shape(1);
            uint32_t m_round = layoutDst.shape(0);
            uint32_t n_round = layoutDst.shape(1);
            AscendC::SetMaskCount();
            AscendC::SetVectorMask<ElementAccumulator, AscendC::MaskMode::COUNTER>(m_actual); 
            AscendC::Duplicate<ElementAccumulator, false>(                                 
                temp,
                (ElementAccumulator)0.0,
                AscendC::MASK_PLACEHOLDER,
                1,                      
                1,                   
                8                          
            );
            AscendC::PipeBarrier<PIPE_V>();

            ElementX pix[32];
            AscendC::SetFlag<AscendC::HardEvent::V_S>((event_t)(0));
            AscendC::WaitFlag<AscendC::HardEvent::V_S>((event_t)(0));
            for (uint32_t i = 0; i < n_actual; i++)
            {
                pix[i] = srcTensor_v.GetValue(i);
            }
            AscendC::SetFlag<AscendC::HardEvent::S_V>((event_t)(0));
            AscendC::WaitFlag<AscendC::HardEvent::S_V>((event_t)(0));

            AscendC::UnaryRepeatParams params;
            params.dstBlkStride = 1;
            params.srcBlkStride = 1;
            params.dstRepStride = 8;
            params.srcRepStride = 4; 
            for (uint32_t i = 0; i < n_actual; i++)
            {
                AscendC::Axpy<ElementAccumulator, ElementA, false>(
                    temp,
                    srcTensor_m[i * m_round],
                    pix[i],
                    AscendC::MASK_PLACEHOLDER,
                    1,
                    params);
                AscendC::PipeBarrier<PIPE_V>();
            }
            params.dstRepStride = 4; 
            params.srcRepStride = 8;
            AscendC::Cast<ElementA, ElementAccumulator, false>(
                srcTensor_m,
                temp,
                AscendC::RoundMode::CAST_NONE,
                AscendC::MASK_PLACEHOLDER,
                1,
                params);
            AscendC::BinaryRepeatParams addparams;
            addparams.dstBlkStride = 1;
            addparams.src0BlkStride = 1;
            addparams.src1BlkStride = 1;
            addparams.dstRepStride = 8;
            addparams.src0RepStride = 8;
            addparams.src1RepStride = 8;
            AscendC::PipeBarrier<PIPE_V>();
            AscendC::Add<ElementA, false>(
                dstTensor,
                srcTensor_m,
                dstTensor,
                AscendC::MASK_PLACEHOLDER,
                1,
                addparams);
            AscendC::SetMaskNorm();
            AscendC::ResetMask(); 
        }
    };

    template <
        class ElementA,
        class ElementX,
        class ElementY>
    struct TileVmad<Arch::AtlasA2,
                    Gemm::GemmType<ElementA, layout::ColumnMajor>,
                    Gemm::GemmType<ElementX, layout::VectorLayout>,
                    Gemm::GemmType<ElementY, layout::VectorLayout>,
                    void>
    {
        using ElementAccumulator =
            typename Gemv::helper::ElementAccumulatorSelector<ElementA, ElementX>::ElementAccumulator;
        using LayoutDst = layout::ColumnMajor;
        using LayoutSrc = layout::ColumnMajor;
        static constexpr uint32_t ELE_NUM_PER_C0 = BYTE_PER_C0 / sizeof(ElementA); 

        // Mehtods

        ACT_DEVICE
        TileVmad() {};

        ACT_DEVICE
        void operator()(
            AscendC::LocalTensor<ElementY> dstTensor,
            AscendC::LocalTensor<ElementX> srcTensor_v,
            AscendC::LocalTensor<ElementA> srcTensor_m,
            AscendC::LocalTensor<ElementAccumulator> temp,
            LayoutDst const &layoutDst, LayoutSrc const &layoutSrc
        )
        {
            uint32_t m_actual = layoutSrc.shape(0);
            uint32_t n_actual = layoutSrc.shape(1);
            uint32_t m_round = layoutDst.shape(0);
            uint32_t n_round = layoutDst.shape(1);
            ElementX pix[32];
            AscendC::SetFlag<AscendC::HardEvent::V_S>((event_t)(0));
            AscendC::WaitFlag<AscendC::HardEvent::V_S>((event_t)(0));
            for (uint32_t i = 0; i < n_actual; i++)
            {
                pix[i] = srcTensor_v.GetValue(i);
            }
            AscendC::SetFlag<AscendC::HardEvent::S_V>((event_t)(0));
            AscendC::WaitFlag<AscendC::HardEvent::S_V>((event_t)(0));
            AscendC::UnaryRepeatParams params;
            params.dstBlkStride = 1;
            params.srcBlkStride = 1;
            params.dstRepStride = 8;
            params.srcRepStride = 8;
            AscendC::SetMaskCount();
            AscendC::SetVectorMask<ElementA, AscendC::MaskMode::COUNTER>(m_actual); 
            for (uint32_t i = 0; i < n_actual; i++)
            {
                AscendC::Axpy<ElementY, ElementA, false>( 
                    dstTensor,
                    srcTensor_m[i * m_round],
                    pix[i],
                    AscendC::MASK_PLACEHOLDER,
                    1,
                    params);
                AscendC::PipeBarrier<PIPE_V>();
            }
            AscendC::SetMaskNorm();
            AscendC::ResetMask();
        }
    };
}

#endif // ACT_GEMV_TILE_TILE_VMAD_HPP
