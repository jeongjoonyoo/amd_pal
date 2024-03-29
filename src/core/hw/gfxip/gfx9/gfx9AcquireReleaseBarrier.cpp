/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "core/hw/gfxip/gfx9/gfx9CmdUtil.h"
#include "core/hw/gfxip/gfx9/gfx9Device.h"
#include "core/hw/gfxip/gfx9/gfx9Image.h"
#include "core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.h"
#include "palAutoBuffer.h"

using namespace Util;

namespace Pal
{
namespace Gfx9
{

constexpr uint32 GraphicsOnlyPipeStages = PipelineStageVs            |
                                          PipelineStageHs            |
                                          PipelineStageDs            |
                                          PipelineStageGs            |
                                          PipelineStagePs            |
                                          PipelineStageEarlyDsTarget |
                                          PipelineStageLateDsTarget  |
                                          PipelineStageColorTarget;

// A structure that helps cache and reuse the calculated BLT transition and sync requests for an image barrier in
// acquire-release based barrier.
struct AcqRelTransitionInfo
{
    const ImgBarrier*  pImgBarrier;
    HwLayoutTransition transition;
    uint32             bltStageMask;
    uint32             bltAccessMask;
    bool               waNeedRefreshLlc; // Finer-grain refresh LLC flag.
};

// =====================================================================================================================
static IMsaaState* AcqRelBarrierMsaaState(
    const Device*                                pDevice,
    GfxCmdBuffer*                                pCmdBuf,
    LinearAllocatorAuto<VirtualLinearAllocator>* pAllocator,
    const ImgBarrier&                            imgBarrier)
{
    const auto& imageCreateInfo = imgBarrier.pImage->GetImageCreateInfo();

    MsaaStateCreateInfo msaaInfo    = {};
    msaaInfo.sampleMask             = 0xFFFF;
    msaaInfo.coverageSamples        = imageCreateInfo.samples;
    msaaInfo.alphaToCoverageSamples = imageCreateInfo.samples;

    // The following parameters should never be higher than the max number of msaa fragments ( 8 ).
    // All MSAA graphics barrier operations performed by PAL work on a per fragment basis.
    msaaInfo.exposedSamples          = imageCreateInfo.fragments;
    msaaInfo.pixelShaderSamples      = imageCreateInfo.fragments;
    msaaInfo.depthStencilSamples     = imageCreateInfo.fragments;
    msaaInfo.shaderExportMaskSamples = imageCreateInfo.fragments;
    msaaInfo.sampleClusters          = imageCreateInfo.fragments;
    msaaInfo.occlusionQuerySamples   = imageCreateInfo.fragments;

    IMsaaState* pMsaaState = nullptr;
    void*       pMemory    = PAL_MALLOC(pDevice->GetMsaaStateSize(msaaInfo, nullptr), pAllocator, AllocInternalTemp);
    if (pMemory == nullptr)
    {
        pCmdBuf->NotifyAllocFailure();
    }
    else
    {
        Result result = pDevice->CreateMsaaState(msaaInfo, pMemory, &pMsaaState);
        PAL_ASSERT(result == Result::Success);
    }

    return pMsaaState;
}

// =====================================================================================================================
// Translate acquire's accessMask (CacheCoherencyUsageFlags type) to cacheSyncFlags (CacheSyncFlags type)
// This function is GFX9-ONLY.
static uint32 Gfx9ConvertToAcquireSyncFlags(
    uint32                        accessMask,
    EngineType                    engineType,
    bool                          invalidateTcc,
    Developer::BarrierOperations* pBarrierOps)
{
    PAL_ASSERT(pBarrierOps != nullptr);
    uint32 cacheSyncFlagsMask = 0;
    bool gfxSupported = Pal::Device::EngineSupportsGraphics(engineType);

    // The acquire-release barrier treats L2 as the central cache, so we never flush/inv TCC unless it's
    // direct-to-memory access.
    if (TestAnyFlagSet(accessMask, CoherCpu | CoherMemory))
    {
        cacheSyncFlagsMask |= CacheSyncFlushTcc;
        pBarrierOps->caches.flushTcc = 1;
    }

    if (TestAnyFlagSet(accessMask, CoherShader))
    {
        cacheSyncFlagsMask |= CacheSyncInvSqK$ | CacheSyncInvTcp | CacheSyncInvTccMd;
        pBarrierOps->caches.invalSqK$        = 1;
        pBarrierOps->caches.invalTcp         = 1;
        pBarrierOps->caches.invalTccMetadata = 1;
    }

    // There are various BLTs (Copy, Clear, and Resolve) that can involve different caches based on what engine
    // does the BLT.
    // - If a graphics BLT occurred, alias to CB/DB. -> CacheSyncInvRb
    // - If a compute BLT occurred, alias to shader. -> CacheSyncInvSqK$,SqI$,Tcp,TccMd
    // - If a CP L2 BLT occured, alias to L2.        -> None (data is always in TCC as it's the central cache)
    if (TestAnyFlagSet(accessMask, CoherCopy | CoherResolve | CoherClear))
    {
        cacheSyncFlagsMask |= CacheSyncInvSqK$ | CacheSyncInvTcp | CacheSyncInvTccMd;
        pBarrierOps->caches.invalSqK$        = 1;
        pBarrierOps->caches.invalTcp         = 1;
        pBarrierOps->caches.invalTccMetadata = 1;
        if (gfxSupported)
        {
            cacheSyncFlagsMask |= CacheSyncInvRb & (~CacheSyncInvCbMd);
            pBarrierOps->caches.invalCb          = 1;
            // skip CB metadata, not supported on acquire. We always need to invalidate on release if we need it
            pBarrierOps->caches.invalDb          = 1;
            pBarrierOps->caches.invalDbMetadata  = 1;
            // There is no way to invalidate CB/DB without also flushing
            pBarrierOps->caches.flushCb          = 1;
            pBarrierOps->caches.flushDb          = 1;
            pBarrierOps->caches.flushDbMetadata  = 1;
        }
    }

    if (gfxSupported)
    {
        if (TestAnyFlagSet(accessMask, CoherColorTarget))
        {
                cacheSyncFlagsMask |= CacheSyncInvCbData;
                // skip CB metadata, not supported on acquire
                pBarrierOps->caches.invalCb = 1;
                // There is no way to invalidate CB without also flushing
                pBarrierOps->caches.flushCb = 1;
        }

        if (TestAnyFlagSet(accessMask, CoherDepthStencilTarget))
        {
            cacheSyncFlagsMask |= CacheSyncInvDb;
            pBarrierOps->caches.invalDb = 1;
            pBarrierOps->caches.invalDbMetadata = 1;
            // There is no way to invalidate DB without also flushing
            pBarrierOps->caches.flushDb         = 1;
            pBarrierOps->caches.flushDbMetadata = 1;
        }
    }

    if (TestAnyFlagSet(accessMask, CoherStreamOut))
    {
        // Read/write through Tcp$ and SqK$. Tcp$ is read-only.
        cacheSyncFlagsMask |= CacheSyncInvSqK$ | CacheSyncInvTcp;
        pBarrierOps->caches.invalSqK$ = 1;
        pBarrierOps->caches.invalTcp  = 1;
    }

    if (invalidateTcc)
    {
        cacheSyncFlagsMask |= CacheSyncInvTcc;
        pBarrierOps->caches.invalTcc = 1;
    }

    return cacheSyncFlagsMask;
}

// =====================================================================================================================
// Convert coarse BLT-level CacheCoherencyUsageFlags into specific flags based on the dirty state in the CmdBuffer
static uint32 OptimizeBltCacheAccess(
    GfxCmdBuffer* pCmdBuf,
    uint32        accessMask)
{
    // There are various srcCache BLTs (Copy, Clear, and Resolve) which we can further optimize if we know which
    // write caches have been dirtied:
    // - If a graphics BLT occurred, alias these srcCaches to CoherColorTarget.
    // - If a compute BLT occurred, alias these srcCaches to CoherShader.
    // - If a CP L2 BLT occured, alias these srcCaches to CoherTimestamp (this isn't good but we have no CoherL2).
    // - If a CP direct-to-memory write occured, alias these srcCaches to CoherMemory.
    // Clear the original srcCaches from the srcCache mask for the rest of this scope.
    if (TestAnyFlagSet(accessMask, CoherCopy | CoherClear | CoherResolve))
    {
        GfxCmdBufferState cmdBufState = pCmdBuf->GetGfxCmdBufState();
        accessMask &= ~(CoherCopy | CoherClear | CoherResolve);

        accessMask |= cmdBufState.flags.gfxWriteCachesDirty       ? CoherColorTarget : 0;
        accessMask |= cmdBufState.flags.csWriteCachesDirty        ? CoherShader      : 0;
        accessMask |= cmdBufState.flags.cpWriteCachesDirty        ? CoherTimestamp   : 0;
        accessMask |= cmdBufState.flags.cpMemoryWriteL2CacheStale ? CoherMemory      : 0;
    }
    return accessMask;
}

// =====================================================================================================================
// Translate release's accessMask (CacheCoherencyUsageFlags type) to cacheSyncFlags (CacheSyncFlags type)
// This function is GFX9-ONLY.
static uint32 Gfx9ConvertToReleaseSyncFlags(
    uint32                        accessMask,
    bool                          flushTcc,
    Developer::BarrierOperations* pBarrierOps)
{
    PAL_ASSERT(pBarrierOps != nullptr);
    uint32 cacheSyncFlagsMask = 0;

    if (TestAnyFlagSet(accessMask, CoherCpu | CoherMemory))
    {
        // At release we want to invalidate L2 so any future read to L2 would go down to memory, at acquire we want to
        // flush L2 so that main memory gets the latest data.
        cacheSyncFlagsMask |= CacheSyncInvTcc;
        pBarrierOps->caches.invalTcc = 1;
    }

    if (TestAnyFlagSet(accessMask, CoherColorTarget))
    {
        // There is no way to INV the CB metadata caches during acquire. So at release always also invalidate if we
        // need to flush CB metadata. But if we only flush CB data, FLUSH_AND_INV_CB_DATA_TS should be sufficient.
        cacheSyncFlagsMask |= CacheSyncFlushCbData | CacheSyncFlushCbMd | CacheSyncInvCbMd;
        pBarrierOps->caches.flushCb = 1;
        pBarrierOps->caches.flushCbMetadata = 1;
        pBarrierOps->caches.invalCbMetadata = 1;
    }

    if (TestAnyFlagSet(accessMask, CoherDepthStencilTarget))
    {
        cacheSyncFlagsMask |= CacheSyncFlushDbData | CacheSyncFlushDbMd;
        pBarrierOps->caches.flushDb = 1;
        pBarrierOps->caches.flushDbMetadata = 1;
    }

    if (flushTcc)
    {
        cacheSyncFlagsMask |= CacheSyncFlushTcc;
        pBarrierOps->caches.flushTcc = 1;
    }

    return cacheSyncFlagsMask;
}

// =====================================================================================================================
// Fill in a given BarrierOperations struct with info about a layout transition
static BarrierTransition AcqRelBuildTransition(
    const ImgBarrier*             pBarrier,
    HwLayoutTransition            transition,
    bool                          isSecondPass,
    Developer::BarrierOperations* pBarrierOps)
{
    switch (transition)
    {
    case ExpandDepthStencil:
        pBarrierOps->layoutTransitions.depthStencilExpand = 1;
        break;
    case HwlExpandHtileHiZRange:
        pBarrierOps->layoutTransitions.htileHiZRangeExpand = 1;
        break;
    case ResummarizeDepthStencil:
        pBarrierOps->layoutTransitions.depthStencilResummarize = 1;
        break;
    case FastClearEliminate:
        pBarrierOps->layoutTransitions.fastClearEliminate = 1;
        break;
    case FmaskDecomprMsaaColorDecompr: // FmaskDecompress->MsaaColorDecompress
        if (isSecondPass == false)
        {
            pBarrierOps->layoutTransitions.fmaskDecompress = 1;
        }
        else
        {
            pBarrierOps->layoutTransitions.fmaskColorExpand = 1;
        }
        break;
    case FmaskDecompress:
        pBarrierOps->layoutTransitions.fmaskDecompress = 1;
        break;
    case DccDecomprMsaaColorDecompr:   // DccDecompress->MsaaColorDecompress
        if (isSecondPass == false)
        {
            pBarrierOps->layoutTransitions.dccDecompress = 1;
        }
        else
        {
            pBarrierOps->layoutTransitions.fmaskColorExpand = 1;
        }
        break;
    case DccDecompress:
        pBarrierOps->layoutTransitions.dccDecompress = 1;
        break;
    case MsaaColorDecompress:
        pBarrierOps->layoutTransitions.fmaskColorExpand = 1;
        break;
    case InitMaskRam:
        pBarrierOps->layoutTransitions.initMaskRam = 1;
        break;
    case None:
    default:
        PAL_NEVER_CALLED();
    }
    BarrierTransition out = {};
    out.srcCacheMask                 = pBarrier->srcAccessMask;
    out.dstCacheMask                 = pBarrier->dstAccessMask;
    out.imageInfo.pImage             = pBarrier->pImage;
    out.imageInfo.subresRange        = pBarrier->subresRange;
    out.imageInfo.oldLayout          = pBarrier->oldLayout;
    out.imageInfo.newLayout          = pBarrier->newLayout;
    out.imageInfo.pQuadSamplePattern = pBarrier->pQuadSamplePattern;

    return out;
}

// =====================================================================================================================
// Update command buffer dirty state from operations in an acquire
static void UpdateCmdBufStateFromAcquire(
    GfxCmdBuffer*                       pCmdBuf,
    const Developer::BarrierOperations& ops)
{
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 504
    const uint32  eopTsBottomOfPipe = ops.pipelineStalls.eopTsBottomOfPipe;
#else
    const uint32  eopTsBottomOfPipe = ops.pipelineStalls.waitOnEopTsBottomOfPipe;
#endif

    if (eopTsBottomOfPipe != 0)
    {
        pCmdBuf->SetGfxCmdBufGfxBltState(false);

        const bool didFlushOrInvalRb = (ops.caches.flushCb         || ops.caches.invalCb         ||
                                        ops.caches.flushCbMetadata || ops.caches.invalCbMetadata ||
                                        ops.caches.flushDb         || ops.caches.invalDb         ||
                                        ops.caches.flushDbMetadata || ops.caches.invalDbMetadata);
        if (didFlushOrInvalRb && (pCmdBuf->GetGfxCmdBufState().flags.gfxBltActive == false))
        {
            pCmdBuf->SetGfxCmdBufGfxBltWriteCacheState(false);
        }
    }
    if ((eopTsBottomOfPipe != 0) || ops.pipelineStalls.csPartialFlush)
    {
        pCmdBuf->SetGfxCmdBufCsBltState(false);
    }
    if (ops.caches.flushTcc && (pCmdBuf->GetGfxCmdBufState().flags.csBltActive == false))
    {
        pCmdBuf->SetGfxCmdBufCsBltWriteCacheState(false);
    }
    if (ops.caches.flushTcc && (pCmdBuf->GetGfxCmdBufState().flags.cpBltActive == false))
    {
        pCmdBuf->SetGfxCmdBufCpBltWriteCacheState(false);
    }
    if (ops.caches.invalTcc && (pCmdBuf->GetGfxCmdBufState().flags.cpBltActive == false))
    {
        pCmdBuf->SetGfxCmdBufCpMemoryWriteL2CacheStaleState(false);
    }
}

// =====================================================================================================================
// Translate acquire's accessMask to CoherCntl.
static uint32 Gfx10BuildAcquireCoherCntl(
    EngineType                    engineType,
    uint32                        accessMask,
    Developer::BarrierOperations* pBarrierOps)
{
    PAL_ASSERT(pBarrierOps != nullptr);
    // K$ and I$ and all previous tcCacheOp controlled caches are moved to GCR fields, set in CalcAcquireMemGcrCntl().
    regCP_COHER_CNTL cpCoherCntl = {};

    // We do direct conversion from accessMask to hardware cache sync bits to avoid the unnecessary transition to
    // cacheSyncFlags. We don't want transition gfx10's accessMask to cacheSyncFlags because cacheSyncFlags are Gfx9
    // style.
    const bool wbInvCbData = TestAnyFlagSet(accessMask, CoherCopy | CoherResolve | CoherClear | CoherColorTarget);
    const bool wbInvDb     = TestAnyFlagSet(accessMask, CoherCopy    |
                                                        CoherResolve |
                                                        CoherClear   |
                                                        CoherDepthStencilTarget);

    if (wbInvCbData)
    {
        cpCoherCntl.bits.CB_ACTION_ENA = 1;
        pBarrierOps->caches.invalCb    = 1;
        pBarrierOps->caches.flushCb    = 1;
    }
    if (wbInvDb)
    {
        cpCoherCntl.bits.DB_ACTION_ENA      = 1;
        pBarrierOps->caches.invalDb         = 1;
        pBarrierOps->caches.flushDb         = 1;
        pBarrierOps->caches.invalDbMetadata = 1;
        pBarrierOps->caches.flushDbMetadata = 1;
    }

    return cpCoherCntl.u32All;
}

// =====================================================================================================================
// Wrapper to call RPM's InitMaskRam to issues a compute shader blt to initialize the Mask RAM allocatons for an Image.
// Returns "true" if the compute engine was used for the InitMaskRam operation.
bool Device::AcqRelInitMaskRam(
    GfxCmdBuffer*      pCmdBuf,
    CmdStream*         pCmdStream,
    const ImgBarrier&  imgBarrier,
    HwLayoutTransition transition
    ) const
{
    // If the LayoutUninitializedTarget usage is set, no other usages should be set.
    PAL_ASSERT(TestAnyFlagSet(imgBarrier.oldLayout.usages, ~LayoutUninitializedTarget) == false);

    const auto& image       = static_cast<const Pal::Image&>(*imgBarrier.pImage);
    const auto& gfx9Image   = static_cast<const Gfx9::Image&>(*image.GetGfxImage());
    const auto& subresRange = imgBarrier.subresRange;

#if PAL_ENABLE_PRINTS_ASSERTS
    const auto& engineProps  = Parent()->EngineProperties().perEngine[pCmdBuf->GetEngineType()];
    const auto& createInfo   = image.GetImageCreateInfo();
    const bool  isWholeImage = image.IsFullSubResRange(subresRange);

    // This queue must support this barrier transition.
    PAL_ASSERT(engineProps.flags.supportsImageInitBarrier == 1);

    // By default, the entire image must be initialized in one go. Per-subres support can be requested
    // using an image flag as long as the queue supports it.
    PAL_ASSERT(isWholeImage || ((engineProps.flags.supportsImageInitPerSubresource == 1) &&
                                (createInfo.flags.perSubresInit == 1)));
#endif

    PAL_ASSERT(gfx9Image.HasColorMetaData() || gfx9Image.HasHtileData());

    return RsrcProcMgr().InitMaskRam(pCmdBuf, pCmdStream, gfx9Image, subresRange);
}

// =====================================================================================================================
// Issue the specified BLT operation(s) (i.e., decompress, resummarize) necessary to convert a depth/stencil image from
// one ImageLayout to another.
void Device::AcqRelDepthStencilTransition(
    GfxCmdBuffer*      pCmdBuf,
    const ImgBarrier&  imgBarrier,
    HwLayoutTransition transition
    ) const
{
    PAL_ASSERT(imgBarrier.pImage != nullptr);

    const auto& image = static_cast<const Pal::Image&>(*imgBarrier.pImage);

    if (transition == HwLayoutTransition::HwlExpandHtileHiZRange)
    {
        const auto& gfx9Image = static_cast<const Image&>(*image.GetGfxImage());

        // CS blit to resummarize Htile.
        RsrcProcMgr().HwlResummarizeHtileCompute(pCmdBuf, gfx9Image, imgBarrier.subresRange);
    }
    else
    {
        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuf->Allocator(), false);
        IMsaaState* pMsaaState = AcqRelBarrierMsaaState(this, pCmdBuf, &allocator, imgBarrier);

        if (pMsaaState != nullptr)
        {
            if (transition == HwLayoutTransition::ExpandDepthStencil)
            {
                RsrcProcMgr().ExpandDepthStencil(pCmdBuf,
                                                 image,
                                                 pMsaaState,
                                                 imgBarrier.pQuadSamplePattern,
                                                 imgBarrier.subresRange);
            }
            else
            {
                PAL_ASSERT(transition == HwLayoutTransition::ResummarizeDepthStencil);

                // DB blit to resummarize.
                RsrcProcMgr().ResummarizeDepthStencil(pCmdBuf,
                                                      image,
                                                      imgBarrier.newLayout,
                                                      pMsaaState,
                                                      imgBarrier.pQuadSamplePattern,
                                                      imgBarrier.subresRange);
            }

            pMsaaState->Destroy();
            PAL_SAFE_FREE(pMsaaState, &allocator);
        }
    }
}

// =====================================================================================================================
// Issue the specified BLT operation(s) (i.e., decompresses) necessary to convert a color image from one ImageLayout to
// another.
void Device::AcqRelColorTransition(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    const ImgBarrier&             imgBarrier,
    HwLayoutTransition            transition,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(imgBarrier.pImage != nullptr);

    const EngineType            engineType  = pCmdBuf->GetEngineType();
    const auto&                 image       = static_cast<const Pal::Image&>(*imgBarrier.pImage);
    const auto&                 gfx9Image   = static_cast<const Gfx9::Image&>(*image.GetGfxImage());
    const SubResourceInfo*const pSubresInfo = image.SubresourceInfo(imgBarrier.subresRange.startSubres);

    PAL_ASSERT(image.IsDepthStencil() == false);

    if (transition == HwLayoutTransition::MsaaColorDecompress)
    {
        RsrcProcMgr().FmaskColorExpand(pCmdBuf, gfx9Image, imgBarrier.subresRange);
    }
    else
    {
        LinearAllocatorAuto<VirtualLinearAllocator> allocator(pCmdBuf->Allocator(), false);
        IMsaaState* pMsaaState = AcqRelBarrierMsaaState(this, pCmdBuf, &allocator, imgBarrier);

        if (pMsaaState != nullptr)
        {
            if ((transition == HwLayoutTransition::DccDecompress) ||
                (transition == HwLayoutTransition::DccDecomprMsaaColorDecompr))
            {
                RsrcProcMgr().DccDecompress(pCmdBuf,
                                            pCmdStream,
                                            gfx9Image,
                                            pMsaaState,
                                            imgBarrier.pQuadSamplePattern,
                                            imgBarrier.subresRange);
            }
            else if ((transition == HwLayoutTransition::FmaskDecompress) ||
                     (transition == HwLayoutTransition::FmaskDecomprMsaaColorDecompr))
            {
                RsrcProcMgr().FmaskDecompress(pCmdBuf,
                                              pCmdStream,
                                              gfx9Image,
                                              pMsaaState,
                                              imgBarrier.pQuadSamplePattern,
                                              imgBarrier.subresRange);
            }
            else
            {
                PAL_ASSERT(transition == HwLayoutTransition::FastClearEliminate);

                // Note: if FCE is not submitted to GPU, we don't need to update cache flags.
                bool isSubmitted = RsrcProcMgr().FastClearEliminate(pCmdBuf,
                                                                    pCmdStream,
                                                                    gfx9Image,
                                                                    pMsaaState,
                                                                    imgBarrier.pQuadSamplePattern,
                                                                    imgBarrier.subresRange);
            }

            pMsaaState->Destroy();
            PAL_SAFE_FREE(pMsaaState, &allocator);
        }
    }

    // Handle corner cases where it needs a second pass.
    if ((transition == HwLayoutTransition::FmaskDecomprMsaaColorDecompr) ||
        (transition == HwLayoutTransition::DccDecomprMsaaColorDecompr))
    {
        uint32 stageMask  = 0;
        uint32 accessMask = 0;

        GetBltStageAccessInfo(transition, pCmdBuf, gfx9Image, imgBarrier.subresRange, &stageMask, &accessMask);

        const IGpuEvent* pEvent = pCmdBuf->GetInternalEvent();
        pCmdBuf->CmdResetEvent(*pEvent, HwPipePostIndexFetch);

        // Release for first pass.
        IssueReleaseSync(pCmdBuf, pCmdStream, stageMask, accessMask, false, pEvent, pBarrierOps);

        // Acquire for second pass.
        IssueAcquireSync(pCmdBuf,
                         pCmdStream,
                         stageMask,
                         accessMask,
                         false,
                         FullSyncBaseAddr,
                         FullSyncSize,
                         1,
                         &pEvent,
                         pBarrierOps);

        // Tell RGP about this transition
        BarrierTransition rgpTransition = AcqRelBuildTransition(&imgBarrier, transition, true, pBarrierOps);
        DescribeBarrier(pCmdBuf, &rgpTransition, pBarrierOps);

        // And clear it so it can differentiate sync and async flushes
        *pBarrierOps = {};

        RsrcProcMgr().FmaskColorExpand(pCmdBuf, gfx9Image, imgBarrier.subresRange);
    }
}

// =====================================================================================================================
// Issue appropriate cache sync hardware commands to satisfy the cache release requirements.
void Device::IssueReleaseSync(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    uint32                        stageMask,
    uint32                        accessMask,
    bool                          flushLlc,
    const IGpuEvent*              pGpuEvent,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    // Validate input.
    PAL_ASSERT(stageMask != 0);
    PAL_ASSERT(pGpuEvent != nullptr);
    PAL_ASSERT(pBarrierOps != nullptr);

    const EngineType engineType      = pCmdBuf->GetEngineType();
    const GpuEvent*  pEvent          = static_cast<const GpuEvent*>(pGpuEvent);
    const gpusize    gpuEventStartVa = pEvent->GetBoundGpuMemory().GpuVirtAddr();

    uint32* pCmdSpace = pCmdStream->ReserveCommands();

    if (pCmdBuf->GetGfxCmdBufState().flags.cpBltActive && TestAnyFlagSet(stageMask, PipelineStageBlt))
    {
        // We must guarantee that all prior CP DMA accelerated blts have completed before we write this event because
        // the CmdSetEvent and CmdResetEvent functions expect that the prior blts have reached the post-blt stage by
        // the time the event is written to memory. Given that our CP DMA blts are asynchronous to the pipeline stages
        // the only way to satisfy this requirement is to force the MEC to stall until the CP DMAs are completed.
        pBarrierOps->pipelineStalls.syncCpDma = 1;
        pCmdSpace += m_cmdUtil.BuildWaitDmaData(pCmdSpace);
        pCmdBuf->SetGfxCmdBufCpBltState(false);
    }

    // Converts PipelineStageBlt stage to specific internal pipeline stage.
    stageMask  = pCmdBuf->ConvertToInternalPipelineStageMask(stageMask);
    accessMask = OptimizeBltCacheAccess(pCmdBuf, accessMask);

    if (pCmdBuf->IsGraphicsSupported() == false)
    {
        stageMask &= ~GraphicsOnlyPipeStages;
    }

    pCmdSpace += BuildReleaseSyncPackets(engineType,
                                         stageMask,
                                         accessMask,
                                         flushLlc,
                                         gpuEventStartVa,
                                         pCmdSpace,
                                         pBarrierOps);

    pCmdStream->CommitCommands(pCmdSpace);
}

// =====================================================================================================================
// Issue appropriate cache sync hardware commands to satisfy the cache acquire requirements.
void Device::IssueAcquireSync(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    uint32                        stageMask,
    uint32                        accessMask,
    bool                          invalidateLlc,
    gpusize                       rangeStartAddr,
    gpusize                       rangeSize,
    uint32                        gpuEventCount,
    const IGpuEvent*const*        ppGpuEvents,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(pBarrierOps != nullptr);

    const EngineType engineType     = pCmdBuf->GetEngineType();
    const bool       isGfxSupported = Pal::Device::EngineSupportsGraphics(engineType);

    if (isGfxSupported == false)
    {
        stageMask &= ~GraphicsOnlyPipeStages;
    }

    // BuildWaitRegMem waits in the ME, if the waitPoint needs to stall at the PFP request a PFP/ME sync.
    const bool pfpSyncMe = TestAnyFlagSet(stageMask, PipelineStageTopOfPipe         |
                                                     PipelineStageFetchIndirectArgs |
                                                     PipelineStageFetchIndices);

    uint32* pCmdSpace = pCmdStream->ReserveCommands();

    // Wait on the GPU memory slot(s) in all specified IGpuEvent objects.
    if (gpuEventCount != 0)
    {
        PAL_ASSERT(ppGpuEvents != nullptr);
        pBarrierOps->pipelineStalls.waitOnTs = 1;

        for (uint32 i = 0; i < gpuEventCount; i++)
        {
            const uint32    numEventSlots   = Parent()->ChipProperties().gfxip.numSlotsPerEvent;
            const GpuEvent* pGpuEvent       = static_cast<const GpuEvent*>(ppGpuEvents[i]);
            const gpusize   gpuEventStartVa = pGpuEvent->GetBoundGpuMemory().GpuVirtAddr();

            for (uint32 slotIdx = 0; slotIdx < numEventSlots; slotIdx++)
            {
                pCmdSpace += m_cmdUtil.BuildWaitRegMem(engineType,
                                                       mem_space__me_wait_reg_mem__memory_space,
                                                       function__me_wait_reg_mem__equal_to_the_reference_value,
                                                       engine_sel__me_wait_reg_mem__micro_engine,
                                                       gpuEventStartVa + (sizeof(uint32) * slotIdx),
                                                       GpuEvent::SetValue,
                                                       0xFFFFFFFF,
                                                       pCmdSpace);
            }
        }
    }

    if (accessMask != 0)
    {
        accessMask = OptimizeBltCacheAccess(pCmdBuf, accessMask);
        pCmdSpace += BuildAcquireSyncPackets(engineType,
                                             stageMask,
                                             accessMask,
                                             invalidateLlc,
                                             rangeStartAddr,
                                             rangeSize,
                                             pCmdSpace,
                                             pBarrierOps);
    }

    if (pfpSyncMe && isGfxSupported)
    {
        // Stalls the CP PFP until the ME has processed all previous commands.  Useful in cases where the ME is waiting
        // on some condition, but the PFP needs to stall execution until the condition is satisfied.  This must go last
        // otherwise the PFP could resume execution before the ME is done with all of its waits.
        pCmdSpace += m_cmdUtil.BuildPfpSyncMe(pCmdSpace);
        pBarrierOps->pipelineStalls.pfpSyncMe = 1;
    }

    UpdateCmdBufStateFromAcquire(pCmdBuf, *pBarrierOps);

    pCmdStream->CommitCommands(pCmdSpace);
}

// =====================================================================================================================
// Figure out the specific BLT operation(s) necessary to convert a color image from one ImageLayout to another.
HwLayoutTransition Device::ConvertToColorBlt(
    const GfxCmdBuffer* pCmdBuf,
    const Pal::Image&   image,
    const SubresRange&  subresRange,
    ImageLayout         oldLayout,
    ImageLayout         newLayout
    ) const
{
    const auto&                 gfx9Image   = static_cast<const Gfx9::Image&>(*image.GetGfxImage());
    const SubResourceInfo*const pSubresInfo = image.SubresourceInfo(subresRange.startSubres);

    const ColorLayoutToState    layoutToState = gfx9Image.LayoutToColorCompressionState();
    const ColorCompressionState oldState      =
        ImageLayoutToColorCompressionState(layoutToState, oldLayout);
    const ColorCompressionState newState      =
        ImageLayoutToColorCompressionState(layoutToState, newLayout);

    // Fast clear eliminates are only possible on universal queue command buffers and will be ignored on others. This
    // should be okay because prior operations should be aware of this fact (based on layout), and prohibit us from
    // getting to a situation where one is needed but has not been performed yet.
    const bool fastClearEliminateSupported = pCmdBuf->IsGraphicsSupported();
    const bool isMsaaImage                 = (image.GetImageCreateInfo().samples > 1);

    HwLayoutTransition transition = HwLayoutTransition::None;

    if ((oldState != ColorDecompressed) && (newState == ColorDecompressed))
    {
        if (gfx9Image.HasDccData())
        {
            if ((oldState == ColorCompressed) || pSubresInfo->flags.supportMetaDataTexFetch)
            {
                transition = isMsaaImage ? HwLayoutTransition::DccDecomprMsaaColorDecompr :
                                           HwLayoutTransition::DccDecompress;
            }
        }
        else if (isMsaaImage)
        {
            // Need FmaskDecompress in preparation for the following full MSAA color decompress.
            transition = (oldState == ColorCompressed) ? HwLayoutTransition::FmaskDecomprMsaaColorDecompr :
                                                         HwLayoutTransition::MsaaColorDecompress;
        }
        else
        {
            // Not Dcc image, nor Msaa image.
            PAL_ASSERT(oldState == ColorCompressed);
            transition = fastClearEliminateSupported ? HwLayoutTransition::FastClearEliminate :
                                                       HwLayoutTransition::None;
        }
    }
    else if ((oldState == ColorCompressed) && (newState == ColorFmaskDecompressed))
    {
        PAL_ASSERT(isMsaaImage);
        if (pSubresInfo->flags.supportMetaDataTexFetch == false)
        {
            if (gfx9Image.HasDccData())
            {
                // If the base pixel data is DCC compressed, but the image can't support metadata texture fetches,
                // we need a DCC decompress.  The DCC decompress effectively executes an fmask decompress
                // implicitly.
                transition = HwLayoutTransition::DccDecompress;
            }
            else
            {
                transition = HwLayoutTransition::FmaskDecompress;
            }
        }
        else
        {
            // if the image is TC compatible just need to do a fast clear eliminate
            transition = fastClearEliminateSupported ? HwLayoutTransition::FastClearEliminate :
                                                       HwLayoutTransition::None;
        }
    }
    else if ((oldState == ColorCompressed) && (newState == ColorCompressed))
    {
        // This case indicates that the layout capabilities changed, but the color image is able to remain in
        // the compressed state.  If the image is about to be read, we may need to perform a fast clear
        // eliminate BLT if the clear color is not texture compatible.  This BLT will end up being skipped on
        // the GPU side if the latest clear color was supported by the texture hardware (i.e., black or white).
        constexpr uint32 TcCompatReadFlags = LayoutShaderRead           |
                                             LayoutShaderFmaskBasedRead |
                                             LayoutCopySrc              |
                                             LayoutResolveSrc;

        if (fastClearEliminateSupported                         &&
            TestAnyFlagSet(newLayout.usages, TcCompatReadFlags) &&
            (gfx9Image.HasDccData() && pSubresInfo->flags.supportMetaDataTexFetch))
        {
            transition = HwLayoutTransition::FastClearEliminate;
        }
    }

    return transition;
}

// =====================================================================================================================
// Figure out the specific BLT operation(s) necessary to convert a depth/stencil image from one ImageLayout to another.
HwLayoutTransition Device::ConvertToDepthStencilBlt(
    const GfxCmdBuffer* pCmdBuf,
    const Pal::Image&   image,
    const SubresRange&  subresRange,
    ImageLayout         oldLayout,
    ImageLayout         newLayout
    ) const
{
    const auto& gfx9Image = static_cast<const Image&>(*image.GetGfxImage());

    const DepthStencilLayoutToState    layoutToState =
        gfx9Image.LayoutToDepthCompressionState(subresRange.startSubres);
    const DepthStencilCompressionState oldState =
        ImageLayoutToDepthCompressionState(layoutToState, oldLayout);
    const DepthStencilCompressionState newState =
        ImageLayoutToDepthCompressionState(layoutToState, newLayout);

    HwLayoutTransition transition = HwLayoutTransition::None;

    if ((oldState == DepthStencilCompressed) && (newState != DepthStencilCompressed))
    {
        transition = HwLayoutTransition::ExpandDepthStencil;
    }
    // Resummarize the htile values from the depth-stencil surface contents when transitioning from "HiZ invalid"
    // state to something that uses HiZ.
    else if ((oldState == DepthStencilDecomprNoHiZ) && (newState != DepthStencilDecomprNoHiZ))
    {
        // Use compute if:
        //   - We're on the compute engine
        //   - or we should force ExpandHiZRange for resummarize and we support compute operations
        //   - or we have a workaround which indicates if we need to use the compute path.
        const auto& createInfo = image.GetImageCreateInfo();
        const bool  z16Unorm1xAaDecompressUninitializedActive =
            (Settings().waZ16Unorm1xAaDecompressUninitialized &&
             (createInfo.samples == 1) &&
             ((createInfo.swizzledFormat.format == ChNumFormat::X16_Unorm) ||
              (createInfo.swizzledFormat.format == ChNumFormat::D16_Unorm_S8_Uint)));
        const bool  useCompute = ((pCmdBuf->GetEngineType() == EngineTypeCompute) ||
                                  (pCmdBuf->IsComputeSupported() &&
                                   (Pal::Image::ForceExpandHiZRangeForResummarize ||
                                    z16Unorm1xAaDecompressUninitializedActive)));
        if (useCompute)
        {
            // CS blit to open-up the HiZ range.
            transition = HwLayoutTransition::HwlExpandHtileHiZRange;
        }
        else
        {
            transition = HwLayoutTransition::ResummarizeDepthStencil;
        }
    }

    return transition;
}

// =====================================================================================================================
// Helper function that figures out what BLT transition is needed based on the image's old and new layout.
HwLayoutTransition Device::ConvertToBlt(
    const GfxCmdBuffer* pCmdBuf,
    const ImgBarrier&   imgBarrier
    ) const
{
    // At least one usage must be specified for the old and new layouts.
    PAL_ASSERT((imgBarrier.oldLayout.usages != 0) && (imgBarrier.newLayout.usages != 0));

    // With the exception of a transition out of the uninitialized state, at least one queue type must be
    // valid for every layout.
    PAL_ASSERT(((imgBarrier.oldLayout.usages == LayoutUninitializedTarget) ||
                (imgBarrier.oldLayout.engines != 0)) &&
                (imgBarrier.newLayout.engines != 0));

    PAL_ASSERT(imgBarrier.pImage != nullptr);

    const ImageLayout oldLayout   = imgBarrier.oldLayout;
    const ImageLayout newLayout   = imgBarrier.newLayout;
    const auto&       image       = static_cast<const Pal::Image&>(*imgBarrier.pImage);
    const auto&       subresRange = imgBarrier.subresRange;

    HwLayoutTransition transition = HwLayoutTransition::None;

    if (TestAnyFlagSet(oldLayout.usages, LayoutUninitializedTarget))
    {
        // If the LayoutUninitializedTarget usage is set, no other usages should be set.
        PAL_ASSERT(TestAnyFlagSet(oldLayout.usages, ~LayoutUninitializedTarget) == false);

        const auto& gfx9Image   = static_cast<const Gfx9::Image&>(*image.GetGfxImage());

#if PAL_ENABLE_PRINTS_ASSERTS
        const auto& engineProps  = Parent()->EngineProperties().perEngine[pCmdBuf->GetEngineType()];
        const auto& createInfo   = image.GetImageCreateInfo();
        const bool  isWholeImage = image.IsFullSubResRange(subresRange);

        // This queue must support this barrier transition.
        PAL_ASSERT(engineProps.flags.supportsImageInitBarrier == 1);

        // By default, the entire image must be initialized in one go. Per-subres support can be requested
        // using an image flag as long as the queue supports it.
        PAL_ASSERT(isWholeImage || ((engineProps.flags.supportsImageInitPerSubresource == 1) &&
                                    (createInfo.flags.perSubresInit == 1)));
#endif

        if (gfx9Image.HasColorMetaData() || gfx9Image.HasHtileData())
        {
            transition = HwLayoutTransition::InitMaskRam;
        }
    }
    else if (TestAnyFlagSet(newLayout.usages, LayoutUninitializedTarget))
    {
        // If the LayoutUninitializedTarget usage is set, no other usages should be set.
        PAL_ASSERT(TestAnyFlagSet(newLayout.usages, ~LayoutUninitializedTarget) == false);

        // We do no decompresses, expands, or any other kind of blt in this case.
    }
    else if ((TestAnyFlagSet(oldLayout.usages, LayoutUninitializedTarget) == false) &&
             (TestAnyFlagSet(newLayout.usages, LayoutUninitializedTarget) == false))
    {
        // Call helper function to calculate specific BLT operation(s) (can be none) for an image layout transition.
        if (image.IsDepthStencil())
        {
            transition = ConvertToDepthStencilBlt(pCmdBuf, image, subresRange, oldLayout, newLayout);
        }
        else
        {
            transition = ConvertToColorBlt(pCmdBuf, image, subresRange, oldLayout, newLayout);
        }
    }

    return transition;
}

// =====================================================================================================================
//  We will need flush & inv L2 on MSAA Z, MSAA color, mips in the metadata tail, or any stencil.
//
// The driver assumes that all meta-data surfaces are pipe-aligned, but there are cases where the HW does not actually
// pipe-align the data.  In these cases, the L2 cache needs to be flushed prior to the metadata being read by a shader.
bool Device::WaRefreshTccToAlignMetadata(
    const ImgBarrier& imgBarrier,
    uint32            srcAccessMask,
    uint32            dstAccessMask
    ) const
{
    const auto& image     = static_cast<const Pal::Image&>(*imgBarrier.pImage);
    const auto& gfx9Image = static_cast<const Image&>(*image.GetGfxImage());

    bool needRefreshL2 = false;

    if (gfx9Image.NeedFlushForMetadataPipeMisalignment(imgBarrier.subresRange))
    {
        if ((srcAccessMask == 0) || (dstAccessMask == 0))
        {
            // 1. If release's dstAccessMask or acquire's srcAccessMask is zero, that means we're at the edge of a
            //    split barrier, and the future/past usage is unknown. In such case we need to assume the src and dst
            //    caches can be cross front/backend, so refresh L2 in this case.
            // 2. Both sides being zero is a valid case. For example a transition from CopySrc to DepthStencil layout
            //    is from DepthStencilDecomprWithHiZ to DepthStencilCompressed, no decompress or resummarize is needed.
            //    So BLT's accessMask is zero. CopySrc doesn't need to flush data when release from it, so
            //    srcAccessMask is zero too. In such case, we know that the metadata must have been in correct
            //    alignment to frontend to make sure CopySrc reads from the correct L2 bank. So we still need an LLC
            //    refresh to ensure the later DepthStencil work sees the metadata in its L2 bank as invalidated then
            //    pulls it from memory.
            needRefreshL2 = true;
        }
        else
        {
            // Because we are not able to convert CoherCopy, CoherClear, CoherResolve to specific frontend or backend
            // coherency flags, we cannot make accurate decision here. This code works hard to not over-sync too much.
            constexpr uint32 ShaderOnlyMask             = CoherShader;
            constexpr uint32 TargetOnlyMask             = CoherColorTarget | CoherDepthStencilTarget;
            constexpr uint32 MaybeShaderMaybeTargetMask = CoherCopy | CoherResolve | CoherClear;

            if ((TestAnyFlagSet(srcAccessMask, ShaderOnlyMask) &&
                 TestAnyFlagSet(dstAccessMask, TargetOnlyMask | MaybeShaderMaybeTargetMask)) ||
                (TestAnyFlagSet(srcAccessMask, ShaderOnlyMask | MaybeShaderMaybeTargetMask) &&
                 TestAnyFlagSet(dstAccessMask, TargetOnlyMask)))
            {
                needRefreshL2 = true;
            }
        }
    }

    return needRefreshL2;
}

// =====================================================================================================================
// Look up for the stage and access mask associate with the transition.
void Device::GetBltStageAccessInfo(
    HwLayoutTransition  transition,
    const GfxCmdBuffer* pCmdBuffer,
    const Image&        gfxImage,
    const SubresRange&  range,
    uint32*             pStageMask,
    uint32*             pAccessMask
    ) const
{
    uint32 stageMask  = 0;
    uint32 accessMask = 0;

    switch (transition)
    {
    // Depth/Stencil transitions.
    case HwLayoutTransition::ExpandDepthStencil:
        if (RsrcProcMgr().WillDecompressWithCompute(pCmdBuffer, gfxImage, range))
        {
            stageMask  = PipelineStageCs;
            accessMask = CoherShader;
        }
        else
        {
            stageMask  = PipelineStageEarlyDsTarget;
            accessMask = CoherDepthStencilTarget;
        }
        break;

    case HwLayoutTransition::HwlExpandHtileHiZRange:
        stageMask  = PipelineStageCs;
        accessMask = CoherShader;
        break;

    case HwLayoutTransition::ResummarizeDepthStencil:
        stageMask  = PipelineStageEarlyDsTarget;
        accessMask = CoherDepthStencilTarget;
        break;

    // Color Transitions.
    case HwLayoutTransition::FastClearEliminate:
        // Fallthrough as FastClearEliminate and FmaskDecompress involve same stageMask and accessMask.
    case HwLayoutTransition::FmaskDecompress:
        stageMask  = PipelineStageColorTarget;
        accessMask = CoherColorTarget;
        break;

    case HwLayoutTransition::DccDecompress:
        if (RsrcProcMgr().WillDecompressWithCompute(pCmdBuffer, gfxImage, range))
        {
            stageMask  = PipelineStageCs;
            accessMask = CoherShader;
        }
        else
        {
            stageMask  = PipelineStageColorTarget;
            accessMask = CoherColorTarget;
        }
        break;

    case HwLayoutTransition::MsaaColorDecompress:
        stageMask  = PipelineStageCs;
        accessMask = CoherShader;
        break;

    // Two-pass transition's acquire and release have different masks.
    case HwLayoutTransition::FmaskDecomprMsaaColorDecompr:
        stageMask  = PipelineStageColorTarget | PipelineStageCs;
        accessMask = CoherColorTarget | CoherShader;
        break;

    // Two-pass transition's acquire and release have different masks. DccDecompress can be done by either graphics or
    // CS, FmaskColorExpand is always done by CS.
    case HwLayoutTransition::DccDecomprMsaaColorDecompr:
        if (RsrcProcMgr().WillDecompressWithCompute(pCmdBuffer, gfxImage, range) == false)
        {
            stageMask  = PipelineStageColorTarget;
            accessMask = CoherColorTarget;
        }
        stageMask  |= PipelineStageCs;
        accessMask |= CoherShader;
        break;

    case HwLayoutTransition::InitMaskRam:
        stageMask  = PipelineStageCs;
        accessMask = CoherShader;
        break;

    case HwLayoutTransition::None:
        // Do nothing.
        break;

    default:
        PAL_NEVER_CALLED();
        break;
    }

    if (pStageMask != nullptr)
    {
        *pStageMask = stageMask;
    }

    if (pAccessMask != nullptr)
    {
        *pAccessMask = accessMask;
    }
}

// =====================================================================================================================
// BarrierRelease perform any necessary layout transition, availability operation, and enqueue command(s) to set a given
// IGpuEvent object once the prior operations' intersection with the given synchronization scope is confirmed complete.
// The availability operation will flush the requested local caches.
void Device::BarrierRelease(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    const AcquireReleaseInfo&     barrierReleaseInfo,
    const IGpuEvent*              pClientEvent,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    // Validate input data.
    PAL_ASSERT(barrierReleaseInfo.dstStageMask == 0);
    PAL_ASSERT(barrierReleaseInfo.dstGlobalAccessMask == 0);
    for (uint32 i = 0; i < barrierReleaseInfo.memoryBarrierCount; i++)
    {
        PAL_ASSERT(barrierReleaseInfo.pMemoryBarriers[i].dstAccessMask == 0);
    }
    for (uint32 i = 0; i < barrierReleaseInfo.imageBarrierCount; i++)
    {
        PAL_ASSERT(barrierReleaseInfo.pImageBarriers[i].dstAccessMask == 0);
    }

    const uint32 preBltStageMask   = barrierReleaseInfo.srcStageMask;
    uint32       preBltAccessMask  = barrierReleaseInfo.srcGlobalAccessMask;
    bool         globallyAvailable = false;
    bool         waRefreshLlc      = false; // Coarse-grain refresh LLC flag.

    // Assumes always do full-range flush sync.
    for (uint32 i = 0; i < barrierReleaseInfo.memoryBarrierCount; i++)
    {
        const MemBarrier& barrier = barrierReleaseInfo.pMemoryBarriers[i];

        preBltAccessMask  |= barrier.srcAccessMask;
        globallyAvailable |= barrier.flags.globallyAvailable;
    }

    // A container to cache the calculated BLT transitions and some cache info for reuse.
    AutoBuffer<AcqRelTransitionInfo, 8, Platform> transitionList(barrierReleaseInfo.imageBarrierCount, GetPlatform());
    uint32 bltTransitionCount = 0;

    Result result = Result::Success;

    if (transitionList.Capacity() < barrierReleaseInfo.imageBarrierCount)
    {
        pCmdBuf->NotifyAllocFailure();
    }
    else
    {
        // Loop through image transitions to update client requested access.
        for (uint32 i = 0; i < barrierReleaseInfo.imageBarrierCount; i++)
        {
            const ImgBarrier& imageBarrier = barrierReleaseInfo.pImageBarriers[i];

            // Update client requested access mask.
            preBltAccessMask |= imageBarrier.srcAccessMask;

            // Check if we need to do transition BLTs.
            HwLayoutTransition transition = ConvertToBlt(pCmdBuf, imageBarrier);

            transitionList[i].pImgBarrier      = &imageBarrier;
            transitionList[i].transition       = transition;
            transitionList[i].waNeedRefreshLlc = false;

            uint32 bltStageMask  = 0;
            uint32 bltAccessMask = 0;

            if (transition != HwLayoutTransition::None)
            {
                const auto& image       = static_cast<const Pal::Image&>(*imageBarrier.pImage);
                const auto& gfx9Image   = static_cast<const Image&>(*image.GetGfxImage());
                const auto& subresRange = imageBarrier.subresRange;

                GetBltStageAccessInfo(transition, pCmdBuf, gfx9Image, subresRange, &bltStageMask, &bltAccessMask);

                transitionList[i].bltStageMask  = bltStageMask;
                transitionList[i].bltAccessMask = bltAccessMask;
                bltTransitionCount++;
            }
            else
            {
                transitionList[i].bltStageMask  = 0;
                transitionList[i].bltAccessMask = 0;
            }

            if (WaRefreshTccToAlignMetadata(imageBarrier, imageBarrier.srcAccessMask, bltAccessMask))
            {
                transitionList[i].waNeedRefreshLlc = true;
                waRefreshLlc = true;
            }
        }

        // Initialize an IGpuEvent* pEvent pointing at the client provided event.
        // If we have internal BLT(s), use internal event to signal/wait.
        const IGpuEvent* pActiveEvent = (bltTransitionCount > 0) ? pCmdBuf->GetInternalEvent() : pClientEvent;

        if (pActiveEvent != nullptr)
        {
            pCmdBuf->CmdResetEvent(*pActiveEvent, HwPipePostIndexFetch);
        }

        // Perform an all-in-one release prior to the potential BLT(s): IssueReleaseSync() on pActiveEvent.
        IssueReleaseSync(pCmdBuf,
                         pCmdStream,
                         preBltStageMask,
                         preBltAccessMask,
                         globallyAvailable | waRefreshLlc,
                         pActiveEvent,
                         pBarrierOps);

        // Issue BLT(s) if there exists transitions that require one.
        if (bltTransitionCount > 0)
        {
            // If BLT(s) will be issued, we need to know how to release from it/them.
            uint32 postBltStageMask  = 0;
            uint32 postBltAccessMask = 0;
            bool   needEventWait     = true;

            // Issue pre-BLT acquires.
            for (uint32 i = 0; i < barrierReleaseInfo.imageBarrierCount; i++)
            {
                if (transitionList[i].transition != HwLayoutTransition::None)
                {
                    const auto& image = static_cast<const Pal::Image&>(*transitionList[i].pImgBarrier->pImage);

                    //- Issue an acquire on pEvent with the stageMask/scopeMask.
                    IssueAcquireSync(pCmdBuf,
                                     pCmdStream,
                                     transitionList[i].bltStageMask,
                                     transitionList[i].bltAccessMask,
                                     transitionList[i].waNeedRefreshLlc,
                                     image.GetGpuVirtualAddr(),
                                     image.GetGpuMemSize(),
                                     (needEventWait ? 1 : 0),
                                     &pActiveEvent,
                                     pBarrierOps);
                    needEventWait = false;
                }
            }

            // Issue BLTs.
            for (uint32 i = 0; i < barrierReleaseInfo.imageBarrierCount; i++)
            {
                if (transitionList[i].transition != HwLayoutTransition::None)
                {
                    IssueBlt(pCmdBuf,
                             pCmdStream,
                             transitionList[i].pImgBarrier,
                             transitionList[i].transition,
                             pBarrierOps);

                    // Add current BLT's stageMask/accessMask into a stageMask/accessMask used for an all-in-one
                    // post-BLT release.
                    postBltStageMask  |= transitionList[i].bltStageMask;
                    postBltAccessMask |= transitionList[i].bltAccessMask;
                }
            }

            // Get back the client provided event and signal it when the whole barrier-release is done.
            pActiveEvent = pClientEvent;

            if (pActiveEvent != nullptr)
            {
                pCmdBuf->CmdResetEvent(*pActiveEvent, HwPipePostIndexFetch);
            }

            // Release from BLTs.
            IssueReleaseSync(pCmdBuf,
                             pCmdStream,
                             postBltStageMask,
                             postBltAccessMask,
                             waRefreshLlc,
                             pActiveEvent,
                             pBarrierOps);
        }
    }
}

// =====================================================================================================================
// BarrierAcquire will wait on the specified IGpuEvent object to be signaled, perform any necessary layout transition,
// and issue the required visibility operations. The visibility operation will invalidate the required ranges in local
// caches.
void Device::BarrierAcquire(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    const AcquireReleaseInfo&     barrierAcquireInfo,
    uint32                        gpuEventCount,
    const IGpuEvent*const*        ppGpuEvents,
    Developer::BarrierOperations* pBarrierOps
) const
{
    // Validate input data.
    PAL_ASSERT(barrierAcquireInfo.srcStageMask == 0);
    PAL_ASSERT(barrierAcquireInfo.srcGlobalAccessMask == 0);
    for (uint32 i = 0; i < barrierAcquireInfo.memoryBarrierCount; i++)
    {
        PAL_ASSERT(barrierAcquireInfo.pMemoryBarriers[i].srcAccessMask == 0);
    }
    for (uint32 i = 0; i < barrierAcquireInfo.imageBarrierCount; i++)
    {
        PAL_ASSERT(barrierAcquireInfo.pImageBarriers[i].srcAccessMask == 0);
    }

    bool waRefreshLlc = false; // Coarse-grain refresh LLC flag.

    // A container to cache the calculated BLT transitions and some cache info for reuse.
    AutoBuffer<AcqRelTransitionInfo, 8, Platform> transitionList(barrierAcquireInfo.imageBarrierCount, GetPlatform());
    uint32 bltTransitionCount = 0;

    if (transitionList.Capacity() >= barrierAcquireInfo.imageBarrierCount)
    {
        // Acquire for BLTs.
        for (uint32 i = 0; i < barrierAcquireInfo.imageBarrierCount; i++)
        {
            const auto& imgBarrier = barrierAcquireInfo.pImageBarriers[i];

            // Check if we need to do transition BLTs.
            const HwLayoutTransition transition = ConvertToBlt(pCmdBuf, imgBarrier);

            transitionList[i].pImgBarrier      = &imgBarrier;
            transitionList[i].transition       = transition;
            transitionList[i].waNeedRefreshLlc = false;

            uint32 bltStageMask  = 0;
            uint32 bltAccessMask = 0;

            if (transition != HwLayoutTransition::None)
            {
                const auto& image       = static_cast<const Pal::Image&>(*imgBarrier.pImage);
                const auto& gfx9Image   = static_cast<const Image&>(*image.GetGfxImage());
                const auto& subresRange = imgBarrier.subresRange;

                GetBltStageAccessInfo(transition, pCmdBuf, gfx9Image, subresRange, &bltStageMask, &bltAccessMask);

                transitionList[i].bltStageMask  = bltStageMask;
                transitionList[i].bltAccessMask = bltAccessMask;
                bltTransitionCount++;

                if (WaRefreshTccToAlignMetadata(imgBarrier, bltAccessMask, imgBarrier.dstAccessMask))
                {
                    transitionList[i].waNeedRefreshLlc = true;
                    waRefreshLlc = true;
                }
            }
            else
            {
                transitionList[i].bltStageMask  = 0;
                transitionList[i].bltAccessMask = 0;
            }
        }

        const IGpuEvent*const* ppActiveEvents   = ppGpuEvents;
        uint32                 activeEventCount = gpuEventCount;

        if (bltTransitionCount > 0)
        {
            // If BLT(s) will be issued, we need to know how to release from it/them.
            uint32 postBltStageMask  = 0;
            uint32 postBltAccessMask = 0;
            bool   needEventWait     = true;

            // Issue pre-BLT acquires.
            for (uint32 i = 0; i < barrierAcquireInfo.imageBarrierCount; i++)
            {
                if (transitionList[i].transition != HwLayoutTransition::None)
                {
                    const auto& image = static_cast<const Pal::Image&>(*transitionList[i].pImgBarrier->pImage);

                    //- Issue an acquire on pEvent with the stageMask/scopeMask.
                    IssueAcquireSync(pCmdBuf,
                                     pCmdStream,
                                     transitionList[i].bltStageMask,
                                     transitionList[i].bltAccessMask,
                                     transitionList[i].waNeedRefreshLlc,
                                     image.GetGpuVirtualAddr(),
                                     image.GetGpuMemSize(),
                                     (needEventWait ? activeEventCount : 0),
                                     ppActiveEvents,
                                     pBarrierOps);
                    needEventWait = false;
                }
            }

            // Issue BLTs.
            for (uint32 i = 0; i < barrierAcquireInfo.imageBarrierCount; i++)
            {
                if (transitionList[i].transition != HwLayoutTransition::None)
                {
                    IssueBlt(pCmdBuf,
                             pCmdStream,
                             transitionList[i].pImgBarrier,
                             transitionList[i].transition,
                             pBarrierOps);

                    // Add current BLT's stageMask/accessMask into a stageMask/accessMask used for an all-in-one
                    // post-BLT release.
                    postBltStageMask  |= transitionList[i].bltStageMask;
                    postBltAccessMask |= transitionList[i].bltAccessMask;
                }
            }

            // We have internal BLT(s), enable internal event to signal/wait.
            const IGpuEvent* pEvent = pCmdBuf->GetInternalEvent();
            pCmdBuf->CmdResetEvent(*pEvent, HwPipePostIndexFetch);

            // Release from BLTs.
            IssueReleaseSync(pCmdBuf,
                             pCmdStream,
                             postBltStageMask,
                             postBltAccessMask,
                             waRefreshLlc,
                             pEvent,
                             pBarrierOps);

            ppActiveEvents   = &pEvent;
            activeEventCount = 1;
        }

        // Issue acquire for client requested global cache sync.
        IssueAcquireSync(pCmdBuf,
                         pCmdStream,
                         barrierAcquireInfo.dstStageMask,
                         barrierAcquireInfo.dstGlobalAccessMask,
                         false,
                         FullSyncBaseAddr,
                         FullSyncSize,
                         activeEventCount,
                         ppActiveEvents,
                         pBarrierOps);

        // Loop through memory transitions to issue client-requested acquires for ranged memory syncs.
        for (uint32 i = 0; i < barrierAcquireInfo.memoryBarrierCount; i++)
        {
            const MemBarrier& barrier              = barrierAcquireInfo.pMemoryBarriers[i];
            const GpuMemSubAllocInfo& memAllocInfo = barrier.memory;

            const uint32  acquireAccessMask  = barrier.dstAccessMask;
            const gpusize rangedSyncBaseAddr = memAllocInfo.pGpuMemory->Desc().gpuVirtAddr + memAllocInfo.offset;
            const gpusize rangedSyncSize     = memAllocInfo.size;

            IssueAcquireSync(pCmdBuf,
                             pCmdStream,
                             barrierAcquireInfo.dstStageMask,
                             acquireAccessMask,
                             false,
                             rangedSyncBaseAddr,
                             rangedSyncSize,
                             0,
                             nullptr,
                             pBarrierOps);
        }

        // Loop through memory transitions to issue client-requested acquires for image syncs.
        for (uint32 i = 0; i < barrierAcquireInfo.imageBarrierCount; i++)
        {
            const ImgBarrier& imgBarrier = barrierAcquireInfo.pImageBarriers[i];
            const Pal::Image& image      = static_cast<const Pal::Image&>(*imgBarrier.pImage);

            IssueAcquireSync(pCmdBuf,
                             pCmdStream,
                             barrierAcquireInfo.dstStageMask,
                             imgBarrier.dstAccessMask,
                             transitionList[i].waNeedRefreshLlc,
                             image.GetGpuVirtualAddr(),
                             image.GetGpuMemSize(),
                             0,
                             nullptr,
                             pBarrierOps);
        }
    }
    else
    {
        pCmdBuf->NotifyAllocFailure();
    }
}

// =====================================================================================================================
// BarrierReleaseThenAcquire is effectively the same as calling BarrierRelease immediately by calling BarrierAcquire.
// This is a convenience method for clients implementing single point barriers, and is functionally equivalent to the
// current CmdBarrier() interface.
void Device::BarrierReleaseThenAcquire(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    const AcquireReleaseInfo&     barrierInfo,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    // Internal event per command buffer is used for ReleaseThenAcquire case. All release/acquire-based barriers in the
    // same command buffer use the same event.
    const IGpuEvent* pEvent = pCmdBuf->GetInternalEvent();

    Result result = Result::Success;

    AutoBuffer<MemBarrier, 8, Platform> memBarriers(barrierInfo.memoryBarrierCount, GetPlatform());
    if (barrierInfo.memoryBarrierCount > 0)
    {
        if (memBarriers.Capacity() < barrierInfo.memoryBarrierCount)
        {
            pCmdBuf->NotifyAllocFailure();
        }
        else
        {
            for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
            {
                memBarriers[i].memory        = barrierInfo.pMemoryBarriers[i].memory;
                memBarriers[i].srcAccessMask = barrierInfo.pMemoryBarriers[i].srcAccessMask;
                memBarriers[i].dstAccessMask = 0;
            }
        }
    }

    AutoBuffer<ImgBarrier, 8, Platform> imgBarriers(barrierInfo.imageBarrierCount, GetPlatform());
    if ((result==Result::Success) && (barrierInfo.imageBarrierCount > 0))
    {
        if (imgBarriers.Capacity() < barrierInfo.imageBarrierCount)
        {
            pCmdBuf->NotifyAllocFailure();
        }
        else
        {
            for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
            {
                imgBarriers[i].pImage             = barrierInfo.pImageBarriers[i].pImage;
                imgBarriers[i].subresRange        = barrierInfo.pImageBarriers[i].subresRange;
                imgBarriers[i].box                = barrierInfo.pImageBarriers[i].box;
                imgBarriers[i].srcAccessMask      = barrierInfo.pImageBarriers[i].srcAccessMask;
                imgBarriers[i].dstAccessMask      = 0;
                imgBarriers[i].oldLayout          = barrierInfo.pImageBarriers[i].oldLayout;
                imgBarriers[i].newLayout          = barrierInfo.pImageBarriers[i].newLayout; // Do decompress in release.
                imgBarriers[i].pQuadSamplePattern = barrierInfo.pImageBarriers[i].pQuadSamplePattern;
            }
        }
    }

    // Build BarrierRelease function.
    AcquireReleaseInfo releaseInfo;
    releaseInfo.srcStageMask        = barrierInfo.srcStageMask;
    releaseInfo.srcGlobalAccessMask = barrierInfo.srcGlobalAccessMask;
    releaseInfo.dstStageMask        = 0;
    releaseInfo.dstGlobalAccessMask = 0;
    releaseInfo.memoryBarrierCount  = barrierInfo.memoryBarrierCount;
    releaseInfo.pMemoryBarriers     = &memBarriers[0];
    releaseInfo.imageBarrierCount   = barrierInfo.imageBarrierCount;
    releaseInfo.pImageBarriers      = &imgBarriers[0];

    BarrierRelease(pCmdBuf, pCmdStream, releaseInfo, pEvent, pBarrierOps);

    // Build BarrierAcquire function.
    AcquireReleaseInfo acquireInfo;
    acquireInfo.srcStageMask        = 0;
    acquireInfo.srcGlobalAccessMask = 0;
    acquireInfo.dstStageMask        = barrierInfo.dstStageMask;
    acquireInfo.dstGlobalAccessMask = barrierInfo.dstGlobalAccessMask;

    acquireInfo.memoryBarrierCount  = barrierInfo.memoryBarrierCount;
    for (uint32 i = 0; i < barrierInfo.memoryBarrierCount; i++)
    {
        memBarriers[i].srcAccessMask = 0;
        memBarriers[i].dstAccessMask = barrierInfo.pMemoryBarriers[i].dstAccessMask;
    }
    acquireInfo.pMemoryBarriers = &memBarriers[0];

    acquireInfo.imageBarrierCount = barrierInfo.imageBarrierCount;
    for (uint32 i = 0; i < barrierInfo.imageBarrierCount; i++)
    {
        imgBarriers[i].srcAccessMask = 0;
        imgBarriers[i].dstAccessMask = barrierInfo.pImageBarriers[i].dstAccessMask;
        imgBarriers[i].oldLayout     = barrierInfo.pImageBarriers[i].newLayout;
        imgBarriers[i].newLayout     = barrierInfo.pImageBarriers[i].newLayout;
    }
    acquireInfo.pImageBarriers = &imgBarriers[0];

    BarrierAcquire(pCmdBuf, pCmdStream, acquireInfo, 1, &pEvent, pBarrierOps);
}

// =====================================================================================================================
// Helper function that issues requested transition for the image barrier.
void Device::IssueBlt(
    GfxCmdBuffer*                 pCmdBuf,
    CmdStream*                    pCmdStream,
    const ImgBarrier*             pImgBarrier,
    HwLayoutTransition            transition,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(pImgBarrier != nullptr);
    PAL_ASSERT(transition  != HwLayoutTransition::None);
    PAL_ASSERT(pBarrierOps != nullptr);

    // Tell RGP about this transition
    BarrierTransition rgpTransition = AcqRelBuildTransition(pImgBarrier, transition, false, pBarrierOps);
    DescribeBarrier(pCmdBuf, &rgpTransition, pBarrierOps);

    // And clear it so it can differentiate sync and async flushes
    *pBarrierOps = {};

    const auto& image = static_cast<const Pal::Image&>(*pImgBarrier->pImage);

    if (transition == HwLayoutTransition::InitMaskRam)
    {
        // Transition out of LayoutUninitializedTarget needs to initialize metadata memories.
        AcqRelInitMaskRam(pCmdBuf, pCmdStream, *pImgBarrier, transition);
    }
    else
    {
        // Image does normal BLT.
        if (image.IsDepthStencil())
        {
            AcqRelDepthStencilTransition(pCmdBuf, *pImgBarrier, transition);
        }
        else
        {
            AcqRelColorTransition(pCmdBuf, pCmdStream, *pImgBarrier, transition, pBarrierOps);
        }
    }
}

// =====================================================================================================================
// Build the necessary packets to fulfill the requested cache sync for release.
size_t Device::BuildReleaseSyncPackets(
    EngineType                    engineType,
    uint32                        stageMask,
    uint32                        accessMask,
    bool                          flushLlc,
    gpusize                       gpuEventStartVa,
    void*                         pBuffer,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(pBarrierOps != nullptr);

    // Issue RELEASE_MEM packets to flush caches (optional) and signal gpuEvent.
    const uint32   numEventSlots = Parent()->ChipProperties().gfxip.numSlotsPerEvent;
    VGT_EVENT_TYPE vgtEvents[MaxSlotsPerEvent]; // Always create the max size.
    uint32         vgtEventCount = 0;

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 500
    // If it reaches here, we know the Release-Acquire barrier is enabled, so each event should have MaxSlotsPerEvent
    // number of slots.
    PAL_ASSERT(numEventSlots == MaxSlotsPerEvent);
#endif

    // If any of the access mask bits that could result in RB sync are set, use CACHE_FLUSH_AND_INV_TS.
    // There is no way to INV the CB metadata caches during acquire. So at release always also invalidate if we are to
    // flush CB metadata.
    if (TestAnyFlagSet(accessMask, CoherCopy | CoherResolve | CoherClear | CoherColorTarget | CoherDepthStencilTarget))
    {
        // Issue a pipelined EOP event that writes timestamp to a GpuEvent slot when all prior GPU work completes.
        vgtEvents[vgtEventCount++] = CACHE_FLUSH_AND_INV_TS_EVENT;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 504
        pBarrierOps->pipelineStalls.eopTsBottomOfPipe       = 1;
#endif

        pBarrierOps->caches.flushCb                         = 1;
        pBarrierOps->caches.invalCb                         = 1;
        pBarrierOps->caches.flushCbMetadata                 = 1;
        pBarrierOps->caches.invalCbMetadata                 = 1;

        pBarrierOps->caches.flushDb                         = 1;
        pBarrierOps->caches.invalDb                         = 1;
        pBarrierOps->caches.flushDbMetadata                 = 1;
        pBarrierOps->caches.invalDbMetadata                 = 1;
    }
    // Unfortunately, there is no VS_DONE event with which to implement PipelineStageVs/Hs/Ds/Gs, so it has to
    // conservatively use BottomOfPipe.
    else if (TestAnyFlagSet(stageMask, PipelineStageVs            |
                                       PipelineStageHs            |
                                       PipelineStageDs            |
                                       PipelineStageGs            |
                                       PipelineStageEarlyDsTarget |
                                       PipelineStageLateDsTarget  |
                                       PipelineStageColorTarget   |
                                       PipelineStageBottomOfPipe))
    {
        // Implement set with an EOP event written when all prior GPU work completes.
        vgtEvents[vgtEventCount++] = BOTTOM_OF_PIPE_TS;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 504
        pBarrierOps->pipelineStalls.eopTsBottomOfPipe       = 1;
#endif
    }
    else if (TestAnyFlagSet(stageMask, PipelineStagePs | PipelineStageCs))
    {
        // If the signal/wait event has multiple slots, we can utilize it to issue separate EOS event for PS and CS
        // waves. Otherwise just fall back to a single BOP pipeline stage.
        if (numEventSlots > 1)
        {
            if (TestAnyFlagSet(stageMask, PipelineStagePs))
            {
                // Implement set with an EOS event waiting for PS waves to complete.
                vgtEvents[vgtEventCount++] = PS_DONE;
                pBarrierOps->pipelineStalls.eosTsPsDone = 1;
            }

            if (TestAnyFlagSet(stageMask, PipelineStageCs))
            {
                // Implement set/reset with an EOS event waiting for CS waves to complete.
                vgtEvents[vgtEventCount++] = CS_DONE;
                pBarrierOps->pipelineStalls.eosTsCsDone = 1;
            }
        }
        else
        {
            vgtEvents[vgtEventCount++] = BOTTOM_OF_PIPE_TS;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 504
            pBarrierOps->pipelineStalls.eopTsBottomOfPipe       = 1;
#endif
        }
    }

    // Create info for RELEASE_MEM. Initialize common part at here.
    ExplicitReleaseMemInfo releaseMemInfo = {};
    releaseMemInfo.engineType = engineType;

    bool requestCacheSync = false;

    if (m_gfxIpLevel == GfxIpLevel::GfxIp9)
    {
        releaseMemInfo.coherCntl = Gfx9BuildReleaseCoherCntl(accessMask,
                                                             flushLlc,
                                                             vgtEventCount,
                                                             &vgtEvents[0],
                                                             pBarrierOps);

        requestCacheSync = (releaseMemInfo.coherCntl != 0);
    }
    else if (IsGfx10(m_gfxIpLevel))
    {
        releaseMemInfo.gcrCntl = Gfx10BuildReleaseGcrCntl(accessMask, flushLlc, pBarrierOps);

        requestCacheSync = (releaseMemInfo.gcrCntl != 0);
    }

    // If we have cache sync request yet don't issue any VGT event, we need to issue a dummy one.
    if (requestCacheSync && (vgtEventCount == 0))
    {
        // Flush at earliest supported pipe point for RELEASE_MEM (CS_DONE always works).
        vgtEvents[vgtEventCount++] = CS_DONE;
        pBarrierOps->pipelineStalls.eosTsCsDone = 1;
    }

    PAL_ASSERT(vgtEventCount <= numEventSlots);

    // Build the release packets.
    size_t dwordsWritten = 0;

    for (uint32 i = 0; i < vgtEventCount; i++)
    {
        // Issue release with requested eop/eos event on ME engine.
        releaseMemInfo.vgtEvent = vgtEvents[i];
        releaseMemInfo.dstAddr  = gpuEventStartVa + (i * sizeof(uint32));
        releaseMemInfo.dataSel  = data_sel__me_release_mem__send_32_bit_low;
        releaseMemInfo.data     = GpuEvent::SetValue;

        dwordsWritten += m_cmdUtil.ExplicitBuildReleaseMem(releaseMemInfo,
                                                           VoidPtrInc(pBuffer, sizeof(uint32) * dwordsWritten),
                                                           0,
                                                           0);
    }

    // Set remaining (unused) event slots as early as possible. Implement set/reset event with a WRITE_DATA command
    // using the CP.
    WriteDataInfo writeData = {};
    writeData.engineType = engineType;
    writeData.engineSel  = engine_sel__me_write_data__micro_engine;
    writeData.dstSel     = dst_sel__me_write_data__memory;

    for (uint32 slotIdx = vgtEventCount; slotIdx < numEventSlots; slotIdx++)
    {
        writeData.dstAddr = gpuEventStartVa + (sizeof(uint32) * slotIdx);

        dwordsWritten += m_cmdUtil.BuildWriteData(writeData,
                                                  GpuEvent::SetValue,
                                                  VoidPtrInc(pBuffer, sizeof(uint32) * dwordsWritten));
    }

    return dwordsWritten;
}

// =====================================================================================================================
// Translate accessMask to CP_COHER_CNTL. (CacheCoherencyUsageFlags -> CacheSyncFlags -> TcCacheOps -> CpCoherCntl)
// This function is GFX9-ONLY.
uint32 Device::Gfx9BuildReleaseCoherCntl(
    uint32                        accessMask,
    bool                          flushTcc,
    uint32                        vgtEventCount,
    const VGT_EVENT_TYPE*         pVgtEvents,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    uint32 cacheSyncFlags = Gfx9ConvertToReleaseSyncFlags(accessMask, flushTcc, pBarrierOps);

    // Loop check vgtEvent[].
    for (uint32 i = 0; i < vgtEventCount; i++)
    {
        if (pVgtEvents[i] == CACHE_FLUSH_AND_INV_TS_EVENT)
        {
            cacheSyncFlags &= ~CacheSyncFlushAndInvRb;
            // We don't need to reset these in barrier ops because they still happen, just
            // not during the TcCacheOp
            break;
        }
    }

    const uint32 tcCacheOp = static_cast<uint32>(SelectTcCacheOp(&cacheSyncFlags));

    // The release doesn't need to iterate until cacheSyncFlags is cleared.
    PAL_ASSERT(cacheSyncFlags == 0);

    return Gfx9TcCacheOpConversionTable[tcCacheOp];
}

// =====================================================================================================================
// Translate accessMask to syncReqs.cacheFlags. (CacheCoherencyUsageFlags -> GcrCntl)
uint32 Device::Gfx10BuildReleaseGcrCntl(
    uint32                        accessMask,
    bool                          flushGl2,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(pBarrierOps != nullptr);
    Gfx10ReleaseMemGcrCntl gcrCntl = {};

    if (TestAnyFlagSet(accessMask, CoherCpu | CoherMemory))
    {
        // At release we want to invalidate L2 so any future read to L2 would go down to memory, at acquire we want to
        // flush L2 so that main memory gets the latest data.
        gcrCntl.bits.gl2Inv = 1;
        pBarrierOps->caches.invalTcc = 1;
    }

    // Setup GL2Range and Sequence only if cache flush/inv is requested.
    if (gcrCntl.u32All != 0)
    {
        // GL2_RANGE[1:0]
        //  0:ALL          wb/inv op applies to entire physical cache (ignore range)
        //  1:VOL          wb/inv op applies to all volatile tagged lines in the GL2 (ignore range)
        //  2:RANGE      - wb/inv ops applies to just the base/limit virtual address range
        //  3:FIRST_LAST - wb/inv ops applies to 128B at BASE_VA and 128B at LIMIT_VA
        gcrCntl.bits.gl2Range = 0; // ReleaseMem doesn't support RANGE.

        // SEQ[1:0]   controls the sequence of operations on the cache hierarchy (L0/L1/L2)
        //      0: PARALLEL   initiate wb/inv ops on specified caches at same time
        //      1: FORWARD    L0 then L1/L2, complete L0 ops then initiate L1/L2
        //                    Typically only needed when doing WB of L0 K$, M$, or RB w/ WB of GL2
        //      2: REVERSE    L2 -> L1 -> L0
        //                    Typically only used for post-unaligned-DMA operation (invalidate only)
        // Because GCR can issue any cache flush, we need to ensure the flush sequence unconditionally.
        gcrCntl.bits.seq = 1;
    }

    if (flushGl2)
    {
        gcrCntl.bits.gl2Wb = 1;
        pBarrierOps->caches.flushTcc = 1;
    }

    return gcrCntl.u32All;
}

// =====================================================================================================================
// Build the necessary packets to fulfill the requested cache sync for acquire.
size_t Device::BuildAcquireSyncPackets(
    EngineType                    engineType,
    uint32                        stageMask,
    uint32                        accessMask,
    bool                          invalidateLlc,
    gpusize                       baseAddress,
    gpusize                       sizeBytes,
    void*                         pBuffer,      // [out] Build the PM4 packet in this buffer.
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    size_t dwordsWritten = 0;

    // Create info for ACQUIRE_MEM. Initialize common part at here.
    ExplicitAcquireMemInfo acquireMemInfo = {};
    acquireMemInfo.engineType   = engineType;
    acquireMemInfo.baseAddress  = baseAddress;
    acquireMemInfo.sizeBytes    = sizeBytes;
    acquireMemInfo.flags.usePfp = TestAnyFlagSet(stageMask, PipelineStageTopOfPipe         |
                                                            PipelineStageFetchIndirectArgs |
                                                            PipelineStageFetchIndices);

    if (m_gfxIpLevel == GfxIpLevel::GfxIp9)
    {
        uint32 cacheSyncFlags = Gfx9ConvertToAcquireSyncFlags(accessMask, engineType, invalidateLlc, pBarrierOps);

        // If there's no graphics support on this engine we shouldn't see gfx-specific requests.
        PAL_ASSERT(TestAnyFlagSet(cacheSyncFlags, CacheSyncFlushAndInvRb) ==
                   Pal::Device::EngineSupportsGraphics(engineType));

        while (cacheSyncFlags != 0)
        {
            const uint32 tcCacheOp = static_cast<uint32>(SelectTcCacheOp(&cacheSyncFlags));

            regCP_COHER_CNTL cpCoherCntl = {};
            cpCoherCntl.u32All                       = Gfx9TcCacheOpConversionTable[tcCacheOp];
            cpCoherCntl.bits.CB_ACTION_ENA           = TestAnyFlagSet(cacheSyncFlags, CacheSyncFlushAndInvCbData);
            cpCoherCntl.bits.DB_ACTION_ENA           = TestAnyFlagSet(cacheSyncFlags, CacheSyncFlushAndInvDb);
            cpCoherCntl.bits.SH_KCACHE_ACTION_ENA    = TestAnyFlagSet(cacheSyncFlags, CacheSyncInvSqK$);
            cpCoherCntl.bits.SH_ICACHE_ACTION_ENA    = TestAnyFlagSet(cacheSyncFlags, CacheSyncInvSqI$);
            cpCoherCntl.bits.SH_KCACHE_WB_ACTION_ENA = TestAnyFlagSet(cacheSyncFlags, CacheSyncFlushSqK$);

            cacheSyncFlags &= ~(CacheSyncFlushAndInvCbData |
                                CacheSyncFlushAndInvDb     |
                                CacheSyncInvSqK$           |
                                CacheSyncInvSqI$           |
                                CacheSyncFlushSqK$);

            acquireMemInfo.coherCntl = cpCoherCntl.u32All;

            // Build ACQUIRE_MEM packet.
            dwordsWritten += m_cmdUtil.ExplicitBuildAcquireMem(acquireMemInfo,
                                                               VoidPtrInc(pBuffer, sizeof(uint32) * dwordsWritten));
        }
    }
    else if (IsGfx10(m_gfxIpLevel))
    {
        if (Pal::Device::EngineSupportsGraphics(acquireMemInfo.engineType))
        {
            acquireMemInfo.coherCntl = Gfx10BuildAcquireCoherCntl(engineType, accessMask, pBarrierOps);
        }

        // The only difference between the GFX9 and GFX10 versions of this packet are that GFX10
        // added a new "gcr_cntl" field.
        acquireMemInfo.gcrCntl = Gfx10BuildAcquireGcrCntl(accessMask,
                                                          invalidateLlc,
                                                          baseAddress,
                                                          sizeBytes,
                                                          (acquireMemInfo.coherCntl != 0),
                                                          pBarrierOps);

        // Build ACQUIRE_MEM packet.
        dwordsWritten += m_cmdUtil.ExplicitBuildAcquireMem(acquireMemInfo,
                                                           VoidPtrInc(pBuffer, sizeof(uint32) * dwordsWritten));
    }

    return dwordsWritten;
}

// =====================================================================================================================
// Translate accessMask to GcrCntl.
uint32 Device::Gfx10BuildAcquireGcrCntl(
    uint32                        accessMask,
    bool                          invalidateGl2,
    gpusize                       baseAddress,
    gpusize                       sizeBytes,
    bool                          isFlushing,
    Developer::BarrierOperations* pBarrierOps
    ) const
{
    PAL_ASSERT(pBarrierOps != nullptr);
    // K$ and I$ and all previous tcCacheOp controlled caches are moved to GCR fields, set in CalcAcquireMemGcrCntl().

    // Cache operations supported by ACQUIRE_MEM's gcr_cntl.
    Gfx10AcquireMemGcrCntl gcrCntl = {};

    // The L1 / L2 caches are physical address based. When specify the range, the GCR will perform virtual address
    // to physical address translation before the wb / inv. If the acquired op is full sync, we must ignore the range,
    // otherwise page fault may occur because page table cannot cover full range virtual address.
    //    When the source address is virtual , the GCR block will have to perform the virtual address to physical
    //    address translation before the wb / inv. Since the pages in memory are a collection of fragments, you can't
    //    specify the full range without walking into a page that has no PTE triggering a fault. In the cases where
    //    the driver wants to wb / inv the entire cache, you should not use range based method, and instead flush the
    //    entire cache without it. The range based method is not meant to be used this way, it is for selective page
    //    invalidation.
    //
    // GL1_RANGE[1:0] - range control for L0 / L1 physical caches(K$, V$, M$, GL1)
    //  0:ALL         - wb / inv op applies to entire physical cache (ignore range)
    //  1:reserved
    //  2:RANGE       - wb / inv op applies to just the base / limit virtual address range
    //  3:FIRST_LAST  - wb / inv op applies to 128B at BASE_VA and 128B at LIMIT_VA
    //
    // GL2_RANGE[1:0]
    //  0:ALL         - wb / inv op applies to entire physical cache (ignore range)
    //  1:VOL         - wb / inv op applies to all volatile tagged lines in the GL2 (ignore range)
    //  2:RANGE       - wb / inv op applies to just the base/limit virtual address range
    //  3:FIRST_LAST  - wb / inv op applies to 128B at BASE_VA and 128B at LIMIT_VA
    if (((baseAddress == FullSyncBaseAddr) && (sizeBytes == FullSyncSize)) ||
        (sizeBytes > CmdUtil::Gfx10AcquireMemGl1Gl2RangedCheckMaxSurfaceSizeBytes))
    {
        gcrCntl.bits.gl1Range = 0;
        gcrCntl.bits.gl2Range = 0;
    }
    else
    {
        {
            gcrCntl.bits.gl1Range = 2;
            gcrCntl.bits.gl2Range = 2;
        }
    }

    const bool isShaderCacheDirty = TestAnyFlagSet(accessMask, CoherShader  |
                                                               CoherCopy    |
                                                               CoherResolve |
                                                               CoherClear   |
                                                               CoherStreamOut);

    // GLM_WB[0]  - write-back control for the meta-data cache of GL2. L2MD is write-through, ignore this bit.
    // GLM_INV[0] - invalidate enable for the meta-data cache of GL2
    // GLK_WB[0]  - write-back control for shaded scalar L0 cache
    // GLK_INV[0] - invalidate enable for shader scalar L0 cache
    // GLV_INV[0] - invalidate enable for shader vector L0 cache
    // GL1_INV[0] - invalidate enable for GL1
    // GL2_INV[0] - invalidate enable for GL2
    // GL2_WB[0]  - writeback enable for GL2
    gcrCntl.bits.glmWb  = 0;
    gcrCntl.bits.glmInv = isShaderCacheDirty;
    gcrCntl.bits.glkWb  = 0;
    gcrCntl.bits.glkInv = isShaderCacheDirty;
    gcrCntl.bits.glvInv = isShaderCacheDirty;
    gcrCntl.bits.gl1Inv = isShaderCacheDirty;

    // Leave gcrCntl.bits.gl2Us unset.
    // Leave gcrCntl.bits.gl2Discard unset.
    gcrCntl.bits.gl2Inv = invalidateGl2;
    gcrCntl.bits.gl2Wb  = TestAnyFlagSet(accessMask, CoherCpu | CoherMemory);

    pBarrierOps->caches.invalSqK$        |= isShaderCacheDirty;
    pBarrierOps->caches.invalTcp         |= isShaderCacheDirty;
    pBarrierOps->caches.invalTccMetadata |= isShaderCacheDirty;
    pBarrierOps->caches.invalGl1         |= isShaderCacheDirty;
    pBarrierOps->caches.invalTcc         |= invalidateGl2;
    pBarrierOps->caches.flushTcc         |= TestAnyFlagSet(accessMask, CoherCpu | CoherMemory);

    // SEQ[1:0]   controls the sequence of operations on the cache hierarchy (L0/L1/L2)
    //      0: PARALLEL   initiate wb/inv ops on specified caches at same time
    //      1: FORWARD    L0 then L1/L2, complete L0 ops then initiate L1/L2
    //                    Typically only needed when doing WB of L0 K$, M$, or RB w/ WB of GL2
    //      2: REVERSE    L2 -> L1 -> L0
    //                    Typically only used for post-unaligned-DMA operation (invalidate only)
    // If we're issuing an RB cache flush while writing back GL2, we need to ensure the bottom-up flush sequence.
    //  Note: If we ever start flushing K$ or M$, isFlushing should be updated
    PAL_ASSERT((gcrCntl.bits.glmWb == 0) && (gcrCntl.bits.glkWb == 0));
    gcrCntl.bits.seq = (isFlushing && gcrCntl.bits.gl2Wb) ? 1 : 0;

    return gcrCntl.u32All;
}

} // Gfx9
} // Pal
