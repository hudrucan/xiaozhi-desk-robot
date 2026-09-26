const statusCache = {};
const domainSchedule = new Map();
let statusTimer = null;
let statusPending = false;
let coreFailures = 0;
let robotActive = false;
let motorsActive = false;
let cliffDetected = false;

const domainConfig = {
  core: { path: "/api/status/core", render: renderCoreStatus },
  motors: { path: "/api/status/motors", render: renderMotorStatus },
  sensors: { path: "/api/status/sensors", render: renderSensorStatus },
  battery: { path: "/api/status/battery", render: renderBatteryStatus },
  display: { path: "/api/status/display", render: renderDisplayStatus },
  camera: { path: "/api/status/camera", render: renderCameraStatus },
  audio: { path: "/api/status/audio", render: renderAudioStatus },
  system: { path: "/api/status/system", render: renderSystemStatus },
  environment: { path: "/api/status/environment", render: renderEnvironmentStatus },
  chat: { path: "/api/chat", render: renderConversation },
  asr: { path: "/api/asr", render: applyAsrStatus },
};

const statusDomainsByTab = {
  overview: ["core", "system", "camera", "sensors", "battery", "environment"],
  control: ["core", "system", "motors", "sensors", "battery", "display", "camera"],
  chat: ["core", "system", "chat", "asr"],
  camera: ["core", "camera"],
  device: ["core", "system", "audio", "display", "environment"],
  diagnostics: ["core", "system"],
};
let activeStatusDomains = new Set();

function domainInterval(name) {
  if (document.hidden || !activeStatusDomains.has(name)) return null;
  if (name === "core") return robotActive ? 400 : 1000;
  if (name === "motors") return motorsActive ? 250 : 1000;
  if (name === "sensors") {
    return motorsActive ? 300 : activeTab === "control" ? 1000
      : activeTab === "overview" ? 5000 : 2000;
  }
  if (name === "battery") return activeTab === "control" ? 3000
    : activeTab === "overview" ? 10000 : 2000;
  if (name === "camera") return activeTab === "camera" ? 1000 : 2500;
  if (name === "audio") return 1000;
  if (name === "display") {
    return statusCache.auto_brightness_enabled || statusCache.oled_auto_contrast_enabled
      ? 1000
      : null;
  }
  if (name === "environment") return activeTab === "overview" ? 10000
    : activeTab === "device" ? 1000 : 3000;
  if (name === "system") {
    return activeTab === "diagnostics" ? 2000
      : ["overview", "camera"].includes(activeTab) ? 5000 : 30000;
  }
  if (name === "chat") return ["Sending", "Waiting", "Speaking"].includes(chatBackendState)
    ? 500 : 2000;
  return null;
}

function renderCoreStatus(status) {
  Object.assign(statusCache, status);
  const idle = status.state === "idle";
  const chatActive = ["connecting", "listening", "speaking"].includes(status.state);
  lastState = status.state || "unknown";
  robotActive = !idle || motorsActive || !!status.reaction_active;
  if (!idle && browserLive) stopBrowserLive("Live preview stopped outside Idle", true);

  const visibleState = status.server_status_phase
    ? String(status.server_status_phase).replaceAll("_", " ")
    : status.state === "listening" && !status.asr_ready
      ? status.asr_preparing ? "preparing ASR" : "processing"
      : (status.state || "idle").replaceAll("_", " ");
  $("#state").textContent = visibleState;
  $("#overviewAssistant").textContent = visibleState;
  $("#sidebarState").textContent = visibleState;
  $("#wakeAction").classList.toggle("on", chatActive);
  $("#wakeLabel").textContent = chatActive ? "End chat" : "Wake";
  if (document.activeElement !== $("#emotionMovement")) {
    $("#emotionMovement").checked = !!status.emotion_movement_enabled;
  }
  $all('[data-emotion]').forEach((button) => {
    button.classList.toggle("on", button.dataset.emotion === (status.emotion || "neutral"));
  });
  const reactionName = status.reaction || "none";
  $("#reactionState").textContent = status.reaction_active
    ? reactionName + " · " + fmtMs(status.reaction_remaining_ms || 0)
    : "Idle";
  $all('[data-reaction]').forEach((button) => {
    button.classList.toggle(
      "on",
      !!status.reaction_active && button.dataset.reaction === reactionName,
    );
  });
  $("#reactionFace").textContent = status.emotion || "neutral";
  $("#reactionLight").textContent = status.reaction_active ? "active" : "idle";
  $("#reactionMotion").textContent = String(status.reaction_motion_state || "not_requested")
    .replaceAll("_", " ");
  $("#reactionCancel").hidden = !status.reaction_active;
  $("#reactionCancel").disabled = !status.reaction_active;
  renderCameraStatus(statusCache);
}

