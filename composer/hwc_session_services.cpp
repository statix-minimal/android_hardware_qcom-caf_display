/*
* Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above
*      copyright notice, this list of conditions and the following
*      disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation. nor the names of its
*      contributors may be used to endorse or promote products derived
*      from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Changes from Qualcomm Innovation Center are provided under the following license:
Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted (subject to the limitations in the
disclaimer below) provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of Qualcomm Innovation Center, Inc. nor the
      names of its contributors may be used to endorse or promote
      products derived from this software without specific prior
      written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <core/buffer_allocator.h>
#include <utils/debug.h>
#include <utils/constants.h>
#include <sync/sync.h>
#include <vector>
#include <string>
#include <QtiGralloc.h>

#include "hwc_buffer_sync_handler.h"
#include "hwc_session.h"
#include "hwc_debugger.h"

#define __CLASS__ "HWCSession"

namespace sdm {

void HWCSession::StartServices() {
  int error = DisplayConfig::DeviceInterface::RegisterDevice(this);
  if (error) {
    DLOGW("Could not register IDisplayConfig as service (%d).", error);
  } else {
    DLOGI("IDisplayConfig service registration completed.");
  }
}

int MapDisplayType(DispType dpy) {
  switch (dpy) {
    case DispType::kPrimary:
      return qdutils::DISPLAY_PRIMARY;

    case DispType::kExternal:
      return qdutils::DISPLAY_EXTERNAL;

    case DispType::kVirtual:
      return qdutils::DISPLAY_VIRTUAL;

    case DispType::kBuiltIn2:
      return qdutils::DISPLAY_BUILTIN_2;

    default:
      break;
  }

  return -EINVAL;
}

bool WaitForResourceNeeded(HWC2::PowerMode prev_mode, HWC2::PowerMode new_mode) {
  return ((prev_mode == HWC2::PowerMode::Off) &&
          (new_mode == HWC2::PowerMode::On || new_mode == HWC2::PowerMode::Doze));
}

HWCDisplay::DisplayStatus MapExternalStatus(DisplayConfig::ExternalStatus status) {
  switch (status) {
    case DisplayConfig::ExternalStatus::kOffline:
      return HWCDisplay::kDisplayStatusOffline;

    case DisplayConfig::ExternalStatus::kOnline:
      return HWCDisplay::kDisplayStatusOnline;

    case DisplayConfig::ExternalStatus::kPause:
      return HWCDisplay::kDisplayStatusPause;

    case DisplayConfig::ExternalStatus::kResume:
      return HWCDisplay::kDisplayStatusResume;

    default:
      break;
  }

  return HWCDisplay::kDisplayStatusInvalid;
}

int HWCSession::RegisterClientContext(std::shared_ptr<DisplayConfig::ConfigCallback> callback,
                                      DisplayConfig::ConfigInterface **intf) {
  if (!intf) {
    DLOGE("Invalid DisplayConfigIntf location");
    return -EINVAL;
  }

  std::weak_ptr<DisplayConfig::ConfigCallback> wp_callback = callback;
  DisplayConfigImpl *impl = new DisplayConfigImpl(wp_callback, this);
  *intf = impl;

  return 0;
}

void HWCSession::UnRegisterClientContext(DisplayConfig::ConfigInterface *intf) {
  delete static_cast<DisplayConfigImpl *>(intf);
}

HWCSession::DisplayConfigImpl::DisplayConfigImpl(
                               std::weak_ptr<DisplayConfig::ConfigCallback> callback,
                               HWCSession *hwc_session) {
  callback_ = callback;
  hwc_session_ = hwc_session;
}

int HWCSession::DisplayConfigImpl::IsDisplayConnected(DispType dpy, bool *connected) {
  int disp_id = MapDisplayType(dpy);
  int disp_idx = hwc_session_->GetDisplayIndex(disp_id);

  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  } else {
    SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[disp_idx]);
    *connected = hwc_session_->hwc_display_[disp_idx];
  }

  return 0;
}

int HWCSession::SetDisplayStatus(int disp_id, HWCDisplay::DisplayStatus status) {
  int disp_idx = GetDisplayIndex(disp_id);
  int err = -EINVAL;
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  if (disp_idx == qdutils::DISPLAY_PRIMARY) {
    DLOGE("Not supported for this display");
    return err;
  }

  {
    SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
    if (!hwc_display_[disp_idx]) {
      DLOGW("Display is not connected");
      return err;
    }
    DLOGI("Display = %d, Status = %d", disp_idx, status);
    err = hwc_display_[disp_idx]->SetDisplayStatus(status);
    if (err != 0) {
      return err;
    }
  }

  if (status == HWCDisplay::kDisplayStatusResume || status == HWCDisplay::kDisplayStatusPause) {
    hwc2_display_t active_builtin_disp_id = GetActiveBuiltinDisplay();
    if (active_builtin_disp_id < HWCCallbacks::kNumRealDisplays) {
      callbacks_.Refresh(active_builtin_disp_id);
    }
  }

  return err;
}

int HWCSession::DisplayConfigImpl::SetDisplayStatus(DispType dpy,
                                                    DisplayConfig::ExternalStatus status) {
  return hwc_session_->SetDisplayStatus(MapDisplayType(dpy), MapExternalStatus(status));
}

int HWCSession::DisplayConfigImpl::ConfigureDynRefreshRate(DisplayConfig::DynRefreshRateOp op,
                                                           uint32_t refresh_rate) {
  SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[HWC_DISPLAY_PRIMARY]);
  HWCDisplay *hwc_display = hwc_session_->hwc_display_[HWC_DISPLAY_PRIMARY];

  if (!hwc_display) {
    DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
    return -EINVAL;
  }

  switch (op) {
    case DisplayConfig::DynRefreshRateOp::kDisableMetadata:
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_METADATA_DYN_REFRESH_RATE, false);

    case DisplayConfig::DynRefreshRateOp::kEnableMetadata:
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_METADATA_DYN_REFRESH_RATE, true);

    case DisplayConfig::DynRefreshRateOp::kSetBinder:
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_BINDER_DYN_REFRESH_RATE, refresh_rate);

    default:
      DLOGW("Invalid operation %d", op);
      return -EINVAL;
  }

  return 0;
}

int HWCSession::GetConfigCount(int disp_id, uint32_t *count) {
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);

  if (hwc_display_[disp_idx]) {
    return hwc_display_[disp_idx]->GetDisplayConfigCount(count);
  }

  return -EINVAL;
}

int HWCSession::DisplayConfigImpl::GetConfigCount(DispType dpy, uint32_t *count) {
  return hwc_session_->GetConfigCount(MapDisplayType(dpy), count);
}

int HWCSession::GetActiveConfigIndex(int disp_id, uint32_t *config) {
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);

  if (hwc_display_[disp_idx]) {
    return hwc_display_[disp_idx]->GetActiveDisplayConfig(config);
  }

  return -EINVAL;
}

int HWCSession::DisplayConfigImpl::GetActiveConfig(DispType dpy, uint32_t *config) {
  return hwc_session_->GetActiveConfigIndex(MapDisplayType(dpy), config);
}

int HWCSession::SetActiveConfigIndex(int disp_id, uint32_t config) {
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
  int error = -EINVAL;
  if (hwc_display_[disp_idx]) {
    error = hwc_display_[disp_idx]->SetActiveDisplayConfig(config);
    if (!error) {
      callbacks_.Refresh(0);
    }
  }

  return error;
}

int HWCSession::SetNoisePlugInOverride(int32_t disp_id, bool override_en, int32_t attn,
                                       int32_t noise_zpos) {
  int32_t disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
  int32_t error = -EINVAL;
  if (hwc_display_[disp_idx]) {
    error = hwc_display_[disp_idx]->SetNoisePlugInOverride(override_en, attn, noise_zpos);
  }

  return error;
}

int HWCSession::DisplayConfigImpl::SetActiveConfig(DispType dpy, uint32_t config) {
  return hwc_session_->SetActiveConfigIndex(MapDisplayType(dpy), config);
}

int HWCSession::DisplayConfigImpl::GetDisplayAttributes(uint32_t config_index, DispType dpy,
                                                        DisplayConfig::Attributes *attributes) {
  int error = -EINVAL;
  int disp_id = MapDisplayType(dpy);
  int disp_idx = hwc_session_->GetDisplayIndex(disp_id);

  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
  } else {
    SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[disp_idx]);
    if (hwc_session_->hwc_display_[disp_idx]) {
      DisplayConfigVariableInfo var_info;
      error = hwc_session_->hwc_display_[disp_idx]->GetDisplayAttributesForConfig(INT(config_index),
                                                                                  &var_info);
      if (!error) {
        attributes->vsync_period = var_info.vsync_period_ns;
        attributes->x_res = var_info.x_pixels;
        attributes->y_res = var_info.y_pixels;
        attributes->x_dpi = var_info.x_dpi;
        attributes->y_dpi = var_info.y_dpi;
        attributes->panel_type = DisplayConfig::DisplayPortType::kDefault;
        attributes->is_yuv = var_info.is_yuv;
      }
    }
  }

  return error;
}

int HWCSession::DisplayConfigImpl::SetPanelBrightness(uint32_t level) {
  if (!(0 <= level && level <= 255)) {
    return -EINVAL;
  }

  if (level == 0) {
    return INT32(hwc_session_->SetDisplayBrightness(HWC_DISPLAY_PRIMARY, -1.0f));
  } else {
    return INT32(hwc_session_->SetDisplayBrightness(HWC_DISPLAY_PRIMARY, (level - 1)/254.0f));
  }
}

int HWCSession::DisplayConfigImpl::GetPanelBrightness(uint32_t *level) {
  float brightness = -1.0f;
  int error = -EINVAL;

  error = hwc_session_->getDisplayBrightness(HWC_DISPLAY_PRIMARY, &brightness);
  if (brightness == -1.0f) {
    *level = 0;
  } else {
    *level = static_cast<uint32_t>(254.0f*brightness + 1);
  }

  return error;
}

int HWCSession::MinHdcpEncryptionLevelChanged(int disp_id, uint32_t min_enc_level) {
  DLOGI("Display %d", disp_id);

  // SSG team hardcoded disp_id as external because it applies to external only but SSG team sends
  // this level irrespective of external connected or not. So to honor the call, make disp_id to
  // primary & set level.
  disp_id = HWC_DISPLAY_PRIMARY;
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
  HWCDisplay *hwc_display = hwc_display_[disp_idx];
  if (!hwc_display) {
    DLOGE("Display = %d is not connected.", disp_idx);
    return -EINVAL;
  }

  return hwc_display->OnMinHdcpEncryptionLevelChange(min_enc_level);
}

int HWCSession::DisplayConfigImpl::MinHdcpEncryptionLevelChanged(DispType dpy,
                                                                 uint32_t min_enc_level) {
  return hwc_session_->MinHdcpEncryptionLevelChanged(MapDisplayType(dpy), min_enc_level);
}

int HWCSession::DisplayConfigImpl::RefreshScreen() {
  SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[HWC_DISPLAY_PRIMARY]);
  hwc_session_->callbacks_.Refresh(HWC_DISPLAY_PRIMARY);
  return 0;
}

int HWCSession::ControlPartialUpdate(int disp_id, bool enable) {
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  if (disp_idx != HWC_DISPLAY_PRIMARY) {
    DLOGW("CONTROL_PARTIAL_UPDATE is not applicable for display = %d", disp_idx);
    return -EINVAL;
  }
  {
    SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
    HWCDisplay *hwc_display = hwc_display_[HWC_DISPLAY_PRIMARY];
    if (!hwc_display) {
      DLOGE("primary display object is not instantiated");
      return -EINVAL;
    }

    uint32_t pending = 0;
    DisplayError hwc_error = hwc_display->ControlPartialUpdate(enable, &pending);
    if (hwc_error == kErrorNone) {
      if (!pending) {
        return 0;
      }
    } else if (hwc_error == kErrorNotSupported) {
      return 0;
    } else {
      return -EINVAL;
    }
  }

  // Todo(user): Unlock it before sending events to client. It may cause deadlocks in future.
  // Wait until partial update control is complete
  int error = WaitForCommitDone(HWC_DISPLAY_PRIMARY, kClientPartialUpdate);
  if (error != 0) {
    DLOGW("%s Partial update failed with error %d", enable ? "Enable" : "Disable", error);
  }

  return error;
}

int HWCSession::DisplayConfigImpl::ControlPartialUpdate(DispType dpy, bool enable) {
  return hwc_session_->ControlPartialUpdate(MapDisplayType(dpy), enable);
}

int HWCSession::ToggleScreenUpdate(bool on) {
  hwc2_display_t active_builtin_disp_id = GetActiveBuiltinDisplay();

  if (active_builtin_disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("No active displays");
    return -EINVAL;
  }
  SEQUENCE_WAIT_SCOPE_LOCK(locker_[active_builtin_disp_id]);

  int error = -EINVAL;
  if (hwc_display_[active_builtin_disp_id]) {
    error = hwc_display_[active_builtin_disp_id]->ToggleScreenUpdates(on);
    if (error) {
      DLOGE("Failed to toggle screen updates = %d. Display = %" PRIu64 ", Error = %d", on,
          active_builtin_disp_id, error);
    }
  } else {
    DLOGW("Display = %" PRIu64 " is not connected.", active_builtin_disp_id);
  }

  return error;
}

int HWCSession::DisplayConfigImpl::ToggleScreenUpdate(bool on) {
  return hwc_session_->ToggleScreenUpdate(on);
}

int HWCSession::SetIdleTimeout(uint32_t value) {
  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);

  int inactive_ms = IDLE_TIMEOUT_INACTIVE_MS;
  Debug::Get()->GetProperty(IDLE_TIME_INACTIVE_PROP, &inactive_ms);
  if (hwc_display_[HWC_DISPLAY_PRIMARY]) {
    hwc_display_[HWC_DISPLAY_PRIMARY]->SetIdleTimeoutMs(value, inactive_ms);
    idle_time_inactive_ms_ = inactive_ms;
    idle_time_active_ms_ = value;
    return 0;
  }

  DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
  return -ENODEV;
}

int HWCSession::DisplayConfigImpl::SetIdleTimeout(uint32_t value) {
  return hwc_session_->SetIdleTimeout(value);
}

int HWCSession::DisplayConfigImpl::GetHDRCapabilities(DispType dpy,
                                                      DisplayConfig::HDRCapsParams *caps) {
  int error = -EINVAL;

  do {
    int disp_id = MapDisplayType(dpy);
    int disp_idx = hwc_session_->GetDisplayIndex(disp_id);
    if (disp_idx == -1) {
      DLOGE("Invalid display = %d", disp_id);
      break;
    }

    SCOPE_LOCK(hwc_session_->locker_[disp_id]);
    HWCDisplay *hwc_display = hwc_session_->hwc_display_[disp_idx];
    if (!hwc_display) {
      DLOGW("Display = %d is not connected.", disp_idx);
      error = -ENODEV;
      break;
    }

    // query number of hdr types
    uint32_t out_num_types = 0;
    float out_max_luminance = 0.0f;
    float out_max_average_luminance = 0.0f;
    float out_min_luminance = 0.0f;
    if (hwc_display->GetHdrCapabilities(&out_num_types, nullptr, &out_max_luminance,
                                        &out_max_average_luminance, &out_min_luminance)
                                        != HWC2::Error::None) {
      break;
    }
    if (!out_num_types) {
      error = 0;
      break;
    }

    // query hdr caps
    caps->supported_hdr_types.resize(out_num_types);

    if (hwc_display->GetHdrCapabilities(&out_num_types, caps->supported_hdr_types.data(),
                                        &out_max_luminance, &out_max_average_luminance,
                                        &out_min_luminance) == HWC2::Error::None) {
      error = 0;
    }
  } while (false);

  return error;
}

int HWCSession::SetCameraLaunchStatus(uint32_t on) {
  if (null_display_mode_) {
    return 0;
  }

  if (!core_intf_) {
    DLOGW("core_intf_ not initialized.");
    return -ENOENT;
  }

  HWBwModes mode = on > 0 ? kBwVFEOn : kBwVFEOff;

  if (core_intf_->SetMaxBandwidthMode(mode) != kErrorNone) {
    return -EINVAL;
  }

  // trigger invalidate to apply new bw caps.
  callbacks_.Refresh(0);

  return 0;
}

int HWCSession::DisplayConfigImpl::SetCameraLaunchStatus(uint32_t on) {
  return hwc_session_->SetCameraLaunchStatus(on);
}

int HWCSession::DisplayBWTransactionPending(bool *status) {
  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);

  if (hwc_display_[HWC_DISPLAY_PRIMARY]) {
    if (sync_wait(bw_mode_release_fd_, 0) < 0) {
      DLOGI("bw_transaction_release_fd is not yet signaled: err= %s", strerror(errno));
      *status = false;
    }

    return 0;
  }

  DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
  return -ENODEV;
}

int HWCSession::DisplayConfigImpl::DisplayBWTransactionPending(bool *status) {
  return hwc_session_->DisplayBWTransactionPending(status);
}

int HWCSession::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  hwc2_display_t active_builtin_disp_id = GetActiveBuiltinDisplay();
  if (active_builtin_disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGW("No active displays");
    return 0;
  }
  bool needs_refresh = false;
  {
    SEQUENCE_WAIT_SCOPE_LOCK(locker_[active_builtin_disp_id]);
    if (hwc_display_[active_builtin_disp_id]) {
      if (!enable) {
        if (!idle_pc_ref_cnt_) {
          auto err = hwc_display_[active_builtin_disp_id]->ControlIdlePowerCollapse(enable,
                                                                                    synchronous);
          if (err != kErrorNone) {
            return (err == kErrorNotSupported) ? 0 : -EINVAL;
          }
          needs_refresh = true;
        }
        idle_pc_ref_cnt_++;
      } else if (idle_pc_ref_cnt_ > 0) {
        if (!(idle_pc_ref_cnt_ - 1)) {
          auto err = hwc_display_[active_builtin_disp_id]->ControlIdlePowerCollapse(enable,
                                                                                    synchronous);
          if (err != kErrorNone) {
            return (err == kErrorNotSupported) ? 0 : -EINVAL;
          }
        }
        idle_pc_ref_cnt_--;
      }
    } else {
      DLOGW("Display = %d is not connected.", UINT32(active_builtin_disp_id));
      return -ENODEV;
    }
  }
  if (needs_refresh) {
    int ret = WaitForCommitDone(active_builtin_disp_id, kClientIdlepowerCollapse);
    if (ret != 0) {
      DLOGW("%s Idle PC failed with error %d", enable ? "Enable" : "Disable", ret);
      return -EINVAL;
    }
  }

  DLOGI("Idle PC %s!!", enable ? "enabled" : "disabled");
  return 0;
}

int HWCSession::DisplayConfigImpl::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  return hwc_session_->ControlIdlePowerCollapse(enable, synchronous);
}

int HWCSession::IsWbUbwcSupported(bool *value) {
  HWDisplaysInfo hw_displays_info = {};
  DisplayError error = core_intf_->GetDisplaysStatus(&hw_displays_info);
  if (error != kErrorNone) {
    return -EINVAL;
  }

  for (auto &iter : hw_displays_info) {
    auto &info = iter.second;
    if (info.display_type == kVirtual && info.is_wb_ubwc_supported) {
      *value = 1;
    }
  }

  return error;
}

int32_t HWCSession::getDisplayBrightness(uint32_t display, float *brightness) {
  if (!brightness) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[display]);
  int32_t error = -EINVAL;
  *brightness = -1.0f;

  HWCDisplay *hwc_display = hwc_display_[display];
  if (hwc_display && hwc_display_[display]->GetDisplayClass() == DISPLAY_CLASS_BUILTIN) {
    error = INT32(hwc_display_[display]->GetPanelBrightness(brightness));
    if (error) {
      DLOGE("Failed to get the panel brightness. Error = %d", error);
    }
  }

  return error;
}

int32_t HWCSession::getDisplayMaxBrightness(uint32_t display, uint32_t *max_brightness_level) {
  if (!max_brightness_level) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  int32_t error = -EINVAL;
  HWCDisplay *hwc_display = hwc_display_[display];
  if (hwc_display && hwc_display_[display]->GetDisplayClass() == DISPLAY_CLASS_BUILTIN) {
    error = INT32(hwc_display_[display]->GetPanelMaxBrightness(max_brightness_level));
    if (error) {
      DLOGE("Failed to get the panel max brightness, display %u error %d", display, error);
    }
  }

  return error;
}

int HWCSession::SetCameraSmoothInfo(CameraSmoothOp op, int32_t fps) {
  std::lock_guard<decltype(callbacks_lock_)> lock_guard(callbacks_lock_);
  for (auto const& [id, callback] : callback_clients_) {
    if (callback) {
      callback->notifyCameraSmoothInfo(op, fps);
    }
  }

  return 0;
}

int HWCSession::NotifyResolutionChange(int32_t disp_id, Attributes& attr) {
  std::lock_guard<decltype(callbacks_lock_)> lock_guard(callbacks_lock_);
  for (auto const& [id, callback] : callback_clients_) {
    if (callback) {
      callback->notifyResolutionChange(disp_id, attr);
    }
  }

  return 0;
}

int HWCSession::NotifyFpsMitigation(int32_t disp_id, Attributes attr, Concurrency con) {
  std::lock_guard<decltype(callbacks_lock_)> lock_guard(callbacks_lock_);
  for (auto const& [id, callback] : callback_clients_) {
    if (callback) {
      callback->notifyFpsMitigation(disp_id, attr, con);
    }
  }

  return 0;
}

int HWCSession::RegisterCallbackClient(
        const std::shared_ptr<IDisplayConfigCallback>& callback, int64_t *client_handle) {
  std::lock_guard<decltype(callbacks_lock_)> lock_guard(callbacks_lock_);
  callback_clients_.emplace(callback_client_id_, callback);
  *client_handle = callback_client_id_;
  callback_client_id_++;

  return 0;
}

int HWCSession::UnregisterCallbackClient(const int64_t client_handle) {
  bool removed = false;

  std::lock_guard<decltype(callbacks_lock_)> lock_guard(callbacks_lock_);
  for(auto it = callback_clients_.begin(); it != callback_clients_.end(); ) {
    if (it->first == client_handle) {
      it = callback_clients_.erase(it);
      removed = true;
    } else {
      it++;
    }
  }

  return removed ? 0 : -EINVAL;
}

int HWCSession::DisplayConfigImpl::SetDisplayAnimating(uint64_t display_id, bool animating) {
  return hwc_session_->CallDisplayFunction(display_id, &HWCDisplay::SetDisplayAnimating, animating);
}

int HWCSession::DisplayConfigImpl::GetWriteBackCapabilities(bool *isWbUbwcSupported) {
  return hwc_session_->IsWbUbwcSupported(isWbUbwcSupported);
}

int HWCSession::SetDisplayDppsAdROI(uint32_t display_id, uint32_t h_start,
                                    uint32_t h_end, uint32_t v_start, uint32_t v_end,
                                    uint32_t factor_in, uint32_t factor_out) {
  return CallDisplayFunction(display_id,
                             &HWCDisplay::SetDisplayDppsAdROI, h_start, h_end, v_start, v_end,
                             factor_in, factor_out);
}

int HWCSession::DisplayConfigImpl::SetDisplayDppsAdROI(uint32_t display_id, uint32_t h_start,
                                                       uint32_t h_end, uint32_t v_start,
                                                       uint32_t v_end, uint32_t factor_in,
                                                       uint32_t factor_out) {
  return hwc_session_->SetDisplayDppsAdROI(display_id, h_start, h_end, v_start, v_end,
                                           factor_in, factor_out);
}

int HWCSession::DisplayConfigImpl::UpdateVSyncSourceOnPowerModeOff() {
  hwc_session_->update_vsync_on_power_off_ = true;
  return 0;
}

int HWCSession::DisplayConfigImpl::UpdateVSyncSourceOnPowerModeDoze() {
  hwc_session_->update_vsync_on_doze_ = true;
  return 0;
}

int HWCSession::DisplayConfigImpl::IsPowerModeOverrideSupported(uint32_t disp_id,
                                                                bool *supported) {
  if (!hwc_session_->async_powermode_ || (disp_id > HWCCallbacks::kNumRealDisplays)) {
    *supported = false;
  } else {
    *supported = true;
  }

  return 0;
}

int HWCSession::DisplayConfigImpl::SetPowerMode(uint32_t disp_id,
                                                DisplayConfig::PowerMode power_mode) {
  SCOPE_LOCK(hwc_session_->display_config_locker_);

  bool supported = false;
  IsPowerModeOverrideSupported(disp_id, &supported);
  if (!supported) {
    return 0;
  }
  // Added this flag for pixel
  hwc_session_->async_power_mode_triggered_  = true;
  // Active builtin display needs revalidation
  hwc2_display_t active_builtin_disp_id = hwc_session_->GetActiveBuiltinDisplay();
  HWC2::PowerMode previous_mode = hwc_session_->hwc_display_[disp_id]->GetCurrentPowerMode();

  DLOGI("disp_id: %d power_mode: %d", disp_id, power_mode);
  auto mode = static_cast<HWC2::PowerMode>(power_mode);

  HWCDisplay::HWCLayerStack stack = {};
  hwc2_display_t dummy_disp_id = hwc_session_->map_hwc_display_.at(disp_id);

  // Power state transition start.
  // Acquire the display's power-state transition var read lock.
  hwc_session_->power_state_[disp_id].Lock();
  hwc_session_->power_state_transition_[disp_id] = true;
  hwc_session_->locker_[disp_id].Lock();        // Lock the real display.
  hwc_session_->locker_[dummy_disp_id].Lock();  // Lock the corresponding dummy display.

  // Place the real display's layer-stack on the dummy display.
  hwc_session_->hwc_display_[disp_id]->GetLayerStack(&stack);
  hwc_session_->hwc_display_[dummy_disp_id]->SetLayerStack(&stack);
  hwc_session_->hwc_display_[dummy_disp_id]->UpdatePowerMode(
                                       hwc_session_->hwc_display_[disp_id]->GetCurrentPowerMode());

  buffer_handle_t target = 0;
  shared_ptr<Fence> acquire_fence = nullptr;
  int32_t dataspace = 0;
  hwc_region_t damage = {};
  hwc_session_->hwc_display_[disp_id]->GetClientTarget(
                                 target, acquire_fence, dataspace, damage);
  hwc_session_->hwc_display_[dummy_disp_id]->SetClientTarget(
                                       target, acquire_fence, dataspace, damage);

  // Initialize the variable config map of dummy display using map of real display.
  // Pass the real display's last active config index to dummy display.
  std::map<uint32_t, DisplayConfigVariableInfo> variable_config_map;
  int active_config_index = -1;
  uint32_t num_configs = 0;
  hwc_session_->hwc_display_[disp_id]->GetConfigInfo(&variable_config_map, &active_config_index,
                                                     &num_configs);
  hwc_session_->hwc_display_[dummy_disp_id]->SetConfigInfo(variable_config_map,
                                                           active_config_index, num_configs);

  hwc_session_->locker_[dummy_disp_id].Unlock();  // Release the dummy display.
  // Release the display's power-state transition var read lock.
  hwc_session_->power_state_[disp_id].Unlock();

  // From now, till power-state transition ends, for operations that need to be non-blocking, do
  // those operations on the dummy display.

  if (mode == HWC2::PowerMode::Off || mode == HWC2::PowerMode::DozeSuspend) {
    hwc_session_->active_displays_.erase(disp_id);
  } else {
    hwc_session_->active_displays_.insert(disp_id);
  }
  // Perform the actual [synchronous] power-state change.
  hwc_session_->hwc_display_[disp_id]->SetPowerMode(mode, false /* teardown */);

  // Power state transition end.
  // Acquire the display's power-state transition var read lock.
  hwc_session_->power_state_[disp_id].Lock();
  hwc_session_->power_state_transition_[disp_id] = false;
  hwc_session_->locker_[dummy_disp_id].Lock();  // Lock the dummy display.

  // Retrieve the real display's layer-stack from the dummy display.
  hwc_session_->hwc_display_[dummy_disp_id]->GetLayerStack(&stack);
  hwc_session_->hwc_display_[disp_id]->SetLayerStack(&stack);
  bool vsync_pending = hwc_session_->hwc_display_[dummy_disp_id]->VsyncEnablePending();
  if (vsync_pending) {
    hwc_session_->hwc_display_[disp_id]->SetVsyncEnabled(HWC2::Vsync::Enable);
  }
  hwc_session_->hwc_display_[dummy_disp_id]->GetClientTarget(
                                       target, acquire_fence, dataspace, damage);
  hwc_session_->hwc_display_[disp_id]->SetClientTarget(
                                 target, acquire_fence, dataspace, damage);

  // Read display has got layerstack. Update the fences.
  hwc_session_->hwc_display_[disp_id]->PostPowerMode();

  hwc_session_->locker_[dummy_disp_id].Unlock();  // Release the dummy display.
  hwc_session_->locker_[disp_id].Unlock();        // Release the real display.
  // Release the display's power-state transition var read lock.
  hwc_session_->power_state_[disp_id].Unlock();

  HWC2::PowerMode new_mode = hwc_session_->hwc_display_[disp_id]->GetCurrentPowerMode();
  if (active_builtin_disp_id < HWCCallbacks::kNumRealDisplays &&
      hwc_session_->hwc_display_[disp_id]->IsFirstCommitDone() &&
      WaitForResourceNeeded(previous_mode, new_mode)) {
    hwc_session_->WaitForResources(true, active_builtin_disp_id, disp_id);
  }

  return 0;
}

