/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACT_GEMM_KERNEL_BATCHED_MATMUL_HPP
#define ACT_GEMM_KERNEL_BATCHED_MATMUL_HPP

#include "act/act.hpp"
#include "act/arch/resource.hpp"
#include "act/coord.hpp"
#include "act/gemm_coord.hpp"
#include "act/matrix_coord.hpp"

namespace Act::Gemm::Kernel {

// Template for Batched Matmul kernel. Compute batched C = A * B
template <
    class BlockMmad_,
    class BlockEpilogue_,
    class BlockScheduler_
>
class BatchedMatmul {
public:
    using BlockMmad = BlockMmad_;
    using ArchTag = typename BlockMmad::ArchTag;
    using L1TileShape = typename BlockMmad::L1TileShape;
    using ElementA = typename BlockMmad::ElementA;
    using LayoutA = typename BlockMmad::LayoutA;
    using ElementB = typename BlockMmad::ElementB;
    using LayoutB = typename BlockMmad::LayoutB;
    using ElementC = typename BlockMmad::ElementC;
    using LayoutC = typename BlockMmad::LayoutC;
    using ElementAccumulator = typename BlockMmad::ElementAccumulator;

    using BlockScheduler = BlockScheduler_;

    /// Parameters structure
    struct Params {
        // Data members
        uint32_t batchCount;
        GemmCoord problemShape;
        GM_ADDR ptrA;
        LayoutA layoutA;
        int64_t strideA;
        GM_ADDR ptrB;
        LayoutB layoutB;
        int64_t strideB;
        GM_ADDR ptrC;
        LayoutC layoutC;
        int64_t strideC;
        float quantScale;
        uint8_t useFixpipeQuantization;

        // Methods
        ACT_DEVICE
        Params() {}

        ACT_DEVICE
        Params(uint32_t batchCount_, GemmCoord const &problemShape_,
               GM_ADDR ptrA_, LayoutA layoutA_, int64_t strideA_,
               GM_ADDR ptrB_, LayoutB layoutB_, int64_t strideB_,
               GM_ADDR ptrC_, LayoutC layoutC_, int64_t strideC_)
            : batchCount(batchCount_), problemShape(problemShape_),
              ptrA(ptrA_), layoutA(layoutA_), strideA(strideA_),
              ptrB(ptrB_), layoutB(layoutB_), strideB(strideB_),
              ptrC(ptrC_), layoutC(layoutC_), strideC(strideC_) {}

        ACT_DEVICE
        Params(uint32_t batchCount_, GemmCoord const &problemShape_,
                GM_ADDR ptrA_, LayoutA layoutA_, int64_t strideA_,
                GM_ADDR ptrB_, LayoutB layoutB_, int64_t strideB_,
                GM_ADDR ptrC_, LayoutC layoutC_, int64_t strideC_, float quantScale_)
            : batchCount(batchCount_), problemShape(problemShape_),
            ptrA(ptrA_), layoutA(layoutA_), strideA(strideA_),
            ptrB(ptrB_), layoutB(layoutB_), strideB(strideB_),
            ptrC(ptrC_), layoutC(layoutC_), strideC(strideC_),quantScale(quantScale_) {useFixpipeQuantization = 1;}
    };

    // Methods
    ACT_DEVICE
    BatchedMatmul() {}

    template <int32_t CORE_TYPE = g_coreType>
    ACT_DEVICE
    void operator()(Params const &params);

