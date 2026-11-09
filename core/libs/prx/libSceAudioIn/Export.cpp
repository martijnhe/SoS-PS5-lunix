#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <atomic>
#include "prx/common/BlowInput.hpp"

namespace {
constexpr uint32_t kMonoS16Param = 1;
constexpr uint32_t kMaxBlowSamples = 4096;
constexpr int16_t kBlowAmplitude = 24000;
std::atomic<uint32_t> openSamples{0};

void RememberFormat(uint32_t len, uint32_t param) {
 openSamples.store(param == kMonoS16Param && len <= kMaxBlowSamples ? len : 0);
}

// Alternating +A/-A has RMS A/32768 after the plugin's normalization.
int WriteBlowSamples(void* dest) {
 const uint32_t samples = openSamples.load();
 if (dest == nullptr || samples == 0) return 0;
 int16_t* pcm = static_cast<int16_t*>(dest);
 for (uint32_t i = 0; i < samples; ++i) pcm[i] = (i & 1) == 0 ? kBlowAmplitude : static_cast<int16_t>(-kBlowAmplitude);
 return static_cast<int>(samples);
}
}

extern "C" {

int APS5_VABI sceAudioInGetSilentState(int handle) {
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAudioInInput(int handle, void* dest) {
 (void)handle;
 (void)dest;
 if (BlowInput::IsActive()) {
  const int samples = WriteBlowSamples(dest);
  if (samples > 0) return samples;
 }
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param) {
 (void)user_id;
 (void)type;
 (void)index;
 (void)len;
 (void)freq;
 (void)param;
 RememberFormat(len, param);
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioInClose(int32_t handle) {
 (void)handle;
 openSamples.store(0);
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceAudioInHqOpen(int32_t user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param) {
 (void)user_id;
 (void)type;
 (void)index;
 (void)len;
 (void)freq;
 (void)param;
 RememberFormat(len, param);
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

APS5_EXPORT("X+4jdIS75P0", sceAudioInUnknown_X4jdIS75P0);
int32_t APS5_VABI sceAudioInUnknown_X4jdIS75P0(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