function updateDriveAvailability() {
  $all('[data-turn]').forEach((button) => {
    button.disabled = cliffDetected || !!statusCache.gyro_turn_pending || motorsActive;
  });
  $all('[data-drive]').forEach((button) => {
    button.disabled = cliffDetected && button.dataset.drive !== "backward";
  });
  $("#safety").classList.toggle("show", cliffDetected);
}

function renderMotorStatus(status) {
  Object.assign(statusCache, status);
  const motors = status.motors || {};
  motorsActive = !!(motors.moving || motors.queued || motors.sequence_active || motors.live_drive);
  robotActive = lastState !== "idle" || motorsActive || !!statusCache.reaction_active;
  if (document.activeElement !== $("#duration") && Number.isFinite(status.drive_duration_ms)) {
    $("#duration").value = status.drive_duration_ms;
    $("#durationLabel").textContent = fmtMs(status.drive_duration_ms);
  }
  setRange($("#motorSpeed"), status.motor_speed, $("#motorSpeedValue"), "%");
  $("#motion").textContent = motors.faulted
    ? "Fault"
    : motorsActive
      ? (motors.direction || "moving") + (motors.sequence_active ? " · dance" : "")
      : "Stopped";
  if (!liveDriveActive && motors.live_drive) {
    renderJoystick(motors.left_percent || 0, motors.right_percent || 0);
  }
  $("#queue").textContent = motors.queued || 0;
  $("#motorTime").textContent = fmtMs(motors.remaining_ms || 0);
  $("#panicStop").classList.toggle("show", motorsActive);
  const total = motors.sequence_total || 0;
  const completed = motors.sequence_completed || 0;
  $("#danceProgress").style.width = (total ? Math.min(100, completed * 100 / total) : 0) + "%";
  $("#danceState").textContent = motors.sequence_active
    ? completed + "/" + total + " steps"
    : "Ready";
  $("#danceAction").classList.toggle("on", !!motors.sequence_active);
  updateDriveAvailability();
}

function renderSensorStatus(status) {
  Object.assign(statusCache, status);
  cliffDetected = !!status.cliff_detected;
  $("#distance").textContent = status.distance_valid ? status.distance_mm + " mm" : "—";
  $("#overviewDistance").textContent = status.distance_valid
    ? status.distance_mm + " mm" : "No floor return";
  $("#rangeState").textContent = cliffDetected
    ? "Edge · reverse only"
    : status.distance_valid ? "Floor detected" : "No floor return";
  const motionReady = !!status.motion_sensor_available && !!status.motion_sensor_valid;
  $("#motionSensor").textContent = !status.motion_sensor_available
    ? "Offline"
    : motionReady
      ? (status.motion_gesture || "steady") + " · " +
        Number(status.motion_roll_deg).toFixed(0) + "° / " +
        Number(status.motion_pitch_deg).toFixed(0) + "°"
      : "Calibrating";
  $("#motionEmotions").disabled = !status.motion_sensor_available;
  if (document.activeElement !== $("#motionEmotions")) {
    $("#motionEmotions").checked = !!status.motion_emotions_enabled;
  }
  setRange($("#cliffThreshold"), status.cliff_edge_mm, $("#cliffValue"), " mm");
  updateDriveAvailability();
}