int HWCSession::DisplayConfigImpl::IsHDRSupported(uint32_t disp_id, bool *supported) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->hdr_locker_[disp_id]);

  if (hwc_session_->is_hdr_display_.size() <= disp_id) {
    DLOGW("is_hdr_display_ is not initialized for display %d!! Reporting it as HDR not supported",
          disp_id);
    *supported = false;
    return 0;
  }

  *supported = static_cast<bool>(hwc_session_->is_hdr_display_[disp_id]);
  return 0;
}

int HWCSession::DisplayConfigImpl::IsWCGSupported(uint32_t disp_id, bool *supported) {
  // todo(user): Query wcg from sdm. For now assume them same.
  return IsHDRSupported(disp_id, supported);
}

int HWCSession::DisplayConfigImpl::SetLayerAsMask(uint32_t disp_id, uint64_t layer_id) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  HWCDisplay *hwc_display = hwc_session_->hwc_display_[disp_id];
  if (!hwc_display) {
    DLOGW("Display = %d is not connected.", disp_id);
    return -EINVAL;
  }

  if (hwc_session_->disable_mask_layer_hint_) {
    DLOGW("Mask layer hint is disabled!");
    return -EINVAL;
  }

  auto hwc_layer = hwc_display->GetHWCLayer(layer_id);
  if (hwc_layer == nullptr) {
    return -EINVAL;
  }

  hwc_layer->SetLayerAsMask();

  return 0;
}

