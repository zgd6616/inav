/*
 * This file is part of INAV.
 *
 * INAV is free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * INAV is distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#if defined(USE_CMS) && defined(USE_VTX_COMMON)

#include "common/printf.h"
#include "common/utils.h"

#include "cms/cms.h"
#include "cms/cms_types.h"

#include "drivers/vtx_common.h"

#include "fc/config.h"

#include "io/vtx.h"
#include "io/vtx_string.h"
#include "io/vtx_tramp.h"

static OSD_INT16_t temperature = { NULL, 0, 0, 0};

static bool vtxCmsDrawStatusString(char *buf, unsigned bufsize)
{
    const char *defaultString = "- -- ---- ----";
//                               m bc ffff tppp
//                               01234567890123

    if (bufsize < strlen(defaultString) + 1) {
        return false;
    }

    strcpy(buf, defaultString);

    vtxDevice_t *vtxDevice = vtxCommonDevice();
    if (!vtxDevice || !vtxCommonDeviceIsReady(vtxDevice)) {
        return true;
    }

    uint8_t band;
    uint8_t channel;

    buf[0] = '*';
    if (vtxCommonGetBandAndChannel(vtxDevice, &band, &channel)) {
        buf[2] = vtxDevice->bandLetters[band];
        buf[3] = vtxDevice->channelNames[channel][0];
    }
    buf[1] = ' ';
    buf[4] = ' ';

    uint16_t frequency;
    if (vtxCommonGetFrequency(vtxDevice, &frequency)) {
        tfp_sprintf(&buf[5], "%4d", frequency);
    }

    if (vtxDevice && vtxCommonGetDeviceType(vtxDevice) == VTXDEV_TRAMP) {
        tfp_sprintf(&buf[9], " %c%3d", (trampData.power == trampData.configuredPower) ? ' ' : '*', trampData.power);
    } else {
        // Fallback to power indexes
        uint8_t powerIndex;
        if (vtxDevice && vtxCommonGetPowerIndex(vtxDevice, &powerIndex)) {
            const char *powerName = vtxDevice->powerNames[powerIndex];
            tfp_sprintf(&buf[9], "%s", powerName);
        }
    }

    return true;
}

static const char * const vtxCmsUnknown[] = {
    "---"
};

static const char * const vtxCmsPitModeNames[] = {
    "---", "OFF", "ON "
};

typedef struct vtxCmsData_s {
    uint8_t pitMode;
    uint8_t band;
    uint8_t channel;
    uint8_t powerIndex;
} vtxCmsData_t;

static vtxCmsData_t vtxInitialData; // Used for keeping the initial values
static vtxCmsData_t vtxData; // Used for editing

static const OSD_TAB_t vtxCmsEntPitMode = { &vtxData.pitMode, 2, vtxCmsPitModeNames };

static uint8_t vtxCmsBand;
static OSD_TAB_t vtxCmsEntBand = { &vtxData.band, 0, NULL };

static uint8_t vtxCmsChan;
static OSD_TAB_t vtxCmsEntChan = { &vtxData.channel, 0, NULL };

static uint16_t vtxCmsFreq;

static uint8_t vtxCmsPower;
static OSD_TAB_t vtxCmsEntPower = { &vtxData.powerIndex, 0, NULL };

static long vtxCmsSetPitMode(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (vtxInitialData.pitMode == 0) {
        // VTX doesn't report pitmode, so editing is not allowed
        vtxData.pitMode = 0;
    } else {
        // Cycle back between ON/OFF, ignore ---
        if (vtxData.pitMode == 0) {
            vtxData.pitMode = 2;
        }
        vtxDevice_t *vtxDevice = vtxCommonDevice();
        if (vtxDevice) {
            vtxCommonSetPitMode(vtxDevice, vtxData.pitMode == 2 ? 1 : 0);
        }
    }

    return 0;
}

static void vtxCmsUpdateFreq(void)
{
    // TODO: This won't work for 2G4 VTX
    vtxCmsFreq = vtx58_Bandchan2Freq(vtxData.band, vtxData.channel);
}

static long vtxCmsConfigBand(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (vtxInitialData.band == 0) {
        vtxData.band = 0;
    } else {
        if (vtxData.band == 0) {
            vtxData.band = vtxCmsEntBand.max - 1;
        }
        vtxCmsUpdateFreq();
    }
    return 0;
}

static long vtxCmsConfigChan(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (vtxInitialData.channel == 0) {
        vtxData.channel = 0;
    } else {
        if (vtxData.channel == 0) {
            vtxData.channel = vtxCmsEntChan.max - 1;
        }
        vtxCmsUpdateFreq();
    }
    return 0;
}

static long vtxCmsConfigPower(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (vtxInitialData.powerIndex == 0) {
        vtxData.powerIndex = 0;
    } else {
        if (vtxData.powerIndex == 0) {
            vtxData.powerIndex = vtxCmsEntPower.max - 1;
        }
    }
    return 0;
}

static long vtxCmsSet(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    // update'vtx_' settings
    vtxSettingsConfigMutable()->band = vtxCmsBand;
    vtxSettingsConfigMutable()->channel = vtxCmsChan;
    vtxSettingsConfigMutable()->power = vtxCmsPower;
    vtxSettingsConfigMutable()->freq = vtx58_Bandchan2Freq(vtxCmsBand, vtxCmsChan);

    saveConfigAndNotify();

    return MENU_CHAIN_BACK;
}

static long vtxCmsOnEnter(const OSD_Entry *from)
{
    UNUSED(from);

    vtxDevice_t *vtxDevice = vtxCommonDevice();
    vtxDeviceCapability_t capabilities;

    if (vtxDevice && vtxCommonGetDeviceCapability(vtxDevice, &capabilities)) {
        vtxCmsEntBand.max = capabilities.bandCount + 1;
        vtxCmsEntBand.names = (const char **)vtxDevice->bandNames;

        vtxCmsEntChan.max = capabilities.channelCount + 1;
        vtxCmsEntChan.names = (const char **)vtxDevice->channelNames;

        vtxCmsEntPower.max = capabilities.powerCount + 1;
        vtxCmsEntPower.names = (const char **)vtxDevice->powerNames;
    } else {
        vtxCmsEntBand.max = 0;
        vtxCmsEntBand.names = vtxCmsUnknown;

        vtxCmsEntChan.max = 0;
        vtxCmsEntChan.names = vtxCmsUnknown;

        vtxCmsEntPower.max = 0;
        vtxCmsEntPower.names = vtxCmsUnknown;
    }

    uint8_t pitMode;
    if (vtxDevice && vtxCommonGetPitMode(vtxDevice, &pitMode)) {
        vtxInitialData.pitMode = pitMode + 1;
    } else {
        vtxInitialData.pitMode = 0;
    }
    vtxData.pitMode = vtxInitialData.pitMode;

    uint8_t band;
    uint8_t channel;
    if (vtxDevice && vtxCommonGetBandAndChannel(vtxDevice, &band, &channel)) {
        // Band and channel are 1-indexed
        vtxInitialData.band = band;
        vtxInitialData.channel = channel;
    } else {
        vtxInitialData.band = 0;
        vtxInitialData.channel = 0;
    }
    vtxData.band = vtxInitialData.band;
    vtxData.channel = vtxInitialData.channel;

    uint8_t powerIndex;
    if (vtxDevice && vtxCommonGetPowerIndex(vtxDevice, &powerIndex)) {
        vtxInitialData.powerIndex = powerIndex + 1;
    } else {
        vtxInitialData.powerIndex = 0;
    }
    vtxData.powerIndex = vtxInitialData.powerIndex;

    vtxCmsUpdateFreq();

    if (vtxDevice && vtxCommonGetDeviceType(vtxDevice) == VTXDEV_TRAMP) {
        temperature.val = &trampData.temperature;
    } else {
        temperature.val = NULL;
    }

    return 0;
}

static const OSD_Entry vtxCmsMenuSetEntries[] =
{
    OSD_LABEL_ENTRY("CONFIRM"),
    OSD_FUNC_CALL_ENTRY("YES", vtxCmsSet),

    OSD_BACK_ENTRY,
    OSD_END_ENTRY,
};

static const CMS_Menu vtxCmsMenuSet = {
#ifdef CMS_MENU_DEBUG
    .GUARD_text = "XVTXS",
    .GUARD_type = OME_MENU,
#endif
    .onEnter = NULL,
    .onExit = NULL,
    .onGlobalExit = NULL,
    .entries = vtxCmsMenuSetEntries,
};

static const OSD_Entry vtxMenuEntries[] =
{
    OSD_LABEL_ENTRY("- VTX -"),

    OSD_LABEL_FUNC_DYN_ENTRY("", vtxCmsDrawStatusString),
    OSD_TAB_CALLBACK_ENTRY("PIT", vtxCmsSetPitMode, &vtxCmsEntPitMode),
    OSD_TAB_CALLBACK_ENTRY("BAND", vtxCmsConfigBand, &vtxCmsEntBand),
    OSD_TAB_CALLBACK_ENTRY("CHAN", vtxCmsConfigChan, &vtxCmsEntChan),
    OSD_UINT16_RO_ENTRY("(FREQ)", &vtxCmsFreq),
    OSD_TAB_CALLBACK_ENTRY("POWER", vtxCmsConfigPower, &vtxCmsEntPower),
    ((OSD_Entry){ "T(C)", OME_INT16, {.func = NULL}, &temperature, DYNAMIC }),
    OSD_SUBMENU_ENTRY("SET", &vtxCmsMenuSet),

    OSD_BACK_ENTRY,
    OSD_END_ENTRY,
};

const CMS_Menu cmsx_menuVtx = {
#ifdef CMS_MENU_DEBUG
    .GUARD_text = "XVTX",
    .GUARD_type = OME_MENU,
#endif
    .onEnter = vtxCmsOnEnter,
    .onExit = NULL,
    .onGlobalExit = NULL,
    .entries = vtxMenuEntries,
};
#endif