function renderBatteryStatus(status) {
  const ready = !!status.battery_available && !!status.battery_valid;
  const signedCurrent = Number(status.battery_signed_current_ma);
  const currentMa = Number.isFinite(signedCurrent) ? signedCurrent : Number(status.battery_current_ma);
  const flow = status.battery_charging
    ? "Charging"
    : status.battery_discharging ? "Discharging" : "Near zero";
  $("#battery").textContent = !status.battery_available
    ? "Offline"
    : ready
      ? flow + " · " + status.battery_percent + "% · " +
        Number(status.battery_voltage_v).toFixed(2) + " V"
      : "Waiting";
  $("#overviewBattery").textContent = !status.battery_available
    ? "Offline"
    : ready ? flow + " · " + status.battery_percent + "%" : "Waiting";
  $("#power").textContent = ready
    ? (currentMa >= 0 ? "+" : "") + Math.round(currentMa) + " mA · " +
      (Number(status.battery_power_mw) / 1000).toFixed(2) + " W"
    : status.battery_available
      ? "Invalid · CNVR " + (status.battery_conversion_ready ? "yes" : "no") +
        " · OVF " + (status.battery_math_overflow ? "yes" : "no")
      : "—";
  $("#overviewPower").textContent = ready
    ? (currentMa >= 0 ? "+" : "") + Math.round(currentMa) + " mA · " +
      (Number(status.battery_power_mw) / 1000).toFixed(2) + " W"
    : "—";

  const capacityMah = Number(status.battery_capacity_test_mah) || 0;
  const capacityActive = !!status.battery_capacity_test_active;
  const capacityMeasuring = !!status.battery_capacity_test_measuring;
  $("#capacityResult").textContent = capacityMah.toFixed(1) + " mAh · " +
    fmtCapacityTime(status.battery_capacity_test_seconds || 0);
  $("#capacityState").textContent = !status.battery_available
    ? "INA219 unavailable"
    : capacityMeasuring
      ? "Measuring discharge"
      : capacityActive
        ? "Paused · unplug USB/charger"
        : capacityMah > 0 ? "Stopped · result saved" : "Capacity test not started";
  $("#capacityStart").disabled = !status.battery_available || capacityActive;
  $("#capacityStop").disabled = !capacityActive;
  $("#capacityReset").disabled = !status.battery_available || (!capacityActive && capacityMah === 0);
  $("#capacityStart").textContent = capacityMah > 0 ? "Resume" : "Start";
  $("#capacityStart").classList.toggle("on", capacityMeasuring);

  const remainingMah = Number(status.battery_remaining_mah) || 0;
  const totalMah = Number(status.battery_capacity_mah) || 0;
  const restState = status.battery_soc_quasi_resting ? " · quasi-rest correcting" : "";
  const anchorState = status.battery_soc_full_anchored
    ? " · full anchored"
    : status.battery_soc_empty_anchored ? " · empty anchored" : "";
  const recoveryState = status.battery_soc_bootstrap_voltage_rebased
    ? " · startup voltage estimate" : "";
  $("#socState").textContent = ready
    ? remainingMah.toFixed(0) + " mAh remaining · " + totalMah.toFixed(1) +
      " mAh capacity · Coulomb + quasi-rest + anchors · " + flow + restState +
      anchorState + recoveryState +
      (status.battery_soc_tracking_degraded ? " · tracking degraded" : "")
    : "SoC waiting for valid measurement";
}