int HWCSession::DisplayConfigImpl::GetDebugProperty(const std::string prop_name,
                                                    std::string *value) {
  std::string vendor_prop_name = DISP_PROP_PREFIX;
  int error = -EINVAL;
  char val[64] = {};

  vendor_prop_name += prop_name.c_str();
  if (HWCDebugHandler::Get()->GetProperty(vendor_prop_name.c_str(), val) == kErrorNone) {
    *value = val;
    error = 0;
  }

  return error;
}

int HWCSession::DisplayConfigImpl::GetActiveBuiltinDisplayAttributes(
                                                    DisplayConfig::Attributes *attr) {
  int error = -EINVAL;
  hwc2_display_t disp_id = hwc_session_->GetActiveBuiltinDisplay();

  if (disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Invalid display = %d", UINT32(disp_id));
  } else {
    if (hwc_session_->hwc_display_[disp_id]) {
      uint32_t config_index = 0;
      HWC2::Error ret = hwc_session_->hwc_display_[disp_id]->GetActiveConfig(&config_index);
      if (ret != HWC2::Error::None) {
        goto err;
      }
      DisplayConfigVariableInfo var_info;
      error = hwc_session_->hwc_display_[disp_id]->GetDisplayAttributesForConfig(INT(config_index),
                                                                                 &var_info);
      if (!error) {
        attr->vsync_period = var_info.vsync_period_ns;
        attr->x_res = var_info.x_pixels;
        attr->y_res = var_info.y_pixels;
        attr->x_dpi = var_info.x_dpi;
        attr->y_dpi = var_info.y_dpi;
        attr->panel_type = DisplayConfig::DisplayPortType::kDefault;
        attr->is_yuv = var_info.is_yuv;
      }
    }
  }

err:
  return error;
}

