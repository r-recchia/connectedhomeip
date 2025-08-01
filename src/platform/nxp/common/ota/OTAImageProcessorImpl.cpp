/*
 *
 *    Copyright (c) 2021-2023, 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <app/clusters/ota-requestor/OTADownloader.h>
#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <lib/support/BufferReader.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/internal/GenericConfigurationManagerImpl.h>

#include <platform/nxp/common/ota/OTAImageProcessorImpl.h>

#include "OtaSupport.h"

using namespace chip::DeviceLayer;
using namespace ::chip::DeviceLayer::Internal;

#if USE_SMU2_STATIC
// The attribute specifier should not be changed.
static chip::OTAImageProcessorImpl gImageProcessor __attribute__((section(".smu2")));
#else
static chip::OTAImageProcessorImpl gImageProcessor;
#endif

namespace chip {

CHIP_ERROR OTAImageProcessorImpl::Init(OTADownloader * downloader)
{
    VerifyOrReturnError(downloader != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    mDownloader = downloader;

    OtaHookInit();

    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::Clear()
{
    mHeaderParser.Clear();
    mAccumulator.Clear();
    mParams.totalFileBytes  = 0;
    mParams.downloadedBytes = 0;
    mCurrentProcessor       = nullptr;

    ReleaseBlock();
}

CHIP_ERROR OTAImageProcessorImpl::PrepareDownload()
{
    DeviceLayer::PlatformMgr().ScheduleWork(HandlePrepareDownload, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::Finalize()
{
    DeviceLayer::PlatformMgr().ScheduleWork(HandleFinalize, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::Apply()
{
    DeviceLayer::PlatformMgr().ScheduleWork(HandleApply, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::Abort()
{
    DeviceLayer::PlatformMgr().ScheduleWork(HandleAbort, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::ProcessBlock(ByteSpan & block)
{
    if ((block.data() == nullptr) || block.empty())
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    // Store block data for HandleProcessBlock to access
    CHIP_ERROR err = SetBlock(block);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(SoftwareUpdate, "Cannot set block data: %" CHIP_ERROR_FORMAT, err.Format());
    }

    DeviceLayer::PlatformMgr().ScheduleWork(HandleProcessBlock, reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::HandlePrepareDownload(intptr_t context)
{
    auto * imageProcessor = reinterpret_cast<OTAImageProcessorImpl *>(context);

    VerifyOrReturn(imageProcessor != nullptr, ChipLogError(SoftwareUpdate, "ImageProcessor context is null"));

    VerifyOrReturn(imageProcessor->mDownloader != nullptr, ChipLogError(SoftwareUpdate, "mDownloader is null"));

    GetRequestorInstance()->GetProviderLocation(imageProcessor->mBackupProviderLocation);

    imageProcessor->mHeaderParser.Init();
    imageProcessor->mAccumulator.Init(sizeof(OTATlvHeader));
    imageProcessor->mDownloader->OnPreparedForDownload(CHIP_NO_ERROR);
}

CHIP_ERROR OTAImageProcessorImpl::ProcessHeader(ByteSpan & block)
{
    OTAImageHeader header;
    ReturnErrorOnFailure(mHeaderParser.AccumulateAndDecode(block, header));

    mParams.totalFileBytes = header.mPayloadSize;
    mHeaderParser.Clear();
    ChipLogError(SoftwareUpdate, "Processed header successfully");

    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::ProcessPayload(ByteSpan & block)
{
    CHIP_ERROR status = CHIP_NO_ERROR;

    while (true)
    {
        if (!mCurrentProcessor)
        {
            ReturnErrorOnFailure(mAccumulator.Accumulate(block));
            ByteSpan tlvHeader{ mAccumulator.data(), sizeof(OTATlvHeader) };
            ReturnErrorOnFailure(SelectProcessor(tlvHeader));
            ReturnErrorOnFailure(mCurrentProcessor->Init());
        }

        status = mCurrentProcessor->Process(block);
        if (status == CHIP_ERROR_OTA_CHANGE_PROCESSOR)
        {
            mAccumulator.Clear();
            mAccumulator.Init(sizeof(OTATlvHeader));

            mCurrentProcessor = nullptr;

            // If the block size is 0, it means that the processed data was a multiple of
            // received BDX block size (e.g. 8 blocks of 1024 bytes were transferred).
            // After state for selecting next processor is reset, a request for fetching next
            // data must be sent.
            if (block.size() == 0)
            {
                status = CHIP_NO_ERROR;
                break;
            }
        }
        else
        {
            break;
        }
    }

    return status;
}

CHIP_ERROR OTAImageProcessorImpl::SelectProcessor(ByteSpan & block)
{
    OTATlvHeader header;
    Encoding::LittleEndian::Reader reader(block.data(), sizeof(header));

    ReturnErrorOnFailure(reader.Read32(&header.tag).StatusCode());
    ReturnErrorOnFailure(reader.Read32(&header.length).StatusCode());

    auto pair = mProcessorMap.find(header.tag);
    if (pair == mProcessorMap.end())
    {
        ChipLogError(SoftwareUpdate, "There is no registered processor for tag: %" PRIu32, header.tag);
        return CHIP_ERROR_OTA_PROCESSOR_NOT_REGISTERED;
    }

    ChipLogDetail(SoftwareUpdate, "Selected processor with tag: %ld", pair->first);
    mCurrentProcessor = pair->second;
    mCurrentProcessor->SetLength(header.length);
    mCurrentProcessor->SetWasSelected(true);

    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::RegisterProcessor(uint32_t tag, OTATlvProcessor * processor)
{
    auto pair = mProcessorMap.find(tag);
    if (pair != mProcessorMap.end())
    {
        ChipLogError(SoftwareUpdate, "A processor for tag %" PRIu32 " is already registered.", tag);
        return CHIP_ERROR_OTA_PROCESSOR_ALREADY_REGISTERED;
    }

    mProcessorMap.insert({ tag, processor });

    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::SetRebootDelaySec(uint16_t rebootDelay)
{
    mDelayBeforeRebootSec = rebootDelay;
}

void OTAImageProcessorImpl::HandleAbort(intptr_t context)
{
    ChipLogError(SoftwareUpdate, "OTA was aborted");
    auto * imageProcessor = reinterpret_cast<OTAImageProcessorImpl *>(context);
    if (imageProcessor != nullptr)
    {
        imageProcessor->AbortAllProcessors();
    }
    imageProcessor->Clear();

    OtaHookAbort();
}

void OTAImageProcessorImpl::HandleProcessBlock(intptr_t context)
{
    auto * imageProcessor = reinterpret_cast<OTAImageProcessorImpl *>(context);

    VerifyOrReturn(imageProcessor != nullptr, ChipLogError(SoftwareUpdate, "ImageProcessor context is null"));

    VerifyOrReturn(imageProcessor->mDownloader != nullptr, ChipLogError(SoftwareUpdate, "mDownloader is null"));

    CHIP_ERROR status;
    auto block = ByteSpan(imageProcessor->mBlock.data(), imageProcessor->mBlock.size());

    if (imageProcessor->mHeaderParser.IsInitialized())
    {
        status = imageProcessor->ProcessHeader(block);
        if (status != CHIP_NO_ERROR)
        {
            imageProcessor->HandleStatus(status);
        }
    }

    status = imageProcessor->ProcessPayload(block);
    imageProcessor->HandleStatus(status);
}

void OTAImageProcessorImpl::HandleStatus(CHIP_ERROR status)
{
    if (status == CHIP_NO_ERROR || status == CHIP_ERROR_BUFFER_TOO_SMALL)
    {
        mParams.downloadedBytes += mBlock.size();
        FetchNextData(0);
    }
    else if (status == CHIP_ERROR_OTA_FETCH_ALREADY_SCHEDULED)
    {
        mParams.downloadedBytes += mBlock.size();
    }
    else
    {
        ChipLogError(SoftwareUpdate, "Image update canceled. Failed to process OTA block: %" CHIP_ERROR_FORMAT, status.Format());
        GetRequestorInstance()->CancelImageUpdate();
    }
}

void OTAImageProcessorImpl::AbortAllProcessors()
{
    ChipLogError(SoftwareUpdate, "All selected processors will call abort action");

    for (auto const & pair : mProcessorMap)
    {
        if (pair.second->WasSelected())
        {
            pair.second->AbortAction();
            pair.second->Clear();
            pair.second->SetWasSelected(false);
        }
    }
}

bool OTAImageProcessorImpl::IsFirstImageRun()
{
    OTARequestorInterface * requestor = chip::GetRequestorInstance();
    if (requestor == nullptr)
    {
        return false;
    }

    return requestor->GetCurrentUpdateState() == OTARequestorInterface::OTAUpdateStateEnum::kApplying;
}

CHIP_ERROR OTAImageProcessorImpl::ConfirmCurrentImage()
{
    uint32_t currentVersion;
    uint32_t targetVersion;

    OTARequestorInterface * requestor = chip::GetRequestorInstance();
    VerifyOrReturnError(requestor != nullptr, CHIP_ERROR_INTERNAL);

    targetVersion = requestor->GetTargetVersion();
    ReturnErrorOnFailure(DeviceLayer::ConfigurationMgr().GetSoftwareVersion(currentVersion));
    if (currentVersion != targetVersion)
    {
        ChipLogError(SoftwareUpdate, "Current sw version %" PRIu32 " is different than the expected sw version = %" PRIu32,
                     currentVersion, targetVersion);
        return CHIP_ERROR_INCORRECT_STATE;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::SetBlock(ByteSpan & block)
{
    if (!IsSpanUsable(block))
    {
        return CHIP_NO_ERROR;
    }

    if (mBlock.size() < block.size())
    {
        if (!mBlock.empty())
        {
            ReleaseBlock();
        }
        uint8_t * mBlock_ptr = static_cast<uint8_t *>(chip::Platform::MemoryAlloc(block.size()));
        if (mBlock_ptr == nullptr)
        {
            return CHIP_ERROR_NO_MEMORY;
        }
        mBlock = MutableByteSpan(mBlock_ptr, block.size());
    }

    CHIP_ERROR err = CopySpanToMutableSpan(block, mBlock);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(SoftwareUpdate, "Cannot copy block data: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }
    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::HandleFinalize(intptr_t context)
{
    auto * imageProcessor = reinterpret_cast<OTAImageProcessorImpl *>(context);
    if (imageProcessor == nullptr)
    {
        return;
    }

    imageProcessor->ReleaseBlock();
}

void OTAImageProcessorImpl::Cleanup()
{
    AbortAllProcessors();
    Clear();
    GetRequestorInstance()->Reset();
}

void OTAImageProcessorImpl::HandleApply(intptr_t context)
{
    CHIP_ERROR error      = CHIP_NO_ERROR;
    auto * imageProcessor = reinterpret_cast<OTAImageProcessorImpl *>(context);
    if (imageProcessor == nullptr)
    {
        return;
    }

    for (auto const & pair : imageProcessor->mProcessorMap)
    {
        if (pair.second->WasSelected())
        {
            error = pair.second->ApplyAction();
            if (error != CHIP_NO_ERROR)
            {
                ChipLogError(SoftwareUpdate, "Apply action for tag %d processor failed.", (uint8_t) pair.first);
                imageProcessor->Cleanup();
                return;
            }
        }
    }

    // Execute OTA_CommitImage once all ApplyAction will be done to avoid app image update when an other OTA processor failed
    if (OTA_CommitImage(NULL) != gOtaSuccess_c)
    {
        ChipLogError(SoftwareUpdate, "Failed to commit firmware image.");
        imageProcessor->Cleanup();
        return;
    }

    for (auto const & pair : imageProcessor->mProcessorMap)
    {
        pair.second->Clear();
        pair.second->SetWasSelected(false);
    }

    imageProcessor->mAccumulator.Clear();

    ConfigurationManagerImpl().StoreSoftwareUpdateCompleted();
    PlatformMgr().HandleServerShuttingDown();
    /*
     * Set the necessary information to inform the SSBL/bootloader that a new image
     * is available and trigger the actual device reboot after some time, to take
     * into account queued actions, e.g. sending events to a subscription.
     */
    SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(imageProcessor->mDelayBeforeRebootSec * 1000 + CHIP_DEVICE_LAYER_OTA_REBOOT_DELAY),
        [](chip::System::Layer *, void *) { OtaHookReset(); }, nullptr);
}

CHIP_ERROR OTAImageProcessorImpl::ReleaseBlock()
{
    if (mBlock.data() != nullptr)
    {
        chip::Platform::MemoryFree(mBlock.data());
    }

    mBlock = MutableByteSpan();
    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::FetchNextData(uint32_t context)
{
    auto * imageProcessor = &OTAImageProcessorImpl::GetDefaultInstance();
    SystemLayer().ScheduleLambda([imageProcessor] {
        if (imageProcessor->mDownloader)
        {
            imageProcessor->mDownloader->FetchNextData();
        }
    });
}

OTAImageProcessorImpl & OTAImageProcessorImpl::GetDefaultInstance()
{
    return gImageProcessor;
}

} // namespace chip