function renderDisplayStatus(status) {
  Object.assign(statusCache, status);
  const oled = $("#oledState");
  oled.className = "value health " + (status.oled_available ? "good" : "bad");
  oled.querySelector("span").textContent = status.oled_available ? "Ready" : "Offline";
  const screenBrightness = Number(status.screen_brightness);
  if (Number.isFinite(screenBrightness)) {
    const screenInput = $("#screenBrightness");
    if (document.activeElement !== screenInput) {
      screenInput.value = screenBrightness;
    }
    if (status.auto_brightness_enabled || document.activeElement !== screenInput) {
      $("#screenValue").textContent = screenBrightness + "%";
    }
  }
  if (document.activeElement !== $("#autoBrightness")) {
    $("#autoBrightness").checked = !!status.auto_brightness_enabled;
  }
  setRange($("#autoBrightnessMinimum"), status.auto_brightness_minimum,
    $("#autoBrightnessMinimumValue"), "%");
  setRange($("#autoBrightnessMaximum"), status.auto_brightness_maximum,
    $("#autoBrightnessMaximumValue"), "%");
  $("#autoBrightnessMinimum").disabled = !status.auto_brightness_enabled;
  $("#autoBrightnessMaximum").disabled = !status.auto_brightness_enabled;
  setRange($("#statusLightBrightness"), status.status_light_brightness,
    $("#statusLightValue"), "%");
  if (Number.isFinite(status.oled_contrast)) {
    const contrast = Math.round(Math.max(0, Math.min(255, status.oled_contrast)) * 100 / 255);
    setRange($("#oledContrast"), contrast, $("#oledContrastValue"), "%");
  }
  if (document.activeElement !== $("#oledAutoContrast")) {
    $("#oledAutoContrast").checked = !!status.oled_auto_contrast_enabled;
  }
  if (Number.isFinite(status.oled_auto_contrast_minimum)) {
    const minimum = Math.round(
      Math.max(0, Math.min(255, status.oled_auto_contrast_minimum)) * 100 / 255,
    );
    setRange($("#oledAutoContrastMinimum"), minimum,
      $("#oledAutoContrastMinimumValue"), "%");
  }
  if (Number.isFinite(status.oled_auto_contrast_maximum)) {
    const maximum = Math.round(
      Math.max(0, Math.min(255, status.oled_auto_contrast_maximum)) * 100 / 255,
    );
    setRange($("#oledAutoContrastMaximum"), maximum,
      $("#oledAutoContrastMaximumValue"), "%");
  }
  if (Number.isFinite(status.oled_effective_contrast)) {
    const effective = Math.round(
      Math.max(0, Math.min(255, status.oled_effective_contrast)) * 100 / 255,
    );
    const source = status.oled_auto_contrast_enabled
      ? status.oled_auto_contrast_available ? " live" : " fallback"
      : " manual";
    $("#oledEffectiveContrast").textContent = effective + "%" + source;
  }
  $("#oledAutoContrastMinimum").disabled = !status.oled_auto_contrast_enabled;
  $("#oledAutoContrastMaximum").disabled = !status.oled_auto_contrast_enabled;
  $("#lightsAction").classList.toggle("on", (status.status_light_brightness || 0) > 0);
  $("#displayFlip").classList.toggle("on", !!status.display_flipped);
  $("#oledFlip").classList.toggle("on", !!status.oled_flipped);
  renderOledConfig(status);
}

function renderCameraStatus(status) {
  Object.assign(statusCache, status);
  const idle = lastState === "idle";
  const available = !!status.camera_available;
  const previewAvailable = available && idle;
  if (status.camera_sensor && status.camera_profile) {
    let profile = status.camera_profile.replace("_", " ");
    if (status.camera_profile === "auto") {
      profile = status.camera_auto_profile_available
        ? "auto → " + status.camera_effective_profile.replace("_", " ")
        : "auto unavailable → normal";
    }
    $("#cameraSettingsSummary").textContent = status.camera_sensor + " · " + profile +
      " · " + (status.camera_mode || "off");
  }
  if (browserLive && status.web_camera_live) {
    browserLiveConfirmed = true;
    $("#snapshotMeta").textContent = "MJPEG live";
  } else if (browserLive && browserLiveConfirmed && !status.web_camera_live) {
    stopBrowserLive("Live preview stopped", true);
  }
  $("#camera").textContent = !available
    ? "Offline"
    : status.camera_mode === "mcp"
      ? "MCP capture"
    : status.web_camera_live
      ? "Web live"
      : status.live_camera
      ? "Screen preview"
      : status.camera_flipped ? "Flipped" : "Normal";
  $("#liveCamera").classList.toggle("on", !!status.live_camera);
  $("#liveCamera").textContent = status.live_camera
    ? "Stop screen preview"
    : "Screen preview";
  $("#browserLive").classList.toggle("on", !!status.web_camera_live);
  $("#browserLive").textContent = status.web_camera_live ? "Stop live" : "Live";
  $("#takeSnapshot").disabled = !previewAvailable || !!status.web_camera_live;
  $("#browserLive").disabled = !previewAvailable && !browserLive;
  $("#liveCamera").disabled = !previewAvailable;
  $("#cameraFlip").classList.toggle("on", !!status.camera_flipped);
  $("#cameraHealth").textContent = available ? "Ready" : "Offline";
  const requestState = status.camera_request_state || "never";
  if (requestState === "never") {
    $("#cameraLastRequest").textContent = "Never";
  } else {
    const elapsedMs = (Number(status.camera_capture_ms) || 0) +
      (Number(status.camera_vision_ms) || 0);
    const ageSec = Number(status.camera_request_age_sec);
    const age = Number.isFinite(ageSec)
      ? (ageSec < 60 ? Math.max(0, Math.floor(ageSec)) + "s" : fmtTime(ageSec)) + " ago"
      : null;
    const details = [
      requestState.charAt(0).toUpperCase() + requestState.slice(1),
      elapsedMs ? fmtMs(elapsedMs) : null,
      status.camera_image_bytes ? fmtBytes(status.camera_image_bytes) + " image" : null,
      status.camera_response_bytes ? fmtBytes(status.camera_response_bytes) + " response" : null,
      age,
    ].filter(Boolean);
    $("#cameraLastRequest").textContent = details.join(" · ");
  }
}