int HWCSession::DisplayConfigImpl::SetPanelLuminanceAttributes(uint32_t disp_id, float pan_min_lum,
                                                               float pan_max_lum) {
  // currently doing only for virtual display
  if (disp_id != qdutils::DISPLAY_VIRTUAL) {
    return -EINVAL;
  }

  // check for out of range luminance values
  if (pan_min_lum <= 0.0f || pan_min_lum >= 1.0f ||
      pan_max_lum <= 100.0f || pan_max_lum >= 1000.0f) {
    return -EINVAL;
  }

  std::lock_guard<std::mutex> obj(hwc_session_->mutex_lum_);
  hwc_session_->set_min_lum_ = pan_min_lum;
  hwc_session_->set_max_lum_ = pan_max_lum;
  DLOGI("set max_lum %f, min_lum %f", pan_max_lum, pan_min_lum);

  return 0;
}

int HWCSession::DisplayConfigImpl::IsBuiltInDisplay(uint32_t disp_id, bool *is_builtin) {
  if ((hwc_session_->map_info_primary_.client_id == disp_id) &&
      (hwc_session_->map_info_primary_.disp_type == kBuiltIn)) {
    *is_builtin = true;
    return 0;
  }

  for (auto &info : hwc_session_->map_info_builtin_) {
    if (disp_id == info.client_id) {
      *is_builtin = true;
      return 0;
    }
  }

  *is_builtin = false;
  return 0;
}

