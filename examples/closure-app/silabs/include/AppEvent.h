/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    Copyright (c) 2025 Nest Labs, Inc.
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

#include "BaseAppEvent.h"

struct AppEvent : public BaseAppEvent
{
    enum AppEventTypes
    {
        kEventType_Closure = BaseAppEvent::kEventType_Max + 1,
        kEventType_Install,
    };
    struct
    {
        uint8_t Action;
        uint16_t EndpointId;
    } ClosureEvent;
};
