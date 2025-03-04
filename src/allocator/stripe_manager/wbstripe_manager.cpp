/*
 *   BSD LICENSE
 *   Copyright (c) 2021 Samsung Electronics Corporation
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Samsung Electronics Corporation nor the names of
 *       its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "src/allocator/stripe_manager/wbstripe_manager.h"

#include <vector>

#include "src/allocator/context_manager/allocator_ctx/allocator_ctx.h"
#include "src/allocator/context_manager/rebuild_ctx/rebuild_ctx.h"
#include "src/allocator/stripe_manager/stripe.h"
#include "src/allocator/stripe_manager/read_stripe.h"
#include "src/allocator/stripe_manager/read_stripe_completion.h"
#include "src/allocator/stripe_manager/stripe_load_status.h"
#include "src/allocator/stripe_manager/write_stripe_completion.h"
#include "src/event_scheduler/event_scheduler.h"
#include "src/include/branch_prediction.h"
#include "src/include/meta_const.h"
#include "src/io/backend_io/flush_submission.h"
#include "src/logger/logger.h"
#include "src/mapper_service/mapper_service.h"
#include "src/qos/qos_manager.h"
#include "src/resource_manager/buffer_pool.h"

namespace pos
{
WBStripeManager::WBStripeManager(TelemetryPublisher* tp_, int numVolumes_, IReverseMap* iReverseMap_,
    IVolumeInfoManager* volManager, IStripeMap* istripeMap_, AllocatorCtx* allocCtx_,
    AllocatorAddressInfo* info, StripeLoadStatus* stripeLoadStatus, std::string arrayName, int arrayId,
    MemoryManager* memoryManager, EventScheduler* eventSchedulerArg)
: stripeBufferPool(nullptr),
  iStripeMap(istripeMap_),
  addrInfo(info),
  tp(tp_),
  arrayName(arrayName),
  arrayId(arrayId),
  memoryManager(memoryManager),
  stripeLoadStatus(stripeLoadStatus)
{
    allocCtx = allocCtx_;
    volumeManager = volManager;
    numVolumes = numVolumes_;
    iReverseMap = iReverseMap_;
    eventScheduler = eventSchedulerArg;
}

WBStripeManager::WBStripeManager(TelemetryPublisher* tp_, AllocatorAddressInfo* info, AllocatorCtx* allocCtx_, std::string arrayName, int arrayId)
: WBStripeManager(tp_, MAX_VOLUME_COUNT, nullptr, nullptr, nullptr, allocCtx_, info, new StripeLoadStatus(), arrayName, arrayId)
{
}
// LCOV_EXCL_START
WBStripeManager::~WBStripeManager(void)
{
    Dispose();
}
// LCOV_EXCL_STOP
void
WBStripeManager::Init(void)
{
    if (iStripeMap == nullptr) // for UT
    {
        iStripeMap = MapperServiceSingleton::Instance()->GetIStripeMap(arrayId);
    }
    if (volumeManager == nullptr) // for UT
    {
        volumeManager = VolumeServiceSingleton::Instance()->GetVolumeManager(arrayId);
    }
    if (iReverseMap == nullptr)
    {
        iReverseMap = MapperServiceSingleton::Instance()->GetIReverseMap(arrayId);
    }
    if (eventScheduler == nullptr)
    {
        eventScheduler = EventSchedulerSingleton::Instance();
    }
    uint32_t totalNvmStripes = addrInfo->GetnumWbStripes();
    uint32_t chunksPerStripe = addrInfo->GetchunksPerStripe();

    BufferInfo info = {
        .owner = typeid(this).name(),
        .size = CHUNK_SIZE,
        .count = totalNvmStripes * chunksPerStripe};
    stripeBufferPool = memoryManager->CreateBufferPool(info);
    if (stripeBufferPool == nullptr)
    {
        POS_TRACE_ERROR(EID(WBSTRIPE_MANAGER_FAILED_TO_GET_BUFFER),
            "owner:{}, size:{}, count:{}", info.owner, info.size, info.count);
    }

    wbStripeArray.resize(totalNvmStripes, nullptr);
}

void
WBStripeManager::Dispose(void)
{
    if (nullptr != stripeLoadStatus)
    {
        delete stripeLoadStatus;
        stripeLoadStatus = nullptr;
    }

    wbStripeArray.clear();

    if (nullptr != stripeBufferPool)
    {
        memoryManager->DeleteBufferPool(stripeBufferPool);
        stripeBufferPool = nullptr;
    }
}

StripeSmartPtr
WBStripeManager::_GetStripe(StripeAddr& lsa)
{
    if (iStripeMap->IsInUserDataArea(lsa))
    {
        return nullptr;
    }
    return wbStripeArray[lsa.stripeId];
}

void
WBStripeManager::FreeWBStripeId(StripeId wblsid)
{
    assert(!IsUnMapStripe(wblsid));
    allocCtx->ReleaseWbStripe(wblsid);
    QosManagerSingleton::Instance()->DecreaseUsedStripeCnt(arrayName);

    assert(wbStripeArray[wblsid] != nullptr);
    wbStripeArray[wblsid] = nullptr;
}

int
WBStripeManager::FlushAllPendingStripesInVolume(int volumeId, FlushIoSmartPtr flushIo)
{
    // TODO (meta) remove volume manager check and remove updating flushIo
    if (volumeManager->GetVolumeMountStatus(volumeId) == Mounted)
    {
        StripeSmartPtr activeStripe = _FinishActiveStripe(volumeId);
        if (activeStripe != nullptr)
        {
            POS_TRACE_INFO(EID(PICKUP_ACTIVE_STRIPE),
                "Picked Active Stripe: volumeId:{}  wbLsid:{}  vsid:{}  remaining:{}",
                volumeId, activeStripe->GetWbLsid(), activeStripe->GetVsid(),
                activeStripe->GetBlksRemaining());
        }

        for (auto it = wbStripeArray.begin(); it != wbStripeArray.end(); ++it)
        {
            StripeSmartPtr stripe = *it;
            if (stripe == nullptr)
            {
                continue;
            }
            if ((uint32_t)volumeId != stripe->GetVolumeId())
            {
                continue;
            }
            stripe->UpdateFlushIo(flushIo);
        }
    }

    return 0;
}

bool
WBStripeManager::ReferLsidCnt(StripeAddr& lsa)
{
    StripeSmartPtr stripe = _GetStripe(lsa);
    if (nullptr == stripe)
    {
        return false;
    }

    stripe->Refer();
    return true;
}

void
WBStripeManager::DereferLsidCnt(StripeAddr& lsa, uint32_t blockCount)
{
    StripeSmartPtr stripe = _GetStripe(lsa);
    if (nullptr == stripe)
    {
        return;
    }
    stripe->Derefer(blockCount);
}

int
WBStripeManager::FlushAllWbStripes(void)
{
    // Complete active stripes and trigger flush
    for (uint32_t volumeId = 0; volumeId < numVolumes; ++volumeId)
    {
        _FinishActiveStripe(volumeId);
    }

    // Wait for all write buffer stripes to be flushed
    for (auto it = wbStripeArray.begin(); it != wbStripeArray.end(); ++it)
    {
        if (*it != nullptr)
        {
            _WaitForStripeFlushComplete(*it);
        }
    }

    return 0;
}

int
WBStripeManager::FlushAllPendingStripesInVolume(int volumeId)
{
    _FinishActiveStripe(volumeId);

    // Wait for all write buffer stripes with volume id to be flushed
    for (auto it = wbStripeArray.begin(); it != wbStripeArray.end(); ++it)
    {
        StripeSmartPtr stripe = *it;
        if (stripe == nullptr)
        {
            continue;
        }
        if ((uint32_t)volumeId != stripe->GetVolumeId())
        {
            continue;
        }

        _WaitForStripeFlushComplete(stripe);
    }

    return 0;
}

void
WBStripeManager::_WaitForStripeFlushComplete(StripeSmartPtr stripe)
{
    assert(stripe != nullptr);

    while (stripe->GetBlksRemaining() > 0)
    {
        usleep(1);
    }
    while (stripe->IsFinished() == false)
    {
        usleep(1);
    }

    // TODO Stripe will be finished after revmap flushed. So waiting for all revmap IO is not valid for now.
    // But we may need to enable it later - when revmap is flushed after user data flushed
    /*
    if (stripe->GetRevMapPack() != nullptr)
    {
        stripe->GetRevMapPack()->WaitPendingIoDone();
    }
    */
}