int HWCSession::DisplayConfigImpl::GetSupportedDSIBitClks(uint32_t disp_id,
                                                          std::vector<uint64_t> *bit_clks) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_id]) {
    return -EINVAL;
  }

  hwc_session_->hwc_display_[disp_id]->GetSupportedDSIClock(bit_clks);
  return 0;
}

int HWCSession::DisplayConfigImpl::GetDSIClk(uint32_t disp_id, uint64_t *bit_clk) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_id]) {
    return -EINVAL;
  }

  hwc_session_->hwc_display_[disp_id]->GetDynamicDSIClock(bit_clk);

  return 0;
}

int HWCSession::DisplayConfigImpl::SetDSIClk(uint32_t disp_id, uint64_t bit_clk) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_id]) {
    return -1;
  }

  return hwc_session_->hwc_display_[disp_id]->SetDynamicDSIClock(bit_clk);
}

int HWCSession::DisplayConfigImpl::SetCWBOutputBuffer(uint32_t disp_id,
                                                      const DisplayConfig::Rect rect,
                                                      bool post_processed,
                                                      const native_handle_t *buffer) {
  if (!callback_.lock()) {
    DLOGE("Callback_ has not yet been initialized.");
    return -1;
  }

  if (!buffer) {
    DLOGE("Buffer is null.");
    return -1;
  }

  // Output buffer dump is not supported, if Virtual display is present.
  int dpy_index = hwc_session_->GetDisplayIndex(qdutils::DISPLAY_VIRTUAL);
  if ((dpy_index != -1) && hwc_session_->hwc_display_[dpy_index]) {
    DLOGW("Output buffer dump is not supported with Virtual display!");
    return -1;
  }

  hwc2_display_t disp_type = HWC_DISPLAY_PRIMARY;
  if (disp_id == UINT32(DisplayConfig::DisplayType::kPrimary)) {
    dpy_index = hwc_session_->GetDisplayIndex(qdutils::DISPLAY_PRIMARY);
  } else if (disp_id == UINT32(DisplayConfig::DisplayType::kExternal)) {
    dpy_index = hwc_session_->GetDisplayIndex(qdutils::DISPLAY_EXTERNAL);
    disp_type = HWC_DISPLAY_EXTERNAL;
  } else if (disp_id == UINT32(DisplayConfig::DisplayType::kBuiltIn2)) {
    dpy_index = hwc_session_->GetDisplayIndex(qdutils::DISPLAY_BUILTIN_2);
    disp_type = qdutils::HWC_DISPLAY_BUILTIN_2;
  } else {
    DLOGE("CWB is supported on primary and external displays only at present.");
    return -1;
  }

  if (dpy_index == -1) {
    DLOGW("Unable to retrieve display index for display:%d", disp_id);
    return -1;
  }

  // Mutex scope
  {
    SCOPE_LOCK(hwc_session_->locker_[disp_type]);
    if (!hwc_session_->hwc_display_[dpy_index]) {
      DLOGE("Display is not created yet.");
      return -1;
    }
  }

  CwbConfig cwb_config = {};
  cwb_config.tap_point = static_cast<CwbTapPoint>(post_processed);
  LayerRect &roi = cwb_config.cwb_roi;
  roi.left = FLOAT(rect.left);
  roi.top = FLOAT(rect.top);
  roi.right = FLOAT(rect.right);
  roi.bottom = FLOAT(rect.bottom);

  DLOGI("CWB config passed by cwb_client : tappoint %d  CWB_ROI : (%f %f %f %f)",
        cwb_config.tap_point, roi.left, roi.top, roi.right, roi.bottom);

  return hwc_session_->cwb_.PostBuffer(callback_, cwb_config, native_handle_clone(buffer),
                                       disp_type);
}

