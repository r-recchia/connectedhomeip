/*
 *    Copyright (c) 2023 Project CHIP Authors
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

#include <platform/silabs/platformAbstraction/SilabsPlatformBase.h>
#include <stdint.h>
#include <stdio.h>

namespace chip {
namespace DeviceLayer {
namespace Silabs {

class SilabsPlatform : virtual public SilabsPlatformAbstractionBase
{

public:
    enum class ButtonAction : uint8_t
    {
        ButtonReleased = 0,
        ButtonPressed,
        ButtonDisabled,
        InvalidAction
    };

    // Generic Peripherical methods
    CHIP_ERROR Init(void) override;

    // LEDS
#ifdef ENABLE_WSTK_LEDS
    void InitLed(void) override;
    CHIP_ERROR SetLed(bool state, uint8_t led) override;
    bool GetLedState(uint8_t led) override;
    CHIP_ERROR ToggleLed(uint8_t led) override;
#endif

    // Buttons
    inline void SetButtonsCb(SilabsButtonCb callback) override { mButtonCallback = callback; }
    inline uint32_t GetRebootCause() { return mRebootCause; }

    static SilabsButtonCb mButtonCallback;
    uint8_t GetButtonState(uint8_t button) override;

#if defined(SL_CATALOG_CUSTOM_MAIN_PRESENT)
    void StartScheduler(void) override;
#endif // SL_CATALOG_CUSTOM_MAIN_PRESENT

#if (defined(SL_MATTER_RGB_LED_ENABLED) && SL_MATTER_RGB_LED_ENABLED == 1)
    // RGB LEDs
    bool GetRGBLedState(uint8_t led) override;

    CHIP_ERROR SetLedColor(uint8_t led, uint8_t r, uint8_t g, uint8_t b) override;
    CHIP_ERROR GetLedColor(uint8_t led, uint16_t & r, uint16_t & g, uint16_t & b) override;
#endif // (defined(SL_MATTER_RGB_LED_ENABLED) && SL_MATTER_RGB_LED_ENABLED)

    CHIP_ERROR FlashInit() override;
    CHIP_ERROR FlashErasePage(uint32_t addr) override;
    CHIP_ERROR FlashWritePage(uint32_t addr, const uint8_t * data, size_t size) override;

    void SoftwareReset(void) override;

private:
    friend SilabsPlatform & GetPlatform(void);

    // To make underlying SDK thread safe
    void SilabsPlatformLock(void);
    void SilabsPlatformUnlock(void);

    SilabsPlatform(){};
    virtual ~SilabsPlatform() = default;

    uint32_t mRebootCause = 0;
    static SilabsPlatform sSilabsPlatformAbstractionManager;
};

inline SilabsPlatform & GetPlatform(void)
{
    return SilabsPlatform::sSilabsPlatformAbstractionManager;
}

} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