int
WBStripeManager::ReconstructActiveStripe(uint32_t volumeId, StripeId wbLsid, VirtualBlkAddr tailVsa, std::map<uint64_t, BlkAddr> revMapInfos)
{
    StripeSmartPtr stripe = StripeSmartPtr(new Stripe(iReverseMap, addrInfo->GetblksPerStripe()));
    StripeId vsid = tailVsa.stripeId;
    StripeId userLsid = VsidToUserLsid(vsid);
    stripe->Assign(vsid, wbLsid, userLsid, volumeId);
    AssignStripe(stripe);

    int ret = _ReconstructAS(stripe, tailVsa.offset);
    if (ret >= 0)
    {
        uint64_t totalRbaNum = 0;
        assert(volumeManager != nullptr);
        volumeManager->GetVolumeSize(volumeId, totalRbaNum);
        totalRbaNum = DivideUp(totalRbaNum, BLOCK_SIZE);
        ret = iReverseMap->ReconstructReverseMap(volumeId, totalRbaNum, wbLsid, tailVsa.stripeId, tailVsa.offset, revMapInfos, stripe->GetRevMapPack());
    }
    return ret;
}

// This method should be used only by replay handler
// In replay sequence, stripe flush is not triggered until array is ready to handle i/o though the remaining count reaches zero
// This method will trigger flush for all pended stripes during replay
// In normal sequence, stripe flush is triggered once remaining count reaches zero
int
WBStripeManager::FlushAllPendingStripes(void)
{
    int ret = 0;

    for (auto it = wbStripeArray.begin(); it != wbStripeArray.end(); ++it)
    {
        StripeSmartPtr stripe = *it;
        if (stripe == nullptr)
        {
            continue;
        }
        if (stripe->GetBlksRemaining() == 0 && stripe->IsFinished() == false)
        {
            int flushResult = _RequestStripeFlush(stripe);
            if (flushResult < 0)
            {
                POS_TRACE_ERROR(EID(ALLOCATOR_TRIGGER_FLUSH),
                    "Request stripe flush failed, vsid {} lsid {} remaining {}",
                    stripe->GetVsid(), stripe->GetWbLsid(), stripe->GetBlksRemaining());

                ret = flushResult;
            }
            else
            {
                POS_TRACE_DEBUG(EID(ALLOCATOR_TRIGGER_FLUSH),
                    "Requested stripe flush, vsid {} lsid {} remaining {}",
                    stripe->GetVsid(), stripe->GetWbLsid(), stripe->GetBlksRemaining());
            }
        }
    }

    return ret;
}

