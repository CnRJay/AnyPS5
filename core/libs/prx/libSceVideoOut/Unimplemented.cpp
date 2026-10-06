#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceVideoOut/include/Output.hpp"
#include <stdexcept>

extern "C" {

int APS5_VABI sceVideoOutAdjustColor_(int handle, const VideoOutColorSettings* settings, uint32_t settings_size) {
    if (settings_size < sizeof(VideoOutColorSettings)) throw std::runtime_error("sceVideoOutAdjustColor_: VIDEO_OUT_ERROR_INVALID_VALUE");
    return sceVideoOutAdjustColor(handle, settings);
}

int APS5_VABI sceVideoOutColorSettingsSetGamma_(VideoOutColorSettings* settings, float gamma, uint32_t settings_size) {
    if (settings_size < sizeof(VideoOutColorSettings)) throw std::runtime_error("sceVideoOutColorSettingsSetGamma_: VIDEO_OUT_ERROR_INVALID_VALUE");
    return sceVideoOutColorSettingsSetGamma(settings, gamma);
}

APS5_EXPORT("5tRaBjtdTzY", sceVideoOutUnknown_5tRaBjtdTzY);
int APS5_VABI sceVideoOutUnknown_5tRaBjtdTzY(void) {
    NotImplemented_nid_no_patch("5tRaBjtdTzY");
    return 0;
}

APS5_EXPORT("T4ucGB8CsnM", sceVideoOutUnknown_T4ucGB8CsnM);
int APS5_VABI sceVideoOutUnknown_T4ucGB8CsnM(void) {
    NotImplemented_nid_no_patch("T4ucGB8CsnM");
    return 0;
}

}
