/*
 *
 *    Copyright (c) 2024 Project CHIP Authors
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

#pragma once

#include <app/GenericEventManagementTestEventTriggerHandler.h>

namespace chip {
namespace DeviceLayer {
namespace Infineon {
namespace CYW30739 {

class EventManagementTestEventTriggerHandler : public app::GenericEventManagementTestEventTriggerHandler
{
public:
    EventManagementTestEventTriggerHandler(app::Clusters::GeneralDiagnosticsCluster * generalDiagnosticsClusterInstance = nullptr) :
        app::GenericEventManagementTestEventTriggerHandler(generalDiagnosticsClusterInstance)
    {}

    static constexpr uint64_t kFillUpEventLoggingBuffer = 0xffff'ffff'1388'0000;

    CHIP_ERROR HandleEventTrigger(uint64_t eventTrigger) override;

private:
    virtual void TriggerSoftwareFaultEvent(const char * faultRecordString) override;
};

} // namespace CYW30739
} // namespace Infineon
} // namespace DeviceLayer
} // namespace chip