void
WBStripeManager::AssignStripe(StripeSmartPtr stripe)
{
    StripeId wbLsid = stripe->GetWbLsid();
    assert(wbStripeArray[wbLsid] == nullptr);
    wbStripeArray[wbLsid] = stripe;
}

StripeSmartPtr
WBStripeManager::GetStripe(StripeId wbLsid)
{
    return wbStripeArray[wbLsid];
}

//------------------------------------------------------------------------------
void
WBStripeManager::FinishStripe(StripeId wbLsid, VirtualBlkAddr tail)
{
    if (wbLsid > addrInfo->GetnumWbStripes())
    {
        POS_TRACE_ERROR(EID(UNKNOWN_ALLOCATOR_ERROR),
            "Requested to finish stripe with wrong wb lsid {}", wbLsid);
        return;
    }

    StripeSmartPtr stripe = wbStripeArray[wbLsid];
    assert(stripe != nullptr);

    VirtualBlks remainingVsaRange = _GetRemainingBlocks(tail);

    bool flushRequired = _FillBlocksToStripe(stripe, wbLsid, remainingVsaRange.startVsa.offset, remainingVsaRange.numBlks);
    if (flushRequired == true)
    {
        // This stripe will be flushed by the following call, FlushAllPendingStripes
        POS_TRACE_INFO(EID(ALLOCATOR_TRIGGER_FLUSH),
            "Stripe is ready to be flushed, wbLsid {}", wbLsid);
    }
}