int32_t HWCSession::CWB::PostBuffer(std::weak_ptr<DisplayConfig::ConfigCallback> callback,
                                    const CwbConfig &cwb_config, const native_handle_t *buffer,
                                    hwc2_display_t display_type) {
  SCOPE_LOCK(queue_lock_);

  // Ensure that async task runs only until all queued CWB requests have been fulfilled.
  // If cwb queue is empty, async task has not either started or async task has finished
  // processing previously queued cwb requests. Start new async task on such a case as
  // currently running async task will automatically desolve without processing more requests.
  bool post_future = !queue_.size();

  if (!post_future) {  // if queue_ is not empty
    // reject the cwb request if it's made on another display than the currently cwb active display
    shared_ptr<QueueNode> node = queue_.front();
    if (node->display_type != display_type) {
      native_handle_close(buffer);
      native_handle_delete(const_cast<native_handle_t *>(buffer));
      return -1;
    }
  }

  shared_ptr<QueueNode> node(new QueueNode(callback, cwb_config, buffer, display_type));
  queue_.push(node);

  if (post_future) {
    // No need to do future.get() here for previously running async task. Async method will
    // guarantee to exit after cwb for all queued requests is indeed complete i.e. the respective
    // fences have signaled and client is notified through registered callbacks. This will make
    // sure that the new async task does not concurrently work with previous task. Let async running
    // thread dissolve on its own.
    future_ = std::async(HWCSession::CWB::AsyncTask, this);
  }

  return 0;
}

