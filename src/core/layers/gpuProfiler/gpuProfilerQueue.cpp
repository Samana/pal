/*
 *******************************************************************************
 *
 * Copyright (c) 2015-2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "core/layers/gpuProfiler/gpuProfilerCmdBuffer.h"
#include "core/layers/gpuProfiler/gpuProfilerDevice.h"
#include "core/layers/gpuProfiler/gpuProfilerPlatform.h"
#include "core/layers/gpuProfiler/gpuProfilerQueue.h"
#include "palAutoBuffer.h"
#include "palDequeImpl.h"
#include "palSysUtil.h"

using namespace Util;

namespace Pal
{
namespace GpuProfiler
{

// =====================================================================================================================
Queue::Queue(
    IQueue*    pNextQueue,
    Device*    pDevice,
    QueueType  queueType,
    EngineType engineType,
    uint32     engineId,
    uint32     queueId)
    :
    QueueDecorator(pNextQueue, pDevice),
    m_pDevice(pDevice),
    m_queueType(queueType),
    m_engineType(engineType),
    m_engineIndex(engineId),
    m_queueId(queueId),
    m_shaderEngineCount(0),
    m_pCmdAllocator(nullptr),
    m_replayAllocator(64 * 1024),
    m_pGlobalPerfCounterValues(nullptr),
    m_availableCmdBufs(static_cast<Platform*>(pDevice->GetPlatform())),
    m_busyCmdBufs(static_cast<Platform*>(pDevice->GetPlatform())),
    m_availableNestedCmdBufs(static_cast<Platform*>(pDevice->GetPlatform())),
    m_busyNestedCmdBufs(static_cast<Platform*>(pDevice->GetPlatform())),
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    m_availableGartGpuMem(static_cast<Platform*>(pDevice->GetPlatform())),
    m_busyGartGpuMem(static_cast<Platform*>(pDevice->GetPlatform())),
    m_availablePipeStatsQueries(static_cast<Platform*>(pDevice->GetPlatform())),
    m_busyPipeStatsQueries(static_cast<Platform*>(pDevice->GetPlatform())),
#endif
    m_availableGpaSessions(static_cast<Platform*>(pDevice->GetPlatform())),
    m_busyGpaSessions(static_cast<Platform*>(pDevice->GetPlatform())),
    m_numReportedPerfCounters(0),
    m_availableFences(static_cast<Platform*>(pDevice->GetPlatform())),
    m_pendingSubmits(static_cast<Platform*>(pDevice->GetPlatform())),
    m_profilingModeEnabled(false),
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    m_curGartGpuMemOffset(0),
#endif
    m_logItems(static_cast<Platform*>(pDevice->GetPlatform())),
    m_curLogFrame(0),
    m_curLogCmdBufIdx(0),
    m_curLogSqttIdx(0)
{
    memset(&m_nestedAllocatorCreateInfo, 0, sizeof(m_nestedAllocatorCreateInfo));
    memset(&m_gpaSessionSampleConfig,    0, sizeof(m_gpaSessionSampleConfig));
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    memset(&m_curGartGpuMem,             0, sizeof(m_curGartGpuMem));
#endif
    memset(&m_nextSubmitInfo,            0, sizeof(m_nextSubmitInfo));
    memset(&m_perFrameLogItem,           0, sizeof(m_perFrameLogItem));

    // All nested allocations are set the the minimum size (4KB) because applications that submit hundreds of nested
    // command buffers can potentially exhaust the GPU VA range by simply playing back too many nested command buffers.
    // This will have a small performance impact on large nested command buffers but we have little choice for now.
    m_nestedAllocatorCreateInfo.allocInfo[CommandDataAlloc].allocHeap     = GpuHeapGartUswc;
    m_nestedAllocatorCreateInfo.allocInfo[CommandDataAlloc].allocSize     = 4 * 1024;
    m_nestedAllocatorCreateInfo.allocInfo[CommandDataAlloc].suballocSize  = 4 * 1024;
    m_nestedAllocatorCreateInfo.allocInfo[EmbeddedDataAlloc].allocHeap    = GpuHeapGartUswc;
    m_nestedAllocatorCreateInfo.allocInfo[EmbeddedDataAlloc].allocSize    = 4 * 1024;
    m_nestedAllocatorCreateInfo.allocInfo[EmbeddedDataAlloc].suballocSize = 4 * 1024;
}

// =====================================================================================================================
Queue::~Queue()
{
    // Ensure all log items are flushed out before we shut down.
    WaitIdle();
    ProcessIdleSubmits();
    m_logFile.Close();

    PAL_ASSERT(m_busyCmdBufs.NumElements() == 0);
    PAL_ASSERT(m_busyNestedCmdBufs.NumElements() == 0);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    PAL_ASSERT(m_busyGartGpuMem.NumElements() == 0);
#endif
    PAL_ASSERT(m_pendingSubmits.NumElements() == 0);
    PAL_ASSERT(m_busyGpaSessions.NumElements() == 0);

    while (m_availableCmdBufs.NumElements() > 0)
    {
        TargetCmdBuffer* pCmdBuf = nullptr;
        m_availableCmdBufs.PopFront(&pCmdBuf);

        pCmdBuf->Destroy();
        PAL_SAFE_FREE(pCmdBuf, m_pDevice->GetPlatform());
    }

    while (m_availableNestedCmdBufs.NumElements() > 0)
    {
        NestedInfo info = {};
        m_availableNestedCmdBufs.PopFront(&info);

        info.pCmdBuffer->Destroy();
        PAL_SAFE_FREE(info.pCmdBuffer, m_pDevice->GetPlatform());

        info.pCmdAllocator->Destroy();
        PAL_SAFE_FREE(info.pCmdAllocator, m_pDevice->GetPlatform());
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    while (m_availableGartGpuMem.NumElements() > 0)
    {
        GpuMemoryInfo info = {};
        m_availableGartGpuMem.PopFront(&info);

        PAL_ASSERT(info.pGpuMemory != nullptr);

        info.pGpuMemory->Unmap();
        info.pGpuMemory->Destroy();
        PAL_SAFE_FREE(info.pGpuMemory, m_pDevice->GetPlatform());
    }

    while (m_availablePipeStatsQueries.NumElements() > 0)
    {
        IQueryPool* pQuery = nullptr;
        m_availablePipeStatsQueries.PopFront(&pQuery);

        pQuery->Destroy();
        PAL_SAFE_FREE(pQuery, m_pDevice->GetPlatform());
    }
#endif

    while (m_availableGpaSessions.NumElements() > 0)
    {
        GpuUtil::GpaSession* pGpaSession = nullptr;
        m_availableGpaSessions.PopFront(&pGpaSession);

        PAL_SAFE_DELETE(pGpaSession, m_pDevice->GetPlatform());
    }

    while (m_availableFences.NumElements() > 0)
    {
        IFence* pFence = nullptr;
        m_availableFences.PopFront(&pFence);

        pFence->Destroy();
        PAL_SAFE_FREE(pFence, m_pDevice->GetPlatform());
    }

    if (m_pCmdAllocator != nullptr)
    {
        m_pCmdAllocator->Destroy();
        PAL_SAFE_FREE(m_pCmdAllocator, m_pDevice->GetPlatform());
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    if (m_curGartGpuMem.pGpuMemory != nullptr)
    {
        m_curGartGpuMem.pGpuMemory->Unmap();
        m_curGartGpuMem.pGpuMemory->Destroy();
        PAL_SAFE_FREE(m_curGartGpuMem.pGpuMemory, m_pDevice->GetPlatform());
    }
#endif
    PAL_SAFE_FREE(m_pGlobalPerfCounterValues, m_pDevice->GetPlatform());

    DestroyGpaSessionSampleConfig();
}

// =====================================================================================================================
Result Queue::Init()
{
    Result result = m_replayAllocator.Init();

    if (result == Result::Success)
    {
        CmdAllocatorCreateInfo createInfo = { };
        createInfo.flags.autoMemoryReuse                     = 1;
        createInfo.allocInfo[CommandDataAlloc].allocHeap     = GpuHeapGartUswc;
        createInfo.allocInfo[CommandDataAlloc].allocSize     = 2 * 1024 * 1024;
        createInfo.allocInfo[CommandDataAlloc].suballocSize  = 64 * 1024;
        createInfo.allocInfo[EmbeddedDataAlloc].allocHeap    = GpuHeapGartUswc;
        createInfo.allocInfo[EmbeddedDataAlloc].allocSize    = 2 * 1024 * 1024;
        createInfo.allocInfo[EmbeddedDataAlloc].suballocSize = 64 * 1024;

        void* pMemory = PAL_MALLOC(m_pDevice->GetCmdAllocatorSize(createInfo, &result),
                                   m_pDevice->GetPlatform(),
                                   AllocInternal);

        if (result == Result::Success)
        {
            if (pMemory == nullptr)
            {
                result = Result::ErrorOutOfMemory;
            }
            else
            {
                result = m_pDevice->CreateCmdAllocator(createInfo, pMemory, &m_pCmdAllocator);

                if (result != Result::Success)
                {
                    PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
                }
            }
        }
    }

    // Build GpaSession config info based on profiling objectives.
    if (result == Result::Success)
    {
        result = BuildGpaSessionSampleConfig();
    }

    // Note that global perf counters are disabled if this value is zero.
    const uint32 numGlobalPerfCounters = m_pDevice->NumGlobalPerfCounters();

    if (numGlobalPerfCounters > 0)
    {
        if (result == Result::Success)
        {
            m_numReportedPerfCounters = (m_pDevice->ProfilerSettings().gpuProfilerGlobalPerfCounterPerInstance) ?
                m_gpaSessionSampleConfig.perfCounters.numCounters : numGlobalPerfCounters;

            // Allocate enough space for one 64-bit counter per global perf counter.
            const size_t size = sizeof(uint64) * m_numReportedPerfCounters;

            m_pGlobalPerfCounterValues =
                static_cast<uint64*>(PAL_MALLOC(size, m_pDevice->GetPlatform(), AllocInternal));

            if (m_pGlobalPerfCounterValues == nullptr)
            {
                result = Result::ErrorOutOfMemory;
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Submits the specified command buffers to the next layer.  This same implementation is used for both command buffers
// submitted by the application and any internal command buffers this layer needs to submit.
Result Queue::InternalSubmit(
    const SubmitInfo& submitInfo,
    bool              releaseObjects)  // If true, all currently acquired objects can be associated with this submit
                                       // and reclaimed once this submit completes.  Otherwise, continue building
                                       // m_nextSubmitInfo pinning more acquired resources to the next tracked submit.
{
    Result result = m_pNextLayer->Submit(submitInfo);

    if ((result == Result::Success) && releaseObjects)
    {
        // Get an available queue-owned fence.
        m_nextSubmitInfo.pFence = AcquireFence();

        // This call will make it so that the profiler fence will show as signaled once all previous work
        // submitted on this queue has submitted, but WaitForFences() will not work.  This is acceptable for
        // this use case, and lets us avoid interfering with app-specified fences in the real submit above.
        AssociateFenceWithLastSubmit(m_nextSubmitInfo.pFence);

        // Track this submission so we know when we can reclaim the queue-owned command buffers and fence.
        m_pendingSubmits.PushBack(m_nextSubmitInfo);
        memset(&m_nextSubmitInfo, 0, sizeof(PendingSubmitInfo));
    }

    return result;
}

// =====================================================================================================================
// Processes previous submits, sets/resets the device clock mode for all granularity. Inserts cmd buffer to start thread
// trace for per-frame granularity if tracing is enabled. Shared implementation between the DX and normal present paths.
void Queue::BeginNextFrame(
    bool samplingEnabled)
{
    ProcessIdleSubmits();

    if (samplingEnabled)
    {
        // Change device clock mode to profiling mode if not already enabled.
        // Clock mode is set for the whole frame regardless of the granularity.
        if (m_profilingModeEnabled == false)
        {
            ProfilingClockMode(true);
        }

        if (m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame))
        {
            // Insert a command buffer that has commands to start the thread trace for this frame.
            TargetCmdBuffer*const pStartFrameTgtCmdBuf = AcquireCmdBuf();

            CmdBufferBuildInfo buildInfo = {};
            pStartFrameTgtCmdBuf->Begin(buildInfo);

            // Clear the per frame LogItem
            memset(&m_perFrameLogItem, 0, sizeof(m_perFrameLogItem));
            m_perFrameLogItem.type    = Frame;
            m_perFrameLogItem.frameId = static_cast<Platform*>(m_pDevice->GetPlatform())->FrameId();

            // Begin a GPA session.
            pStartFrameTgtCmdBuf->BeginGpaSession(this);

            const bool perfExp = (m_pDevice->NumGlobalPerfCounters() > 0) ||
                                 (m_pDevice->GetProfilerMode() > GpuProfilerSqttOff);

            pStartFrameTgtCmdBuf->BeginSample(this, &m_perFrameLogItem, false, perfExp);
            pStartFrameTgtCmdBuf->End();

            ICmdBuffer* pNextCmdBuf       = NextCmdBuffer(pStartFrameTgtCmdBuf);
            SubmitInfo nextSubmitInfo     = {};
            nextSubmitInfo.cmdBufferCount = 1;
            nextSubmitInfo.ppCmdBuffers   = &pNextCmdBuf;

            InternalSubmit(nextSubmitInfo, false);
        }

    }
    else if (m_profilingModeEnabled) // Sampling is disabled for all granularity - we must reset the clock mode.
    {
        ProfilingClockMode(false);
    }
}

// =====================================================================================================================
// When the GPU profiler layer is active, the submitted command buffers are really just tokenized ICmdBuffer calls.
// Now, at submit time, we actually generate submittable command buffers (possibly with additional commands to
// gather performance data).
Result Queue::Submit(
    const SubmitInfo& submitInfo)
{
    LogQueueCall(QueueCallId::Submit);

    Result     result        = Result::Success;
    auto*const pPlatform     = m_pDevice->GetPlatform();
    bool       beginNewFrame = false;

    const bool   hasCmdBufInfo   = (submitInfo.pCmdBufInfoList != nullptr);
    const bool   breakBatches    = m_pDevice->ProfilerSettings().gpuProfilerBreakSubmitBatches;
    const uint32 batchCount      = breakBatches ? submitInfo.cmdBufferCount : 1;
    const uint32 cmdBufsPerBatch = breakBatches ? 1 : submitInfo.cmdBufferCount;
    const uint32 maxNextCmdBufs  = cmdBufsPerBatch + 1; // One per recorded CmdBuffer plus the end-frame CmdBuffer.

    AutoBuffer<ICmdBuffer*,  32, PlatformDecorator> nextCmdBuffers(maxNextCmdBufs, pPlatform);
    AutoBuffer<CmdBufInfo,   32, PlatformDecorator> nextCmdBufInfoList(maxNextCmdBufs, pPlatform);
    AutoBuffer<GpuMemoryRef, 32, PlatformDecorator> nextGpuMemoryRefs(submitInfo.gpuMemRefCount, pPlatform);
    AutoBuffer<DoppRef,      32, PlatformDecorator> nextDoppRefs(submitInfo.doppRefCount, pPlatform);

    if ((nextCmdBuffers.Capacity()     < maxNextCmdBufs)            ||
        (nextCmdBufInfoList.Capacity() < maxNextCmdBufs)            ||
        (nextDoppRefs.Capacity()       < submitInfo.doppRefCount)   ||
        (nextGpuMemoryRefs.Capacity()  < submitInfo.gpuMemRefCount))
    {
        result = Result::ErrorOutOfMemory;
    }
    else
    {
        for (uint32 i = 0; i < submitInfo.gpuMemRefCount; i++)
        {
            nextGpuMemoryRefs[i].pGpuMemory   = NextGpuMemory(submitInfo.pGpuMemoryRefs[i].pGpuMemory);
            nextGpuMemoryRefs[i].flags.u32All = submitInfo.pGpuMemoryRefs[i].flags.u32All;
        }

        for (uint32 i = 0; i < submitInfo.doppRefCount; i++)
        {
            nextDoppRefs[i].pGpuMemory   = NextGpuMemory(submitInfo.pDoppRefs[i].pGpuMemory);
            nextDoppRefs[i].flags.u32All = submitInfo.pDoppRefs[i].flags.u32All;
        }

        const IGpuMemory* pNextBlockIfFlipping[MaxBlockIfFlippingCount] = {};
        PAL_ASSERT(submitInfo.blockIfFlippingCount <= MaxBlockIfFlippingCount);

        for (uint32 i = 0; i < submitInfo.blockIfFlippingCount; i++)
        {
            pNextBlockIfFlipping[i] = NextGpuMemory(submitInfo.ppBlockIfFlipping[i]);
        }

        uint32 cmdBufIdx = 0;

        for (uint32 i = 0; (i < batchCount) && (result == Result::Success); i++)
        {
            uint32 cmdBufCnt = 0;

            // In most cases, we want to release all newly acquired objects with each submit, since they are only used
            // by one command buffer.  However, when doing frame-granularity captures, we can't release resources used
            // for pending experiments until the entire frame is complete.  In that case, we will delay setting release
            // objects until the next present.
            bool releaseObjects = (m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame) == false);

            for (uint32 j = 0; j < cmdBufsPerBatch; j++)
            {
                // Get an available queue-owned command buffer for this recorded command buffer.
                auto*const pRecordedCmdBuffer = static_cast<CmdBuffer*>(submitInfo.ppCmdBuffers[cmdBufIdx]);

                // Detect a DX12 app has issues a present that will end a logged frame.
                if (pRecordedCmdBuffer->ContainsPresent() && m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame))
                {
                    // Submit an internal command buffer to end the current frame-long performance experiment.
                    TargetCmdBuffer*const pEndFrameTgtCmdBuf = AcquireCmdBuf();

                    CmdBufferBuildInfo buildInfo = { };
                    pEndFrameTgtCmdBuf->Begin(buildInfo);
                    pEndFrameTgtCmdBuf->EndSample(this, &m_perFrameLogItem);
                    pEndFrameTgtCmdBuf->EndGpaSession(&m_perFrameLogItem);
                    pEndFrameTgtCmdBuf->End();

                    nextCmdBuffers[cmdBufCnt] = NextCmdBuffer(pEndFrameTgtCmdBuf);

                    if (hasCmdBufInfo)
                    {
                        // We need to insert a dummy CmdBufInfo if any caller command buffers specify a CmdBufInfo.
                        nextCmdBufInfoList[cmdBufCnt].isValid = false;
                    }

                    cmdBufCnt++;
                    AddLogItem(m_perFrameLogItem);
                    releaseObjects = true;
                }

                TargetCmdBuffer*const pTargetCmdBuffer = AcquireCmdBuf();

                // For the submit call, we need to make sure this array entry points to the next level ICmdBuffer.
                nextCmdBuffers[cmdBufCnt] = NextCmdBuffer(pTargetCmdBuffer);

                // Replay the client-specified command buffer commands into the queue-owned command buffer.
                pRecordedCmdBuffer->Replay(this,
                                           pTargetCmdBuffer,
                                           static_cast<Platform*>(m_pDevice->GetPlatform())->FrameId());

                if (hasCmdBufInfo)
                {
                    // We need to copy the caller's CmdBufInfo.
                    const CmdBufInfo& cmdBufInfo = submitInfo.pCmdBufInfoList[cmdBufIdx];

                    nextCmdBufInfoList[cmdBufCnt].u32All = cmdBufInfo.u32All;

                    if (cmdBufInfo.isValid)
                    {
                        nextCmdBufInfoList[cmdBufCnt].pPrimaryMemory = NextGpuMemory(cmdBufInfo.pPrimaryMemory);
                    }
                }

                cmdBufCnt++;

                // DX12 apps request a present via a command buffer call. If this command buffer includes one, increment
                // the frame ID. It is expected that only the last command buffer in a submit would request a present.
                if (pRecordedCmdBuffer->ContainsPresent())
                {
                    PAL_ASSERT(cmdBufIdx == (submitInfo.cmdBufferCount - 1));
                    static_cast<Platform*>(m_pDevice->GetPlatform())->IncrementFrameId();
                    beginNewFrame  = true;
                }

                cmdBufIdx++;
            }

            // Make sure we didn't overflow the next arrays.
            PAL_ASSERT(cmdBufCnt <= maxNextCmdBufs);

            // Only pass the client fence on to the next layer if this is the last batch, so that it will only be
            // signaled once all work the client specified in this submit has completed.
            const bool passFence = (cmdBufIdx == submitInfo.cmdBufferCount);

            SubmitInfo nextSubmitInfo           = {};
            nextSubmitInfo.cmdBufferCount       = cmdBufCnt;
            nextSubmitInfo.ppCmdBuffers         = &nextCmdBuffers[0];
            nextSubmitInfo.pCmdBufInfoList      = hasCmdBufInfo ? &nextCmdBufInfoList[0] : nullptr;
            nextSubmitInfo.gpuMemRefCount       = submitInfo.gpuMemRefCount;
            nextSubmitInfo.pGpuMemoryRefs       = &nextGpuMemoryRefs[0];
            nextSubmitInfo.doppRefCount         = submitInfo.doppRefCount;
            nextSubmitInfo.pDoppRefs            = &nextDoppRefs[0];
            nextSubmitInfo.blockIfFlippingCount = submitInfo.blockIfFlippingCount;
            nextSubmitInfo.ppBlockIfFlipping    = &pNextBlockIfFlipping[0];
            nextSubmitInfo.pFence               = passFence ? NextFence(submitInfo.pFence) : nullptr;

            InternalSubmit(nextSubmitInfo, releaseObjects);
        }
    }

    if (beginNewFrame)
    {
        // Begin sampling setup work for next frame for dx path only.
        BeginNextFrame(m_pDevice->LoggingEnabled(GpuProfilerGranularityDraw)   ||
                       m_pDevice->LoggingEnabled(GpuProfilerGranularityCmdBuf) ||
                       m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame));
    }
    else if (m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame) == false)
    {
        // Try to reclaim any newly idle allocations on each submit, unless we're doing a per-frame trace,  in which
        // case we don't want to let CPU utilization, disk I/O, etc. of this process to lead to the GPU being starved.
        ProcessIdleSubmits();
    }

    return result;
}

// =====================================================================================================================
// Log the WaitIdle call and pass it to the next layer.
Result Queue::WaitIdle()
{
    LogQueueCall(QueueCallId::WaitIdle);

    return QueueDecorator::WaitIdle();
}

// =====================================================================================================================
// Log the SignalQueueSemaphore call and pass it to the next layer.
Result Queue::SignalQueueSemaphore(
    IQueueSemaphore* pQueueSemaphore)
{
    LogQueueCall(QueueCallId::SignalQueueSemaphore);

    return QueueDecorator::SignalQueueSemaphore(pQueueSemaphore);
}

// =====================================================================================================================
// Log the WaitQueueSemaphore call and pass it to the next layer.
Result Queue::WaitQueueSemaphore(
    IQueueSemaphore* pQueueSemaphore)
{
    LogQueueCall(QueueCallId::WaitQueueSemaphore);

    return QueueDecorator::WaitQueueSemaphore(pQueueSemaphore);
}

// =====================================================================================================================
// Log the PresentDirect call and pass it to the next layer.
Result Queue::PresentDirect(
    const PresentDirectInfo& presentInfo)
{
    LogQueueCall(QueueCallId::PresentDirect);

    // Do the present before ending any per-frame experiments so that they will capture any present-time GPU work.
    Result result = QueueDecorator::PresentDirect(presentInfo);

    if (m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame) && (m_perFrameLogItem.pGpaSession != nullptr))
    {
        // Submit an internal command buffer to end the current frame-long performance experiment.
        TargetCmdBuffer*const pEndFrameTgtCmdBuf = AcquireCmdBuf();

        CmdBufferBuildInfo buildInfo = { };
        pEndFrameTgtCmdBuf->Begin(buildInfo);
        pEndFrameTgtCmdBuf->EndSample(this, &m_perFrameLogItem);
        pEndFrameTgtCmdBuf->EndGpaSession(&m_perFrameLogItem);
        pEndFrameTgtCmdBuf->End();

        ICmdBuffer* pNextCmdBuf        = NextCmdBuffer(pEndFrameTgtCmdBuf);
        SubmitInfo nextSubmitInfo      = {};
        nextSubmitInfo.cmdBufferCount  = 1;
        nextSubmitInfo.ppCmdBuffers    = &pNextCmdBuf;

        AddLogItem(m_perFrameLogItem);
        InternalSubmit(nextSubmitInfo, true);
    }

    static_cast<Platform*>(m_pDevice->GetPlatform())->IncrementFrameId();

    // Begin sampling setup for next frame.
    BeginNextFrame(m_pDevice->LoggingEnabled(GpuProfilerGranularityDraw)   ||
                   m_pDevice->LoggingEnabled(GpuProfilerGranularityCmdBuf) ||
                   m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame));

    return result;
}

// =====================================================================================================================
// Log the PresentSwapChain call and pass it to the next layer.
Result Queue::PresentSwapChain(
    const PresentSwapChainInfo& presentInfo)
{
    LogQueueCall(QueueCallId::PresentSwapChain);

    // Do the present before ending any per-frame experiments so that they will capture any present-time GPU work.
    // Note: We must always call down to the next layer because we must release ownership of the image index.
    Result result = QueueDecorator::PresentSwapChain(presentInfo);

    if (m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame) && (m_perFrameLogItem.pGpaSession != nullptr))
    {
        // Submit an internal command buffer to end the current frame-long performance experiment.
        TargetCmdBuffer*const pEndFrameTgtCmdBuf = AcquireCmdBuf();

        CmdBufferBuildInfo buildInfo = { };
        pEndFrameTgtCmdBuf->Begin(buildInfo);
        pEndFrameTgtCmdBuf->EndSample(this, &m_perFrameLogItem);
        pEndFrameTgtCmdBuf->EndGpaSession(&m_perFrameLogItem);
        pEndFrameTgtCmdBuf->End();

        ICmdBuffer* pNextCmdBuf        = NextCmdBuffer(pEndFrameTgtCmdBuf);
        SubmitInfo nextSubmitInfo      = {};
        nextSubmitInfo.cmdBufferCount  = 1;
        nextSubmitInfo.ppCmdBuffers    = &pNextCmdBuf;

        AddLogItem(m_perFrameLogItem);
        InternalSubmit(nextSubmitInfo, true);
    }

    static_cast<Platform*>(m_pDevice->GetPlatform())->IncrementFrameId();

    // Begin sampling setup for next frame.
    BeginNextFrame(m_pDevice->LoggingEnabled(GpuProfilerGranularityDraw)   ||
                   m_pDevice->LoggingEnabled(GpuProfilerGranularityCmdBuf) ||
                   m_pDevice->LoggingEnabled(GpuProfilerGranularityFrame));

    return result;
}

// =====================================================================================================================
// Log the Delay call and pass it to the next layer.
Result Queue::Delay(
    float delay)
{
    LogQueueCall(QueueCallId::Delay);

    return QueueDecorator::Delay(delay);
}

// =====================================================================================================================
// Log the RemapVirtualMemoryPages call and pass it to the next layer.
Result Queue::RemapVirtualMemoryPages(
    uint32                         rangeCount,
    const VirtualMemoryRemapRange* pRanges,
    bool                           doNotWait,
    IFence*                        pFence)
{
    LogQueueCall(QueueCallId::RemapVirtualMemoryPages);

    return QueueDecorator::RemapVirtualMemoryPages(rangeCount, pRanges, doNotWait, pFence);
}

// =====================================================================================================================
// Log the CopyVirtualMemoryPageMappings call and pass it to the next layer.
Result Queue::CopyVirtualMemoryPageMappings(
    uint32                                    rangeCount,
    const VirtualMemoryCopyPageMappingsRange* pRanges,
    bool                                      doNotWait)
{
    LogQueueCall(QueueCallId::CopyVirtualMemoryPageMappings);

    return QueueDecorator::CopyVirtualMemoryPageMappings(rangeCount, pRanges, doNotWait);
}

// =====================================================================================================================
// Acquires a queue-owned command buffer for submission of a replayed client command buffer.
TargetCmdBuffer* Queue::AcquireCmdBuf()
{
    TargetCmdBuffer* pCmdBuffer = nullptr;

    if (m_availableCmdBufs.NumElements() > 0)
    {
        // Use an idle command buffer from the pool if available.
        m_availableCmdBufs.PopFront(&pCmdBuffer);
    }
    else
    {
        // No command buffers are currently idle (or possibly none exist at all) - allocate a new command buffer.  Note
        // that we create a GpuProfiler::TargetCmdBuffer here, not a GpuProfiler::CmdBuffer which would just record our
        // commands again!
        CmdBufferCreateInfo createInfo = { };
        createInfo.pCmdAllocator = m_pCmdAllocator;
        createInfo.queueType     = m_queueType;
        createInfo.engineType    = m_engineType;

        void* pMemory = PAL_MALLOC(m_pDevice->GetTargetCmdBufferSize(createInfo, nullptr),
                                   m_pDevice->GetPlatform(),
                                   AllocInternal);

        if (pMemory != nullptr)
        {
            Result result = m_pDevice->CreateTargetCmdBuffer(createInfo, pMemory, &pCmdBuffer);

            if (result != Result::Success)
            {
                PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
            }
        }
    }

    // We always submit command buffers in the order they are acquired, so we can go ahead and add this to the busy
    // queue immediately.
    PAL_ASSERT(pCmdBuffer != nullptr);
    m_busyCmdBufs.PushBack(pCmdBuffer);
    m_nextSubmitInfo.cmdBufCount++;

    return pCmdBuffer;
}

// =====================================================================================================================
// Acquires a queue-owned nested command buffer for execution of a replayed client nested command buffer.
TargetCmdBuffer* Queue::AcquireNestedCmdBuf()
{
    NestedInfo info = {};

    if (m_availableNestedCmdBufs.NumElements() > 0)
    {
        // Use an idle command buffer from the pool if available.
        m_availableNestedCmdBufs.PopFront(&info);
    }
    else
    {
        Result result = Result::Success;

        // No command buffers are currently idle (or possibly none exist at all) - allocate a new command allocator and
        // command buffer.
        void* pMemory = PAL_MALLOC(m_pDevice->GetCmdAllocatorSize(m_nestedAllocatorCreateInfo, &result),
                                   m_pDevice->GetPlatform(),
                                   AllocInternal);

        if ((result == Result::Success) && (pMemory != nullptr))
        {
            result = m_pDevice->CreateCmdAllocator(m_nestedAllocatorCreateInfo, pMemory, &info.pCmdAllocator);

            if (result != Result::Success)
            {
                PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
            }
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }

        if (result == Result::Success)
        {
            // Note that we create a GpuProfiler::TargetCmdBuffer here, not a GpuProfiler::CmdBuffer which would just
            // record our commands again!
            CmdBufferCreateInfo createInfo = {};
            createInfo.pCmdAllocator = info.pCmdAllocator;
            createInfo.queueType     = m_queueType;
            createInfo.engineType    = m_engineType;
            createInfo.flags.nested  = 1;

            pMemory = PAL_MALLOC(m_pDevice->GetTargetCmdBufferSize(createInfo, &result),
                                 m_pDevice->GetPlatform(),
                                 AllocInternal);

            if (pMemory != nullptr)
            {
                result = m_pDevice->CreateTargetCmdBuffer(createInfo, pMemory, &info.pCmdBuffer);

                if (result != Result::Success)
                {
                    PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
                }
            }
        }

        PAL_ASSERT(result == Result::Success);
    }

    // We always submit command buffers in the order they are acquired, so we can go ahead and add this to the busy
    // queue immediately.
    m_busyNestedCmdBufs.PushBack(info);
    m_nextSubmitInfo.nestedCmdBufCount++;

    return info.pCmdBuffer;
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
// =====================================================================================================================
// Acquires a range of queue-owned GPU memory for use by the next command buffer submission.
void Queue::AcquireGpuMem(
    gpusize        size,
    gpusize        alignment,
    GpuHeap        heapType,
    GpuMemoryInfo* pGpuMem,
    gpusize*       pOffset)
{
    Util::Deque<GpuMemoryInfo, Platform>* pBusyList      = &m_busyGartGpuMem;
    Util::Deque<GpuMemoryInfo, Platform>* pAvailableList = &m_availableGartGpuMem;

    GpuMemoryInfo* pCurGpuMem       = &m_curGartGpuMem;
    gpusize*       pCurGpuMemOffset = &m_curGartGpuMemOffset;

    *pCurGpuMemOffset = Pow2Align(*pCurGpuMemOffset, alignment);

    // If there isn't enough space left in the current allocation to fulfill this request, get a new allocation.  This
    // is done in a loop to handle the low GPU memory case where we may need to wait for prior work to finish then
    // try again.
    while ((pCurGpuMem->pGpuMemory == nullptr) || ((*pCurGpuMemOffset) + size > pCurGpuMem->pGpuMemory->Desc().size))
    {
        // Mark the current allocation as busy and associated with the upcoming submit.
        if (pCurGpuMem->pGpuMemory != nullptr)
        {
            pBusyList->PushBack(*pCurGpuMem);
            m_nextSubmitInfo.gpuMemCount++;
        }

        if (pAvailableList->NumElements() > 0)
        {
            // We already have an idle GPU memory allocation in the pool, return that to the caller.
            pAvailableList->PopFront(pCurGpuMem);
        }
        else
        {
            const auto&   settings = m_pDevice->ProfilerSettings();
            const gpusize pageSize = m_pDevice->FragmentSize();

            const gpusize sqttBufferSize = (m_pDevice->GetProfilerMode() > GpuProfilerSqttOff) ?
                                           (settings.gpuProfilerSqttBufferSize * m_shaderEngineCount + pageSize) : 0;

            // If there aren't any idle allocations, allocate a new piece of GPU memory.
            constexpr gpusize MaxRaftSize       = 0x400000ull;
            const gpusize     gpuMemoryRaftSize = Max<gpusize>(MaxRaftSize, Pow2Align(sqttBufferSize, pageSize));
            PAL_ASSERT(size <= gpuMemoryRaftSize);

            GpuMemoryCreateInfo createInfo = { };
            createInfo.size      = gpuMemoryRaftSize;
            createInfo.alignment = pageSize;
            createInfo.vaRange   = VaRange::Default;
            createInfo.heapCount = 1;
            createInfo.heaps[0]  = heapType;
            createInfo.priority  = (heapType == GpuHeapInvisible) ? GpuMemPriority::High : GpuMemPriority::Normal;

            void* pMemory = PAL_MALLOC(m_pDevice->GetGpuMemorySize(createInfo, nullptr),
                                       m_pDevice->GetPlatform(),
                                       AllocInternal);

            Result result = Result::ErrorOutOfMemory;

            if (pMemory != nullptr)
            {
                result = m_pDevice->CreateGpuMemory(createInfo, pMemory, &pCurGpuMem->pGpuMemory);
            }

            if (result == Result::Success)
            {
                // GPU profiler memory is perma-resident.
                GpuMemoryRef memRef = {};
                memRef.pGpuMemory = pCurGpuMem->pGpuMemory;

                result = m_pDevice->AddGpuMemoryReferences(1, &memRef, this, GpuMemoryRefCantTrim);
            }

            if ((result == Result::Success) && (heapType != GpuHeapInvisible))
            {
                // GPU profiler memory is perma-mapped.
                result = pCurGpuMem->pGpuMemory->Map(&pCurGpuMem->pCpuAddr);
            }

            if (result != Result::Success)
            {
                if (pCurGpuMem->pGpuMemory != nullptr)
                {
                    pCurGpuMem->pGpuMemory->Destroy();
                    pCurGpuMem->pGpuMemory = nullptr;
                    pCurGpuMem->pCpuAddr   = nullptr;
                }

                PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());

                // Look for any GPU memory that can be reclaimed by submitting and waiting idle.
                WaitIdle();
                ProcessIdleSubmits();

                // Hitting this assert means that we are out of GPU memory.  Consider enabling
                // GpuProfilerBreakSubmitBatches, reducing the amount of data collected (e.g., reduce
                // GpuProfilerSqttBufferSize or reduce number of global perf counters listed in the
                // GpuProfilerGlobalPerfCounterConfigFile), reducing the granularity in GpuProfilerGranularity (only
                // record data per command buffer instead of per draw), or enable profiling filtering to only gather
                // data under certain circumstances.
                PAL_ASSERT(pAvailableList->NumElements() > 0);
            }
        }

        *pCurGpuMemOffset = 0;
    }

    *pGpuMem = *pCurGpuMem;
    *pOffset = *pCurGpuMemOffset;

    *pCurGpuMemOffset += size;
}

// =====================================================================================================================
// Acquires a queue-owned pipeline stats query.
IQueryPool* Queue::AcquirePipeStatsQuery()
{
    IQueryPool* pQuery= nullptr;

    if (m_availablePipeStatsQueries.NumElements() > 0)
    {
        // Use an idle query if available.
        m_availablePipeStatsQueries.PopFront(&pQuery);
    }
    else
    {
        // No queries are currently idle (or possibly none exist at all) - allocate a new one.
        QueryPoolCreateInfo createInfo = { };
        createInfo.queryPoolType = QueryPoolType::PipelineStats;
        createInfo.numSlots      = 1;
        createInfo.enabledStats  = QueryPipelineStatsAll;

        void* pMemory = PAL_MALLOC(m_pDevice->GetQueryPoolSize(createInfo, nullptr),
                                   m_pDevice->GetPlatform(),
                                   AllocInternal);

        if (pMemory != nullptr)
        {
            Result result = m_pDevice->CreateQueryPool(createInfo, pMemory, &pQuery);

            if (result != Result::Success)
            {
                PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
            }
        }
    }

    PAL_ASSERT(pQuery != nullptr);
    m_busyPipeStatsQueries.PushBack(pQuery);
    m_nextSubmitInfo.pipeStatsQueryCount++;

    // Acquire GPU memory for the query from the pool and bind it.
    GpuMemoryRequirements gpuMemReqs = { };
    pQuery->GetGpuMemoryRequirements(&gpuMemReqs);

    GpuMemoryInfo gpuMemInfo;
    gpusize       offset;
    AcquireGpuMem(gpuMemReqs.size, gpuMemReqs.alignment, GpuHeapGartCacheable, &gpuMemInfo, &offset);

    pQuery->BindGpuMemory(gpuMemInfo.pGpuMemory, offset);

    return pQuery;
}
#endif

// =====================================================================================================================
// Acquires a queue-owned GPA session based on the device's performance experiment requests.
Result Queue::AcquireGpaSession(
    GpuUtil::GpaSession** ppGpaSession)
{
    Result result = Result::Success;

    // A session is acquired from either available list or newly-created
    if (m_availableGpaSessions.NumElements() > 0)
    {
        // Use an idle session if available.
        result = m_availableGpaSessions.PopFront(ppGpaSession);
    }
    else
    {
        const auto& platform = *static_cast<const Platform*>(m_pDevice->GetPlatform());
        // GpuProfiler shouldn't insert rgpInstrumentationVer value, though it's fine set it zero for now.
        // Will need to change later if RGP is uncomfortable with it.
        *ppGpaSession        = PAL_NEW(GpuUtil::GpaSession,
                                       m_pDevice->GetPlatform(),
                                       SystemAllocType::AllocObject)
                                      (
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 318
                                       m_pDevice->GetPlatform(),
#endif
                                       m_pDevice,
                                       platform.ApiMajorVer(),
                                       platform.ApiMinorVer(),
                                       0);
        if (*ppGpaSession != nullptr)
        {
            result = (*ppGpaSession)->Init();
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }
    }

    if (result == Result::Success)
    {
        result = m_busyGpaSessions.PushBack(*ppGpaSession);
    }

    if (result == Result::Success)
    {
        m_nextSubmitInfo.gpaSessionCount++;
    }

    return result;
}

// =====================================================================================================================
// Acquires a queue-owned fence.
IFence* Queue::AcquireFence()
{
    IFence* pFence = nullptr;

    if (m_availableFences.NumElements() > 0)
    {
        // Use an idle fence from the pool if available.
        m_availableFences.PopFront(&pFence);
    }
    else
    {
        // No fences are currently idle (or possibly none exist at all) - allocate a new fence.
        void* pMemory = PAL_MALLOC(m_pDevice->GetFenceSize(nullptr), m_pDevice->GetPlatform(), AllocInternal);

        if (pMemory != nullptr)
        {
            Pal::FenceCreateInfo createInfo = {};
            Result result = m_pDevice->CreateFence(createInfo, pMemory, &pFence);
            if (result != Result::Success)
            {
                PAL_SAFE_FREE(pMemory, m_pDevice->GetPlatform());
            }
        }
    }

    PAL_ASSERT(pFence != nullptr);
    return pFence;
}

// =====================================================================================================================
// Determine if any pending submits have completed, and perform accounting on busy/idle command buffers and fences.
void Queue::ProcessIdleSubmits()
{
    while ((m_pendingSubmits.NumElements() > 0) &&
           (m_pendingSubmits.Front().pFence->GetStatus() == Result::Success))
    {
        PendingSubmitInfo submitInfo = { };
        m_pendingSubmits.PopFront(&submitInfo);

        // Output items from the log item queue that are now known to be idle.
        OutputLogItemsToFile(submitInfo.logItemCount);

        for (uint32 i = 0; i < submitInfo.cmdBufCount; i++)
        {
            TargetCmdBuffer* pCmdBuffer = nullptr;
            m_busyCmdBufs.PopFront(&pCmdBuffer);
            m_availableCmdBufs.PushBack(pCmdBuffer);
        }

        for (uint32 i = 0; i < submitInfo.nestedCmdBufCount; i++)
        {
            NestedInfo info = {};
            m_busyNestedCmdBufs.PopFront(&info);

            // Automatic memory reuse is not enabled so we must manually reset the command buffer and allocator.
            Result result = info.pCmdBuffer->Reset(nullptr, true);

            if (result == Result::Success)
            {
                result = info.pCmdAllocator->Reset();
            }

            PAL_ASSERT(result == Result::Success);

            m_availableNestedCmdBufs.PushBack(info);
        }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
        for (uint32 i = 0; i < submitInfo.gpuMemCount; i++)
        {
            if (m_busyGartGpuMem.NumElements() > 0)
            {
                GpuMemoryInfo info = {};
                m_busyGartGpuMem.PopFront(&info);
                m_availableGartGpuMem.PushBack(info);
            }
        }

        for (uint32 i = 0; i < submitInfo.pipeStatsQueryCount; i++)
        {
            IQueryPool* pQuery = nullptr;
            m_busyPipeStatsQueries.PopFront(&pQuery);

            pQuery->BindGpuMemory(nullptr, 0);

            m_availablePipeStatsQueries.PushBack(pQuery);
        }
#endif

        for (uint32 i = 0; i < submitInfo.gpaSessionCount; i++)
        {
            GpuUtil::GpaSession* pGpaSession = nullptr;
            m_busyGpaSessions.PopFront(&pGpaSession);
            pGpaSession->Reset();
            m_availableGpaSessions.PushBack(pGpaSession);
        }

        m_availableFences.PushBack(submitInfo.pFence);
    }
}

// =====================================================================================================================
// Adds an entry to the queue of logged calls to be processed and outputted.
void Queue::AddLogItem(
    const LogItem& logItem)
{
    m_logItems.PushBack(logItem);
    m_nextSubmitInfo.logItemCount++;
}

// =====================================================================================================================
// Adds a log entry for the specified queue call.
void Queue::LogQueueCall(
    QueueCallId callId)
{
    if (m_pDevice->LoggingEnabled(GpuProfilerGranularityDraw) ||
        m_pDevice->LoggingEnabled(GpuProfilerGranularityCmdBuf))
    {
        LogItem logItem = { };
        logItem.type             = QueueCall;
        logItem.frameId          = static_cast<Platform*>(m_pDevice->GetPlatform())->FrameId();
        logItem.queueCall.callId = callId;
        AddLogItem(logItem);
    }
}

// =====================================================================================================================
// If true is passed it sets the device engine and memory clocks to the stable "profiling mode". Restored on false.
void Queue::ProfilingClockMode(
    bool enable)
{
    m_profilingModeEnabled = enable;

    SetClockModeInput clockModeInput = {};
    clockModeInput.clockMode = enable ? DeviceClockMode::Profiling: DeviceClockMode::Default;

    m_pDevice->SetClockMode(clockModeInput, nullptr);
}

// =====================================================================================================================
// Build sample config data for the creation of GPA session per gpuProfilerCmdBuffer's request
Result Queue::BuildGpaSessionSampleConfig()
{
    const auto& settings = m_pDevice->ProfilerSettings();

    const uint32             numCounters = m_pDevice->NumGlobalPerfCounters();
    const GlobalPerfCounter* pCounters   = m_pDevice->GlobalPerfCounters();

    if (numCounters != 0)
    {
        m_gpaSessionSampleConfig.type = GpuUtil::GpaSampleType::Cumulative;
    }
    else if (m_pDevice->GetProfilerMode() > GpuProfilerSqttOff)
    {
        m_gpaSessionSampleConfig.type = GpuUtil::GpaSampleType::Trace;
    }
    else
    {
        // GpuProfiler layer can choose Cumulative/Trace/None mode by setting up perfCounter info or SQTT info in the
        // panel. Timestamps will be collected as long as the engine that cmdbuf submits to supports timestamp,
        // no matter which mode is chosen. From GpaSession's perspective, timestamp & query are sample types too.
        // But in gpuProfiler, it's not this config info that controls whether to collect timestamp or query.
        // GpuProfiler has it's own logic to control.
        m_gpaSessionSampleConfig.type = GpuUtil::GpaSampleType::None;
    }

    PerfExperimentProperties perfExpProps;

    m_gpaSessionSampleConfig.flags.sampleInternalOperations      = 1;
    m_gpaSessionSampleConfig.flags.cacheFlushOnCounterCollection =
        settings.gpuProfilerCacheFlushOnCounterCollection;

    m_gpaSessionSampleConfig.flags.sqShaderMask                  = 1;
    m_gpaSessionSampleConfig.sqShaderMask                        = PerfShaderMaskAll;

    Result result = m_pDevice->GetPerfExperimentProperties(&perfExpProps);

    if (result == Result::Success)
    {
        m_shaderEngineCount = perfExpProps.shaderEngineCount;

        if (m_gpaSessionSampleConfig.type == GpuUtil::GpaSampleType::Cumulative)
        {
            // Cumulative
            uint32 numTotalInstances = 0;
            for (uint32 i = 0; i < numCounters; i++)
            {
                numTotalInstances += perfExpProps.blocks[static_cast<uint32>(pCounters[i].block)].instanceCount;
            }
            m_gpaSessionSampleConfig.perfCounters.numCounters = numTotalInstances;

            GpuUtil::PerfCounterId* pIds =
                static_cast<GpuUtil::PerfCounterId*>(PAL_MALLOC(numTotalInstances * sizeof(GpuUtil::PerfCounterId),
                                                                m_pDevice->GetPlatform(),
                                                                AllocInternal));

            if (pIds != nullptr)
            {
                m_gpaSessionSampleConfig.perfCounters.pIds = pIds;
                uint32 instanceIndex = 0;
                for (uint32 i = 0; i < numCounters; i++)
                {
                    GpuUtil::PerfCounterId counterInfo;
                    counterInfo.block   = pCounters[i].block;
                    counterInfo.eventId = pCounters[i].eventId;

                    const auto& blockProps = perfExpProps.blocks[static_cast<uint32>(pCounters[i].block)];

                    for (uint32 j = 0; j < blockProps.instanceCount; j++)
                    {
                        counterInfo.instance = j;
                        pIds[instanceIndex++] = counterInfo;
                    }
                }
            }
            else
            {
                result = Result::ErrorOutOfMemory;
            }
        }
        else if (m_gpaSessionSampleConfig.type == GpuUtil::GpaSampleType::Trace)
        {
            m_gpaSessionSampleConfig.sqtt.flags.enable = (m_pDevice->GetProfilerMode() > GpuProfilerSqttOff);
            m_gpaSessionSampleConfig.sqtt.gpuMemoryLimit
                = settings.gpuProfilerSqttBufferSize * perfExpProps.shaderEngineCount;
            m_gpaSessionSampleConfig.sqtt.flags.supressInstructionTokens
                = (settings.gpuProfilerSqThreadTraceTokenMask != 0xFFFF);
        }
        else
        {
            PAL_ASSERT(m_gpaSessionSampleConfig.type == GpuUtil::GpaSampleType::None);
        }

        // Always set timestamp pipe-point in the config info.
        m_gpaSessionSampleConfig.timing.preSample  = HwPipeBottom;
        m_gpaSessionSampleConfig.timing.postSample = HwPipeBottom;
    }

    return result;
}

// =====================================================================================================================
// Destruct sample config info
void Queue::DestroyGpaSessionSampleConfig()
{
    if (m_gpaSessionSampleConfig.perfCounters.pIds != nullptr)
    {
        PAL_SAFE_FREE(m_gpaSessionSampleConfig.perfCounters.pIds, m_pDevice->GetPlatform());
    }

    m_gpaSessionSampleConfig = {};
}

// =====================================================================================================================
// Check if the logItem contains a valid GPA sample.
bool Queue::HasValidGpaSample(
    const LogItem* pLogItem,
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    bool isTimingMode
#else
    GpuUtil::GpaSampleType type
#endif
) const
{
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 355
    uint32 sampleId = isTimingMode ? pLogItem->gpaSampleIdTs : pLogItem->gpaSampleId;
    return ((pLogItem->pGpaSession != nullptr) && (sampleId != GpuUtil::InvalidSampleId));
#else
    uint32 sampleId = GpuUtil::InvalidSampleId;

    if (pLogItem->pGpaSession != nullptr)
    {
        switch (type)
        {
        case GpuUtil::GpaSampleType::Cumulative:
        case GpuUtil::GpaSampleType::Trace:
            sampleId = pLogItem->gpaSampleId;
            break;
        case GpuUtil::GpaSampleType::Timing:
            sampleId = pLogItem->gpaSampleIdTs;
            break;
        case GpuUtil::GpaSampleType::Query:
            sampleId = pLogItem->gpaSampleIdQuery;
            break;
        default:
            break;
        }
    }

    return (sampleId != GpuUtil::InvalidSampleId);
#endif
}

} // GpuProfiler
} // Pal