    /// Executes one GEMM
    template <>
    ACT_DEVICE
    void operator()<AscendC::AIC>(Params const &params) {
        BlockScheduler matmulBlockScheduler(params.problemShape, MakeCoord(L1TileShape::M, L1TileShape::N));
        uint32_t coreLoops = params.batchCount * matmulBlockScheduler.GetCoreLoops();

        Arch::Resource<ArchTag> resource;
        BlockMmad blockMmad(resource);

        // Represent the full gm
        AscendC::GlobalTensor<ElementA> gmA;
        gmA.SetGlobalBuffer((__gm__ ElementA *)params.ptrA);
        AscendC::GlobalTensor<ElementB> gmB;
        gmB.SetGlobalBuffer((__gm__ ElementB *)params.ptrB);
        AscendC::GlobalTensor<ElementC> gmC;
        gmC.SetGlobalBuffer((__gm__ ElementC *)params.ptrC);

        if(params.useFixpipeQuantization == 0)
        {
            for (uint32_t loopIdx = AscendC::GetBlockIdx(); loopIdx < coreLoops; loopIdx += AscendC::GetBlockNum()) {
                // Compute block location
                uint32_t batchIdx = matmulBlockScheduler.GetBatchIdx(loopIdx);
                GemmCoord blockCoord = matmulBlockScheduler.GetBlockCoord(loopIdx);
                GemmCoord actualBlockShape = matmulBlockScheduler.GetActualBlockShape(blockCoord);
    
                // batchOffset
                int64_t batchOffsetA = batchIdx * params.strideA;
                int64_t batchOffsetB = batchIdx * params.strideB;
                int64_t batchOffsetC = batchIdx * params.strideC;
    
                // Compute initial location in logical coordinates
                MatrixCoord offsetA{blockCoord.m() * L1TileShape::M, blockCoord.k() * L1TileShape::K};
                MatrixCoord offsetB{blockCoord.k() * L1TileShape::K, blockCoord.n() * L1TileShape::N};
                MatrixCoord offsetC{blockCoord.m() * L1TileShape::M, blockCoord.n() * L1TileShape::N};
                int64_t gmOffsetA = params.layoutA.GetOffset(offsetA);
                int64_t gmOffsetB = params.layoutB.GetOffset(offsetB);
                int64_t gmOffsetC = params.layoutC.GetOffset(offsetC);
    
                // Compute block-scoped matrix multiply-add
                blockMmad(
                    gmA[batchOffsetA + gmOffsetA], params.layoutA,
                    gmB[batchOffsetB + gmOffsetB], params.layoutB,
                    gmC[batchOffsetC + gmOffsetC], params.layoutC,
                    actualBlockShape);
            }
        }
        else
        {
            for (uint32_t loopIdx = AscendC::GetBlockIdx(); loopIdx < coreLoops; loopIdx += AscendC::GetBlockNum()) {
                // Compute block location
                uint32_t batchIdx = matmulBlockScheduler.GetBatchIdx(loopIdx);
                GemmCoord blockCoord = matmulBlockScheduler.GetBlockCoord(loopIdx);
                GemmCoord actualBlockShape = matmulBlockScheduler.GetActualBlockShape(blockCoord);
    
                // batchOffset
                int64_t batchOffsetA = batchIdx * params.strideA;
                int64_t batchOffsetB = batchIdx * params.strideB;
                int64_t batchOffsetC = batchIdx * params.strideC;
    
                // Compute initial location in logical coordinates
                MatrixCoord offsetA{blockCoord.m() * L1TileShape::M, blockCoord.k() * L1TileShape::K};
                MatrixCoord offsetB{blockCoord.k() * L1TileShape::K, blockCoord.n() * L1TileShape::N};
                MatrixCoord offsetC{blockCoord.m() * L1TileShape::M, blockCoord.n() * L1TileShape::N};
                int64_t gmOffsetA = params.layoutA.GetOffset(offsetA);
                int64_t gmOffsetB = params.layoutB.GetOffset(offsetB);
                int64_t gmOffsetC = params.layoutC.GetOffset(offsetC);
    
                // Compute block-scoped matrix multiply-add
                blockMmad(
                    gmA[batchOffsetA + gmOffsetA], params.layoutA,
                    gmB[batchOffsetB + gmOffsetB], params.layoutB,
                    gmC[batchOffsetC + gmOffsetC], params.layoutC,
                    actualBlockShape, params.quantScale);
            }
        }
    }

    template <>
    ACT_DEVICE
    void operator()<AscendC::AIV>(Params const &params) {}
};

} // namespace Act::Gemm::Kernel

#endif // ACT_GEMM_KERNEL_BATCHED_MATMUL_HPP