void HWCSession::CWB::ProcessRequests() {
  while (true) {
    shared_ptr<QueueNode> node = nullptr;
    int status = 0;
    HWC2::Error error = HWC2::Error::None;

    // Mutex scope
    // Just check if there is a next cwb request queued, exit the thread if nothing is pending.
    // Do not keep mutex locked so that client can freely queue more jobs to the current thread.
    {
      SCOPE_LOCK(queue_lock_);
      if (!queue_.size()) {
        DLOGI("CWB buffer is empty. No pending CWB requests found.");
        break;
      }

      node = queue_.front();
    }

    HWCDisplay *hwc_display = hwc_session_->hwc_display_[node->display_type];
    Locker &locker = hwc_session_->locker_[node->display_type];

    // Configure cwb parameters, trigger refresh, wait for commit, get the release fence and
    // wait for fence to signal.

    // Mutex scope
    // Wait for previous commit to finish before configuring next buffer.
    {
      SEQUENCE_WAIT_SCOPE_LOCK(locker);

      error = hwc_display->SetReadbackBuffer(node->buffer, nullptr, node->cwb_config,
                                             kCWBClientExternal);
      if (error == HWC2::Error::None) {
        DLOGI("Successfully configured CWB buffer.");
      } else if (error == HWC2::Error::NoResources || error == HWC2::Error::Unsupported ||
                 error == HWC2::Error::BadDisplay) {  // if cwb request is made either when another
        // display/client is performing CWB or when CWB tearing down then notify warning status -2.
        status = -2;
      } else {  // notify error status -1 to the CWB client
        status = -1;
      }
    }

    if (error != HWC2::Error::BadDisplay) {  // Don't wait if CWB requested on powered-off display.
      hwc_session_->callbacks_.Refresh(node->display_type);

      // Mutex scope
      // Wait for the signal from commit thread to retrieve the CWB release fence
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock);
      }
    }

    if (!status) {
      std::lock_guard<std::mutex> lock(command_seq_mutex_);
      shared_ptr<Fence> release_fence = nullptr;
      // Mutex scope
      {
        SCOPE_LOCK(locker);
        error = hwc_display->GetReadbackBufferFence(&release_fence);
        if (error != HWC2::Error::None) {
          status = -1;
        }
      }
      if (!status) {
        bool is_waiting = true;
        {
          SCOPE_LOCK(fence_queue_lock_);
          // check whether fence wait is going on in another async thread.
          is_waiting = static_cast<bool>(fence_wait_queue_.size());
          // push current cwb request's release fence in the fence wait queue.
          DLOGI("Pushing release fence in fenece wait queue.");
          fence_wait_queue_.push({release_fence, node});
        }
        if (!is_waiting) {  // incase of empty fence wait queue, create a new async thread
          DLOGI("Creating AsyncFenceWaits async thread.");
          // to perform fence wait on the cwb release fence.
          fence_wait_future_ = std::async(HWCSession::CWB::AsyncFenceWaits, this);
        }
      }
    }

    if (status) {  // for erroneous status, notify the cwb client.
      NotifyCWBStatus(status, node);
    }

    // Mutex scope
    // Make sure to exit here, if queue becomes empty after erasing current node from queue,
    // so that the current async task does not operate concurrently with a new future task.
    {
      SCOPE_LOCK(queue_lock_);
      queue_.pop();
      DLOGI("Remove current CWB request from the pending request queue.");

      if (!queue_.size()) {
        DLOGI("No pending cwb requests found in the queue.");
        break;
      }
    }
  }
}

void HWCSession::CWB::PerformFenceWaits() {
  while (true) {
    shared_ptr<Fence> release_fence = nullptr;
    shared_ptr<QueueNode> cwb_node = nullptr;
    {
      SCOPE_LOCK(fence_queue_lock_);
      if (!fence_wait_queue_.size()) {
        DLOGI("CWB fence queue is empty. No pending release fence to wait on.");
        break;
      }

      release_fence = fence_wait_queue_.front().first;
      cwb_node = fence_wait_queue_.front().second;
    }

    int status = 0;
    if (release_fence >= 0) {
      status = Fence::Wait(release_fence);
    } else {
      DLOGE("CWB release fence could not be retrieved.");
      status = -1;
    }

    NotifyCWBStatus(status, cwb_node);

    {
      SCOPE_LOCK(fence_queue_lock_);
      fence_wait_queue_.pop();
      DLOGI("Remove current release fence from the pending fence wait queue.");

      if (!fence_wait_queue_.size()) {
        DLOGI("CWB fence queue is empty. No pending release fence to wait on.");
        break;
      }
    }
  }
}

void HWCSession::CWB::NotifyCWBStatus(int status, shared_ptr<QueueNode> cwb_node) {
  // Notify client about buffer status and erase the node from pending request queue.
  std::shared_ptr<DisplayConfig::ConfigCallback> callback = cwb_node->callback.lock();
  if (callback) {
    DLOGI("Notify the client about buffer status %d.", status);
    callback->NotifyCWBBufferDone(status, cwb_node->buffer);
  }
}

void HWCSession::CWB::AsyncTask(CWB *cwb) {
  cwb->ProcessRequests();
}

void HWCSession::CWB::AsyncFenceWaits(CWB *cwb) {
  cwb->PerformFenceWaits();
}

void HWCSession::CWB::PresentDisplayDone(hwc2_display_t disp_id) {
  if (disp_id == HWC_DISPLAY_VIRTUAL) {
    return;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.notify_one();
}

bool HWCSession::CWB::IsCwbActiveOnDisplay(hwc2_display_t disp_type) {
  SCOPE_LOCK(queue_lock_);
  return (queue_.size() && queue_.front()->display_type == disp_type) ? true : false;
}

int HWCSession::DisplayConfigImpl::SetQsyncMode(uint32_t disp_id, DisplayConfig::QsyncMode mode) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_id]) {
    return -1;
  }

  QSyncMode qsync_mode = kQSyncModeNone;
  switch (mode) {
    case DisplayConfig::QsyncMode::kNone:
      qsync_mode = kQSyncModeNone;
      break;
    case DisplayConfig::QsyncMode::kWaitForFencesOneFrame:
      qsync_mode = kQsyncModeOneShot;
      break;
    case DisplayConfig::QsyncMode::kWaitForFencesEachFrame:
      qsync_mode = kQsyncModeOneShotContinuous;
      break;
    case DisplayConfig::QsyncMode::kWaitForCommitEachFrame:
      qsync_mode = kQSyncModeContinuous;
      break;
  }

  hwc_session_->hwc_display_[disp_id]->SetQSyncMode(qsync_mode);
  hwc_session_->hwc_display_qsync_[disp_id]  = qsync_mode;
  return 0;
}

int HWCSession::DisplayConfigImpl::IsSmartPanelConfig(uint32_t disp_id, uint32_t config_id,
                                                      bool *is_smart) {
  if (disp_id < 0 || disp_id >= HWCCallbacks::kNumDisplays) {
    DLOGE("Not valid display");
    return -EINVAL;
  }
  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_id]) {
    DLOGE("Display %d is not created yet.", disp_id);
    *is_smart = false;
    return -EINVAL;
  }

  if (hwc_session_->hwc_display_[disp_id]->GetDisplayClass() != DISPLAY_CLASS_BUILTIN) {
    return false;
  }

  *is_smart = hwc_session_->hwc_display_[disp_id]->IsSmartPanelConfig(config_id);
  return 0;
}

int HWCSession::DisplayConfigImpl::IsAsyncVDSCreationSupported(bool *supported) {
  if(hwc_session_->disable_non_wfd_vds_ && hwc_session_->debug_enable_hwc_vds_) {
    *supported = false;
    return 0;
  }

  if (!hwc_session_->async_vds_creation_) {
    *supported = false;
    return 0;
  }

  *supported = true;
  return 0;
}