function renderAudioStatus(status) {
  setRange($("#speakerVolume"), status.speaker_volume, $("#speakerValue"), "%");
  setRange($("#microphoneVoiceGain"), status.voice_gain_db,
    $("#microphoneVoiceGainValue"), " dB");
  setRange($("#microphoneCaptureTrim"), status.capture_trim_db,
    $("#microphoneCaptureTrimValue"), " dB");
  const profile = status.voice_profile || "legacy";
  setMicrophoneProfile(profile);
  if (document.activeElement !== $("#microphoneMute")) {
    $("#microphoneMute").checked = !!status.microphone_muted;
  }
  if (document.activeElement !== $("#microphoneNs")) {
    $("#microphoneNs").checked = !!status.ns_requested;
  }
  if (document.activeElement !== $("#microphoneAgc")) {
    $("#microphoneAgc").checked = !!status.agc_requested;
  }
  $("#microphoneNs").disabled = !status.ns_available;
  $("#microphoneAgc").disabled = !status.agc_available;
  $("#microphoneNsState").textContent = !status.ns_available
    ? "Unavailable" : !!status.ns_active !== !!status.ns_requested
      ? "Pending" : status.ns_active ? "Active" : "Off";
  $("#microphoneAgcState").textContent = !status.agc_available
    ? "Unavailable" : !!status.agc_active !== !!status.agc_requested
      ? "Pending" : status.agc_active ? "Active" : "Off";
  $("#micLevel").style.width = Math.max(0, Math.min(100, status.microphone_level || 0)) + "%";
  $("#micClip").textContent = status.microphone_muted
    ? "MUTED"
    : status.microphone_clipping ? "CLIP" : "LIVE";
  $("#micClip").classList.toggle("on", !!status.microphone_muted || !!status.microphone_clipping);
  const formatLevels = (rms, peak) => Number.isFinite(rms) && Number.isFinite(peak)
    ? "RMS " + Number(rms).toFixed(1) + " · Peak " + Number(peak).toFixed(1) + " dBFS"
    : "Unavailable";
  $("#microphoneVoiceLevel").textContent =
    formatLevels(status.voice_rms_dbfs, status.voice_peak_dbfs);
  $("#microphoneAfeLevel").textContent =
    formatLevels(status.afe_output_rms_dbfs, status.afe_output_peak_dbfs);
  $("#microphoneRestart").hidden = !status.voice_processing_restart_required;
}

function memoryResource(freeBytes, totalBytes) {
  const free = Number(freeBytes);
  const total = Number(totalBytes);
  if (!Number.isFinite(free) || !Number.isFinite(total) || total <= 0) {
    return { label: "—", compactLabel: "—", usedPercent: 0 };
  }
  const clampedFree = Math.max(0, Math.min(total, free));
  const useMegabytes = total >= 1048576;
  const divisor = useMegabytes ? 1048576 : 1024;
  const unit = useMegabytes ? " MB" : " KB";
  const precision = useMegabytes ? 1 : 0;
  return {
    label: fmtBytes(clampedFree) + " / " + fmtBytes(total),
    compactLabel: (clampedFree / divisor).toFixed(precision) + " / " +
      (total / divisor).toFixed(precision) + unit,
    usedPercent: Math.round((1 - clampedFree / total) * 100),
  };
}

function wifiResource(rssi) {
  const value = Number(rssi);
  if (!Number.isFinite(value) || value === 0) {
    return { percent: 0, quality: "Unavailable" };
  }
  const percent = Math.round(Math.max(0, Math.min(100, (value + 100) * 100 / 60)));
  const quality = value >= -55 ? "Excellent" : value >= -67 ? "Good" :
    value >= -75 ? "Fair" : "Weak";
  return { percent, quality };
}

