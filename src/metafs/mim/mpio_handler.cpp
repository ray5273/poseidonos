/*
 *   BSD LICENSE
 *   Copyright (c) 2022 Samsung Electronics Corporation
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

#include "mpio_handler.h"

#include <string>
#include <tuple>

#include "src/metafs/common/meta_file_util.h"
#include "src/metafs/config/metafs_config_manager.h"
#include "src/telemetry/telemetry_client/telemetry_client.h"

namespace pos
{
MpioHandler::MpioHandler(const int threadId, const int coreId,
    MetaFsConfigManager* configManager, TelemetryPublisher* tp,
    MetaFsIoWrrQ<Mpio*, MetaFileType>* doneQ)
: partialMpioDoneQ(doneQ),
  mpioAllocator(nullptr),
  coreId(coreId),
  telemetryPublisher(tp),
  sampledTimeSpentProcessingAllStages(),
  sampledTimeSpentFromWriteToRelease(),
  sampledTimeSpentFromPushToPop(),
  sampledProcessedMpioCount(),
  writeIoTypeCount(),
  ioCount(),
  metaFsTimeInterval(configManager->GetTimeIntervalInMillisecondsForMetric()),
  skipCount(0),
  SAMPLING_SKIP_COUNT(configManager->GetSamplingSkipCount()),
  doneCountByStorage(),
  doneCountByFileType()
{
    MFS_TRACE_DEBUG(EID(MFS_DEBUG_MESSAGE),
        "threadId={}, coreId={}", threadId, coreId);

    if (nullptr == doneQ)
        partialMpioDoneQ = new MetaFsIoWrrQ<Mpio*, MetaFileType>(configManager->GetWrrWeight());
}

MpioHandler::~MpioHandler(void)
{
    MFS_TRACE_DEBUG(EID(MFS_DEBUG_MESSAGE),
        "MpioHandler is desctructed");

    if (nullptr != partialMpioDoneQ)
        delete partialMpioDoneQ;
}

void
MpioHandler::EnqueuePartialMpio(Mpio* mpio)
{
    mpio->StoreTimestamp(MpioTimestampStage::PushToDoneQ);
    partialMpioDoneQ->Enqueue(mpio, mpio->GetFileType());
}

void
MpioHandler::BindMpioAllocator(MpioAllocator* mpioAllocator)
{
    assert(this->mpioAllocator == nullptr && mpioAllocator != nullptr);
    this->mpioAllocator = mpioAllocator;
}

void
MpioHandler::BottomhalfMioProcessing(void)
{
    Mpio* mpio = partialMpioDoneQ->Dequeue();
    if (mpio)
    {
        mpio->StoreTimestamp(MpioTimestampStage::PopFromDoneQ);

        mpio->ExecuteAsyncState();

        if (mpio->IsCompleted())
        {
            mpio->StoreTimestamp(MpioTimestampStage::Release);
            _UpdateMetricsConditionally(mpio);
            mpioAllocator->Release(mpio);
        }
    }

    mpioAllocator->TryReleaseTheOldestCache();

    _PublishPeriodicMetrics();
}

void
MpioHandler::_UpdateMetricsConditionally(Mpio* mpio)
{
    uint32_t ioType = (uint32_t)mpio->io.opcode;
    if (ioType >= NUM_IO_TYPE)
    {
        POS_TRACE_ERROR(EID(MFS_INVALID_OPCODE), "ioType:{}", ioType);
        assert(false);
    }
    uint32_t filetype = (uint32_t)mpio->GetFileType();
    uint32_t storageType = (int)mpio->io.targetMediaType;
    uint32_t arrayId = mpio->io.arrayId;

    doneCountByFileType[filetype]++;
    doneCountByStorage[storageType]++;

    auto rawData = mpio->GetMetricRawDataAndClear();
    ioCount[arrayId][storageType][(int)MetaIoOpcode::Write] += std::get<0>(rawData);
    ioCount[arrayId][storageType][(int)MetaIoOpcode::Read] += std::get<1>(rawData);

    if (mpio->GetType() == MpioType::Write)
    {
        if (skipCount++ % SAMPLING_SKIP_COUNT == 0)
        {
            sampledTimeSpentProcessingAllStages[ioType] += mpio->GetElapsedInMilli(MpioTimestampStage::Allocate, MpioTimestampStage::Release).count();
            sampledTimeSpentFromWriteToRelease[ioType] += mpio->GetElapsedInMilli(MpioTimestampStage::Write, MpioTimestampStage::Release).count();
            sampledTimeSpentFromPushToPop[ioType] += mpio->GetElapsedInMilli(MpioTimestampStage::PushToDoneQ, MpioTimestampStage::PopFromDoneQ).count();
            sampledProcessedMpioCount[ioType]++;
            skipCount = 0;
        }

        if (mpio->IsPartialIO())
        {
            writeIoTypeCount[filetype][(int)WriteIoType::PartialIo]++;
        }
        else
        {
            writeIoTypeCount[filetype][(int)WriteIoType::FullIo]++;
        }
    }
}

void
MpioHandler::_PublishPeriodicMetrics()
{
    if (telemetryPublisher && metaFsTimeInterval.CheckInterval())
    {
        POSMetricVector* metricVector = new POSMetricVector();

        for (int i = 0; i < (int)MpioType::Max; ++i)
        {
            POSMetric mFreeMpioCount(TEL40300_METAFS_FREE_MPIO_CNT, POSMetricTypes::MT_GAUGE);
            mFreeMpioCount.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(i));
            mFreeMpioCount.SetGaugeValue(mpioAllocator->GetFreeCount((MpioType)i));
            metricVector->emplace_back(mFreeMpioCount);
        }

        for (uint32_t fileType = 0; fileType < (uint32_t)MetaFileType::MAX; ++fileType)
        {
            for (uint32_t i = 0; i < NUM_WRITE_IO_TYPE; ++i)
            {
                POSMetric m(TEL40307_METAFS_MPIO_WRITE_TYPE_COUNT, POSMetricTypes::MT_GAUGE);
                m.AddLabel("full_io", (i == 0) ? "true" : "false");
                m.AddLabel("file_type", MetaFileUtil::ConvertToFileTypeName((MetaFileType)fileType));
                m.SetGaugeValue(writeIoTypeCount[fileType][i]);
                metricVector->emplace_back(m);
                writeIoTypeCount[fileType][i] = 0;
            }
        }

        for (uint32_t storage = 0; storage < NUM_STORAGE_TYPE; ++storage)
        {
            POSMetric m(TEL40104_METAFS_WORKER_DONE_COUNT_PARTITION, POSMetricTypes::MT_GAUGE);
            m.AddLabel("volume_type", MetaFileUtil::ConvertToMediaTypeName((MetaVolumeType)storage));
            m.SetGaugeValue(doneCountByStorage[storage]);
            metricVector->emplace_back(m);
            doneCountByStorage[storage] = 0;

            for (uint32_t ioType = 0; ioType < NUM_IO_TYPE; ++ioType)
            {
                for (uint32_t arrayId = 0; arrayId < MetaFsConfig::MAX_ARRAY_CNT; ++arrayId)
                {
                    POSMetric m(TEL40308_METAFS_MPIO_TOTAL_IO_COUNT, POSMetricTypes::MT_GAUGE);
                    m.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(ioType));
                    m.AddLabel("volume_type", MetaFileUtil::ConvertToMediaTypeName((MetaVolumeType)storage));
                    m.AddLabel("array_id", std::to_string(arrayId));
                    m.SetGaugeValue(ioCount[arrayId][storage][ioType]);
                    metricVector->emplace_back(m);
                    ioCount[arrayId][storage][ioType] = 0;
                }
            }
        }

        for (uint32_t idx = 0; idx < NUM_FILE_TYPE; idx++)
        {
            POSMetric m(TEL40106_METAFS_WORKER_DONE_COUNT_FILE_TYPE, POSMetricTypes::MT_GAUGE);
            m.AddLabel("file_type", MetaFileUtil::ConvertToFileTypeName((MetaFileType)idx));
            m.SetGaugeValue(doneCountByFileType[idx]);
            metricVector->emplace_back(m);
            doneCountByFileType[idx] = 0;
        }

        if (sampledProcessedMpioCount)
        {
            for (uint32_t ioType = 0; ioType < NUM_IO_TYPE; ++ioType)
            {
                POSMetric mTimeSpentAllStage(TEL40201_METAFS_MPIO_TIME_SPENT_PROCESSING_ALL_STAGES, POSMetricTypes::MT_GAUGE);
                mTimeSpentAllStage.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(ioType));
                mTimeSpentAllStage.SetGaugeValue(sampledTimeSpentProcessingAllStages[ioType]);
                metricVector->emplace_back(mTimeSpentAllStage);

                POSMetric mTimeSpentWriteToRelease(TEL40303_METAFS_MPIO_TIME_FROM_WRITE_TO_RELEASE, POSMetricTypes::MT_GAUGE);
                mTimeSpentWriteToRelease.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(ioType));
                mTimeSpentWriteToRelease.SetGaugeValue(sampledTimeSpentFromWriteToRelease[ioType]);
                metricVector->emplace_back(mTimeSpentWriteToRelease);

                POSMetric mTimeSpentPushToPop(TEL40304_METAFS_MPIO_TIME_FROM_PUSH_TO_POP, POSMetricTypes::MT_GAUGE);
                mTimeSpentPushToPop.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(ioType));
                mTimeSpentPushToPop.SetGaugeValue(sampledTimeSpentFromPushToPop[ioType]);
                metricVector->emplace_back(mTimeSpentPushToPop);

                POSMetric m(TEL40305_METAFS_MPIO_SAMPLED_COUNT, POSMetricTypes::MT_GAUGE);
                m.AddLabel("direction", MetaFileUtil::ConvertToDirectionName(ioType));
                m.SetGaugeValue(sampledProcessedMpioCount[ioType]);
                metricVector->emplace_back(m);

                sampledTimeSpentProcessingAllStages[ioType] = 0;
                sampledTimeSpentFromPushToPop[ioType] = 0;
                sampledTimeSpentFromWriteToRelease[ioType] = 0;
                sampledProcessedMpioCount[ioType] = 0;
            }
        }

        for (auto& item : *metricVector)
        {
            item.AddLabel("thread_name", std::to_string(coreId));
        }

        telemetryPublisher->PublishMetricList(metricVector);
    }
}
} // namespace pos