int HWCSession::DisplayConfigImpl::CreateVirtualDisplay(uint32_t width, uint32_t height,
                                                        int32_t format) {
  if (!hwc_session_->async_vds_creation_) {
    return HWC2_ERROR_UNSUPPORTED;
  }

  hwc_session_->async_vds_creation_requested_ = true;

  if (!width || !height) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  hwc2_display_t active_builtin_disp_id = hwc_session_->GetActiveBuiltinDisplay();
  auto status = hwc_session_->CreateVirtualDisplayObj(width, height, &format,
                                                      &hwc_session_->virtual_id_);
  if (status == HWC2::Error::None) {
    DLOGI("[async] Created virtual display id:%" PRIu64 ", res: %dx%d",
          hwc_session_->virtual_id_, width, height);
    if (active_builtin_disp_id < HWCCallbacks::kNumRealDisplays) {
      hwc_session_->WaitForResources(true, active_builtin_disp_id, hwc_session_->virtual_id_);
    }
  } else {
    DLOGE("Failed to create virtual display: %s", to_string(status).c_str());
  }

  return INT(status);
}

int HWCSession::DisplayConfigImpl::IsRotatorSupportedFormat(int hal_format, bool ubwc,
                                                             bool *supported) {
  if (!hwc_session_->core_intf_) {
    DLOGW("core_intf_ not initialized.");
    *supported = false;
    return -EINVAL;
  }
  int flag = ubwc ? qtigralloc::PRIV_FLAGS_UBWC_ALIGNED : 0;

  LayerBufferFormat sdm_format = HWCLayer::GetSDMFormat(hal_format, flag);

  *supported = hwc_session_->core_intf_->IsRotatorSupportedFormat(sdm_format);
  return 0;
}

int HWCSession::DisplayConfigImpl::ControlQsyncCallback(bool enable) {
  if (enable) {
    hwc_session_->qsync_callback_ = callback_;
  } else {
    hwc_session_->qsync_callback_.reset();
  }

  return 0;
}

int HWCSession::DisplayConfigImpl::GetDisplayHwId(uint32_t disp_id, uint32_t *display_hw_id) {
  int disp_idx = hwc_session_->GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SCOPE_LOCK(hwc_session_->locker_[disp_id]);
  if (!hwc_session_->hwc_display_[disp_idx]) {
    DLOGW("Display %d is not connected.", disp_id);
    return -EINVAL;
  }

  int error = -EINVAL;
  // Supported for Built-In displays only.
  if ((hwc_session_->map_info_primary_.client_id == disp_id) &&
      (hwc_session_->map_info_primary_.disp_type == kBuiltIn)) {
    if (hwc_session_->map_info_primary_.sdm_id >= 0) {
      *display_hw_id = static_cast<uint32_t>(hwc_session_->map_info_primary_.sdm_id);
      error = 0;
    }
    return error;
  }

  for (auto &info : hwc_session_->map_info_builtin_) {
    if (disp_id == info.client_id) {
      if (info.sdm_id >= 0) {
        *display_hw_id = static_cast<uint32_t>(info.sdm_id);
        error = 0;
      }
      return error;
    }
  }

  return error;
}

int HWCSession::DisplayConfigImpl::SendTUIEvent(DispType dpy,
                                                DisplayConfig::TUIEventType event_type) {
  int disp_id = MapDisplayType(dpy);
  switch(event_type) {
    case DisplayConfig::TUIEventType::kPrepareTUITransition:
      return hwc_session_->TUITransitionPrepare(disp_id);
    case DisplayConfig::TUIEventType::kStartTUITransition:
      return hwc_session_->TUITransitionStart(disp_id);
    case DisplayConfig::TUIEventType::kEndTUITransition:
      return hwc_session_->TUITransitionEnd(disp_id);
    default:
      DLOGE("Invalid event %d", event_type);
      return -EINVAL;
  }
}

int HWCSession::GetSupportedDisplayRefreshRates(int disp_id,
                                                std::vector<uint32_t> *supported_refresh_rates) {
  int disp_idx = GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SCOPE_LOCK(locker_[disp_idx]);

  if (hwc_display_[disp_idx]) {
    return hwc_display_[disp_idx]->GetSupportedDisplayRefreshRates(supported_refresh_rates);
  }
  return -EINVAL;
}

int HWCSession::DisplayConfigImpl::GetSupportedDisplayRefreshRates(
    DispType dpy, std::vector<uint32_t> *supported_refresh_rates) {
  return hwc_session_->GetSupportedDisplayRefreshRates(MapDisplayType(dpy),
                                                       supported_refresh_rates);
}

int HWCSession::DisplayConfigImpl::IsRCSupported(uint32_t disp_id, bool *supported) {
  // Mask layers can potentially be shown on any display so report RC supported on all displays if
  // the property enables the feature for use.
  int val = false;  // Default value.
  Debug::GetProperty(ENABLE_ROUNDED_CORNER, &val);
  *supported = val ? true: false;

  return 0;
}

int HWCSession::DisplayConfigImpl::IsSupportedConfigSwitch(uint32_t disp_id, uint32_t config,
                                                         bool *supported) {
  int disp_idx = hwc_session_->GetDisplayIndex(disp_id);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_id);
    return -EINVAL;
  }

  SCOPE_LOCK(hwc_session_->locker_[disp_idx]);
  if (!hwc_session_->hwc_display_[disp_idx]) {
    DLOGW("Display %d is not connected.", disp_id);
    return -EINVAL;
  }

  *supported = hwc_session_->hwc_display_[disp_idx]->IsModeSwitchAllowed(config);

  return 0;
}

int HWCSession::DisplayConfigImpl::ControlIdleStatusCallback(bool enable) {
  if (enable) {
    hwc_session_->idle_callback_ = callback_;
  } else {
    hwc_session_->idle_callback_.reset();
  }

  return 0;
}

int HWCSession::DisplayConfigImpl::GetDisplayType(uint64_t physical_disp_id, DispType *disp_type) {
  if (!disp_type) {
    return -EINVAL;
  }
  return hwc_session_->GetDispTypeFromPhysicalId(physical_disp_id, disp_type);
}

int HWCSession::DisplayConfigImpl::AllowIdleFallback() {
  SEQUENCE_WAIT_SCOPE_LOCK(hwc_session_->locker_[HWC_DISPLAY_PRIMARY]);

  uint32_t active_ms = 0;
  uint32_t inactive_ms = 0;
  Debug::GetIdleTimeoutMs(&active_ms, &inactive_ms);
  if (hwc_session_->hwc_display_[HWC_DISPLAY_PRIMARY]) {
    DLOGI("enable idle time active_ms:%d inactive_ms:%d",active_ms,inactive_ms);
    hwc_session_->hwc_display_[HWC_DISPLAY_PRIMARY]->SetIdleTimeoutMs(active_ms, inactive_ms);
    hwc_session_->is_client_up_ = true;
    hwc_session_->hwc_display_[HWC_DISPLAY_PRIMARY]->MarkClientActive(true);
    hwc_session_->idle_time_inactive_ms_ = inactive_ms;
    hwc_session_->idle_time_active_ms_ = active_ms;
    return 0;
  }

  DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
  return -ENODEV;
}
}  // namespace sdm