function renderSystemStatus(status) {
  const internal = memoryResource(status.free_internal_bytes, status.total_internal_bytes);
  const psram = memoryResource(status.free_psram_bytes, status.total_psram_bytes);
  const wifi = wifiResource(status.rssi);
  $("#uptime").textContent = fmtTime(status.uptime_sec);
  $("#overviewUptime").textContent = fmtTime(status.uptime_sec);
  $("#version").textContent = "v" + (status.version || "—");
  $("#sidebarVersion").textContent = "v" + (status.version || "—");
  $("#ssid").textContent = status.ssid || "—";
  $("#rssi").textContent = status.rssi ? status.rssi + " dBm" : "—";
  $("#ip").textContent = status.ip || "—";
  const transport = status.server_transport === "websocket" ? "WebSocket" :
    status.server_transport === "mqtt" ? "MQTT" : "None";
  $("#serverConnection").textContent = transport + " · " +
    (status.server_connected ? "connected" : "disconnected");
  $("#resetReason").textContent = (status.reset_reason || "unknown").replaceAll("-", " ");
  $("#sram").textContent = internal.label + " · low " +
    fmtBytes(status.minimum_free_internal_bytes);
  $("#psram").textContent = psram.label;
  $("#sidebarUptime").textContent = fmtTime(status.uptime_sec);
  $("#sidebarIp").textContent = status.ip || "—";
  $("#sidebarSram").textContent = internal.compactLabel;
  $("#sidebarPsram").textContent = psram.compactLabel;
  $("#sidebarSramBar").style.width = internal.usedPercent + "%";
  $("#sidebarPsramBar").style.width = psram.usedPercent + "%";
  $("#overviewInternalValue").textContent = internal.label;
  $("#overviewInternalLow").textContent = "Low watermark " +
    fmtBytes(status.minimum_free_internal_bytes);
  $("#overviewPsramValue").textContent = psram.label;
  $("#overviewWifiValue").textContent = status.rssi ? status.rssi + " dBm" : "—";
  $("#overviewWifiQuality").textContent = (status.ssid || "No network") +
    " · " + wifi.quality;
  $("#overviewInternalPercent").textContent = internal.usedPercent + "% used";
  $("#overviewPsramPercent").textContent = psram.usedPercent + "% used";
  $("#overviewWifiPercent").textContent = wifi.percent + "%";
  $("#overviewInternalBar").style.width = internal.usedPercent + "%";
  $("#overviewPsramBar").style.width = psram.usedPercent + "%";
  $("#overviewWifiBar").style.width = wifi.percent + "%";
}

function environmentStateLabel(state) {
  if (state === "online") return "Online";
  if (state === "degraded") return "Degraded";
  return "Unavailable";
}