int
WBStripeManager::_ReconstructAS(StripeSmartPtr stripe, uint64_t blockCount)
{
    if (0 == blockCount)
    {
        POS_TRACE_ERROR(EID(WRONG_BLOCK_COUNT), "Wrong blockCount:{}", blockCount);
        return ERRID(WRONG_BLOCK_COUNT);
    }

    POS_TRACE_DEBUG(EID(ALLOCATOR_RECONSTRUCT_STRIPE),
        "Stripe (vsid {}, wbLsid {}, blockCount {}) is reconstructed",
        stripe->GetVsid(), stripe->GetWbLsid(), blockCount);

    uint32_t remainingBlks = stripe->DecreseBlksRemaining(blockCount);
    if (remainingBlks == 0)
    {
        POS_TRACE_DEBUG(EID(ALLOCATOR_REPLAYED_STRIPE_IS_FULL),
            "Stripe (vsid {}, wbLsid {}) is waiting to be flushed", stripe->GetVsid(), stripe->GetWbLsid());
    }

    return EID(SUCCESS);
}

StripeSmartPtr
WBStripeManager::_FinishActiveStripe(ASTailArrayIdx index)
{
    std::unique_lock<std::mutex> lock(allocCtx->GetActiveStripeTailLock(index));
    VirtualBlkAddr currentTail = allocCtx->GetActiveStripeTail(index);
    if (IsUnMapVsa(currentTail) == true)
    {
        POS_TRACE_DEBUG(EID(PICKUP_ACTIVE_STRIPE),
            "No active stripe for index {}", index);
        return nullptr;
    }

    StripeAddr stripeAddr = iStripeMap->GetLSA(currentTail.stripeId);
    if (stripeAddr.stripeLoc == IN_USER_AREA || stripeAddr.stripeId == UNMAP_STRIPE)
    {
        POS_TRACE_DEBUG(EID(PICKUP_ACTIVE_STRIPE),
            "No active stripe for index {}", index);
        return nullptr;
    }

    StripeId wbLsid = stripeAddr.stripeId;
    VirtualBlks remainingVsaRange = _AllocateRemainingBlocks(index);
    if (IsUnMapVsa(remainingVsaRange.startVsa))
    {
        POS_TRACE_DEBUG(EID(PICKUP_ACTIVE_STRIPE),
            "No active stripe for index {}", index);
        return nullptr;
    }
    else
    {
        POS_TRACE_DEBUG(EID(PICKUP_ACTIVE_STRIPE),
            "Finish active stripe, index {}, wbLsid {}, remaining startVsa stripeId {}, offset {}, numBlks {}",
            index, wbLsid, remainingVsaRange.startVsa.stripeId, remainingVsaRange.startVsa.offset, remainingVsaRange.numBlks);
        return _FinishRemainingBlocks(wbLsid, remainingVsaRange.startVsa.offset, remainingVsaRange.numBlks);
    }
}

VirtualBlks
WBStripeManager::_AllocateRemainingBlocks(ASTailArrayIdx index)
{
    // TODO (meta): Move to allocator context
    VirtualBlkAddr tail = allocCtx->GetActiveStripeTail(index);
    VirtualBlks remainingBlocks = _GetRemainingBlocks(tail);
    allocCtx->SetActiveStripeTail(index, UNMAP_VSA);

    return remainingBlocks;
}

VirtualBlks
WBStripeManager::_GetRemainingBlocks(VirtualBlkAddr tail)
{
    VirtualBlks remainingBlks;

    if (UNMAP_OFFSET == tail.offset)
    {
        remainingBlks.startVsa = UNMAP_VSA;
        remainingBlks.numBlks = 0;
    }
    else if (tail.offset > addrInfo->GetblksPerStripe())
    {
        POS_TRACE_ERROR(EID(WRONG_BLOCK_COUNT),
            "offsetInTail:{} > blksPerStirpe:{}", tail.offset, addrInfo->GetblksPerStripe());

        remainingBlks.startVsa = UNMAP_VSA;
        remainingBlks.numBlks = 0;
    }
    else
    {
        remainingBlks.numBlks = addrInfo->GetblksPerStripe() - tail.offset;
        if (remainingBlks.numBlks == 0)
        {
            remainingBlks.startVsa = UNMAP_VSA;
        }
        else
        {
            remainingBlks.startVsa = tail;
        }
    }

    return remainingBlks;
}