function renderEnvironmentStatus(status) {
  const aht20 = status.aht20 || {};
  const bmp280 = status.bmp280 || {};
  const bh1750 = status.bh1750 || {};
  const acoustic = status.acoustic || {};
  const temperature = aht20.temperature_valid
    ? Number(aht20.temperature_c).toFixed(1) + "°C" : null;
  const humidity = aht20.humidity_valid
    ? Number(aht20.humidity_percent).toFixed(0) + "% RH" : null;
  const pressure = bmp280.pressure_valid
    ? Number(bmp280.pressure_hpa).toFixed(1) + " hPa" : null;
  const illuminance = bh1750.illuminance_valid
    ? Number(bh1750.illuminance_lux).toFixed(0) + " lx" : null;

  const summary = [temperature, humidity, pressure, illuminance].filter(Boolean);
  $("#environmentSummary").textContent = summary.length ? summary.join(" · ") : "No data";
  $("#overviewClimate").textContent = [temperature, humidity].filter(Boolean).join(" · ") ||
    "Unavailable";
  $("#overviewPressure").textContent = pressure || "Unavailable";
  $("#overviewLight").textContent = illuminance || "Unavailable";
  $("#aht20Status").textContent = [environmentStateLabel(aht20.state), temperature, humidity]
    .filter(Boolean).join(" · ");
  $("#bmp280Status").textContent = [environmentStateLabel(bmp280.state), pressure]
    .filter(Boolean).join(" · ");
  $("#bh1750Status").textContent = [environmentStateLabel(bh1750.state), illuminance]
    .filter(Boolean).join(" · ");
  const acousticFloor = $("#acousticFloorStatus");
  $("#microphoneRawLevel").textContent = acoustic.valid
    ? "RMS " + Number(acoustic.rms_dbfs).toFixed(1) + " · Peak " +
      Number(acoustic.peak_dbfs).toFixed(1) + " dBFS"
    : "Unavailable";
  if (!acoustic.valid) {
    $("#acousticStatus").textContent = "Unavailable";
    acousticFloor.hidden = true;
  } else {
    const rms = "RMS " + Number(acoustic.rms_dbfs).toFixed(1) + " dBFS";
    const peak = "Peak " + Number(acoustic.peak_dbfs).toFixed(1) + " dBFS";
    $("#acousticStatus").textContent = acoustic.self_noise
      ? rms + " · SELF-NOISE" : rms + " · " + peak;
    acousticFloor.textContent = acoustic.noise_floor_valid
      ? "Floor " + Number(acoustic.noise_floor_dbfs).toFixed(1) + " dBFS · " +
        (acoustic.self_noise
          ? "paused"
          : (Number(acoustic.signal_over_floor_db) >= 0 ? "+" : "") +
            Number(acoustic.signal_over_floor_db).toFixed(1) + " dB")
      : "Floor learning" + (acoustic.self_noise ? " · paused" : "");
    acousticFloor.hidden = false;
  }
}

function scheduleStatusLoop(delay = 0) {
  clearTimeout(statusTimer);
  if (document.hidden) return;
  statusTimer = setTimeout(runStatusScheduler, Math.max(0, delay));
}

function queueDomains(names, delay = 0) {
  if (document.hidden) return;
  const due = performance.now() + delay;
  names.forEach((name) => {
    if (domainConfig[name]) {
      domainSchedule.set(name, Math.min(domainSchedule.get(name) ?? Infinity, due));
    }
  });
  scheduleStatusLoop(0);
}

function markCoreOnline() {
  coreFailures = 0;
  $("#online").classList.add("ok");
  $("#online span").textContent = "Online";
  $(".sidebar-status").classList.add("is-online");
}

function markCoreFailure() {
  coreFailures++;
  if (coreFailures < 3) return;
  $("#online").classList.remove("ok");
  $("#online span").textContent = "Offline";
  $(".sidebar-status").classList.remove("is-online");
}

async function runStatusScheduler() {
  if (document.hidden || statusPending) return;
  const now = performance.now();
  const next = [...domainSchedule.entries()].sort((left, right) => left[1] - right[1])[0];
  if (!next) return;
  if (next[1] > now) {
    scheduleStatusLoop(next[1] - now);
    return;
  }

  const [name] = next;
  const config = domainConfig[name];
  domainSchedule.delete(name);
  statusPending = true;
  try {
    const response = await fetchWithTimeout(config.path, { cache: "no-store" });
    if (!response.ok) throw Error("Status request failed");
    const status = await response.json();
    config.render(status);
    if (name === "core") markCoreOnline();
  } catch (_) {
    if (name === "core") markCoreFailure();
  } finally {
    statusPending = false;
    const interval = domainInterval(name);
    if (interval !== null) domainSchedule.set(name, performance.now() + interval);
    scheduleStatusLoop(10);
  }
}

function pauseStatusPolling() {
  clearTimeout(statusTimer);
  statusTimer = null;
  domainSchedule.clear();
}

function setStatusTab(tab) {
  pauseStatusPolling();
  activeStatusDomains = new Set(statusDomainsByTab[tab] || statusDomainsByTab.overview);
  if (document.hidden) return;
  const now = performance.now();
  const initialOffsets = {
    core: 0,
    motors: 40,
    sensors: 80,
    battery: 150,
    camera: 220,
    audio: 300,
    system: 400,
    environment: 450,
    display: 500,
    chat: 600,
    asr: 700,
  };
  Object.entries(initialOffsets).forEach(([name, offset]) => {
    if (activeStatusDomains.has(name)) domainSchedule.set(name, now + offset);
  });
  scheduleStatusLoop(0);
}