bool
WBStripeManager::_FillBlocksToStripe(StripeSmartPtr stripe, StripeId wbLsid, BlkOffset startOffset, uint32_t numBlks)
{
    uint32_t startBlock = startOffset;
    uint32_t lastBlock = startOffset + numBlks - 1;
    for (uint32_t block = startBlock; block <= lastBlock; ++block)
    {
        stripe->UpdateReverseMapEntry(block, INVALID_RBA, UINT32_MAX);
    }
    stripe->SetActiveFlushTarget();
    uint32_t remain = stripe->DecreseBlksRemaining(numBlks);
    return (remain == 0);
}

StripeSmartPtr
WBStripeManager::_FinishRemainingBlocks(StripeId wbLsid, BlkOffset startOffset, uint32_t numBlks)
{
    StripeSmartPtr activeStripe = wbStripeArray[wbLsid];
    bool flushRequired = _FillBlocksToStripe(activeStripe, wbLsid, startOffset, numBlks);
    if (flushRequired == true)
    {
        int ret = _RequestStripeFlush(activeStripe);
        if (ret == 0)
        {
            POS_TRACE_DEBUG(EID(ALLOCATOR_TRIGGER_FLUSH),
                "Flush stripe (vsid {}, wbLsid {})", activeStripe->GetVsid(), wbLsid);
        }
        else
        {
            POS_TRACE_ERROR(EID(ALLOCATOR_TRIGGER_FLUSH),
                "Request stripe flush failed (vsid {}, wbLsid {})", activeStripe->GetVsid(), wbLsid);
        }
    }

    return activeStripe;
}

int
WBStripeManager::_RequestStripeFlush(StripeSmartPtr stripe)
{
    EventSmartPtr event(new FlushSubmission(stripe, arrayId));
    return stripe->Flush(event);
}

int
WBStripeManager::LoadPendingStripesToWriteBuffer(void)
{
    stripeLoadStatus->Reset();

    for (auto it = wbStripeArray.begin(); it != wbStripeArray.end(); ++it)
    {
        StripeSmartPtr stripe = *it;
        if (stripe == nullptr)
        {
            continue;
        }

        StripeAddr addr = iStripeMap->GetLSA(stripe->GetVsid());
        if (IsUnMapStripe(addr.stripeId))
        {
            continue;
        }

        if (addr.stripeLoc == IN_WRITE_BUFFER_AREA)
        {
            StripeAddr from = {
                .stripeLoc = IN_USER_AREA,
                .stripeId = (*it)->GetUserLsid()};
            StripeAddr to = {
                .stripeLoc = IN_WRITE_BUFFER_AREA,
                .stripeId = (*it)->GetWbLsid()};

            stripeLoadStatus->StripeLoadStarted();
            _LoadStripe(from, to);

            POS_TRACE_INFO(0000,
                "Start loading stripe, vsid {}, wbLsid {}, userLsid {}",
                stripe->GetVsid(), stripe->GetWbLsid(), stripe->GetUserLsid());
        }
    }

    while (stripeLoadStatus->IsDone() == false)
    {
        usleep(1);
    }

    return 0;
}

void
WBStripeManager::_LoadStripe(StripeAddr from, StripeAddr to)
{
    std::vector<void*> bufferList;
    for (uint32_t chunkCnt = 0; chunkCnt < addrInfo->GetchunksPerStripe(); ++chunkCnt)
    {
        void* buffer = stripeBufferPool->TryGetBuffer();
        if (buffer == nullptr)
        {
            POS_TRACE_ERROR(EID(UNKNOWN_ALLOCATOR_ERROR),
                "Failed to allocate buffer for stripe load");
            assert(false);
        }
        bufferList.push_back(buffer);
    }

    CallbackSmartPtr writeStripeCompletion(new WriteStripeCompletion(stripeBufferPool, bufferList, stripeLoadStatus));
    CallbackSmartPtr readStripeCompletion(new ReadStripeCompletion(to, bufferList, writeStripeCompletion, arrayId));
    CallbackSmartPtr readStripe(new ReadStripe(from, bufferList, readStripeCompletion, arrayId));

    eventScheduler->EnqueueEvent(readStripe);
}
} // namespace pos
