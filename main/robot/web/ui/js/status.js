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

function domainInterval(name) {
  if (["core", "motors", "sensors"].includes(name)) return robotActive ? 300 : 900;
  if (["battery", "camera", "audio"].includes(name)) return 1000;
  if (name === "environment") return 1500;
  if (name === "system") return 4000;
  if (name === "chat") return ["Sending", "Waiting", "Speaking"].includes(chatBackendState)
    ? 400 : 1200;
  return null;
}

function renderCoreStatus(status) {
  Object.assign(statusCache, status);
  const idle = status.state === "idle";
  const chatActive = ["connecting", "listening", "speaking"].includes(status.state);
  lastState = status.state || "unknown";
  robotActive = !idle || motorsActive;
  if (!idle && browserLive) stopBrowserLive("Paused until Idle", true);

  const visibleState = status.state === "listening" && !status.asr_ready
    ? status.asr_preparing ? "preparing ASR" : "processing"
    : (status.state || "idle").replaceAll("_", " ");
  $("#state").textContent = visibleState;
  $("#wakeAction").classList.toggle("on", chatActive);
  $("#wakeLabel").textContent = chatActive ? "End chat" : "Wake";
  if (document.activeElement !== $("#emotionMovement")) {
    $("#emotionMovement").checked = !!status.emotion_movement_enabled;
  }
  $$('[data-emotion]').forEach((button) => {
    button.classList.toggle("on", button.dataset.emotion === (status.emotion || "neutral"));
  });
  renderCameraStatus(statusCache);
}

function updateDriveAvailability() {
  $$('[data-turn]').forEach((button) => {
    button.disabled = cliffDetected || !!statusCache.gyro_turn_pending || motorsActive;
  });
  $$('[data-drive]').forEach((button) => {
    button.disabled = cliffDetected && button.dataset.drive !== "backward";
  });
  $("#safety").classList.toggle("show", cliffDetected);
}

function renderMotorStatus(status) {
  Object.assign(statusCache, status);
  const motors = status.motors || {};
  motorsActive = !!(motors.moving || motors.queued || motors.sequence_active);
  robotActive = lastState !== "idle" || motorsActive;
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
  $("#power").textContent = ready
    ? (currentMa >= 0 ? "+" : "") + Math.round(currentMa) + " mA · " +
      (Number(status.battery_power_mw) / 1000).toFixed(2) + " W"
    : status.battery_available
      ? "Invalid · CNVR " + (status.battery_conversion_ready ? "yes" : "no") +
        " · OVF " + (status.battery_math_overflow ? "yes" : "no")
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
  setRange($("#screenBrightness"), status.screen_brightness, $("#screenValue"), "%");
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
  $("#lightsAction").classList.toggle("on", (status.status_light_brightness || 0) > 0);
  $("#displayFlip").classList.toggle("on", !!status.display_flipped);
  $("#oledFlip").classList.toggle("on", !!status.oled_flipped);
  renderOledConfig(status);
}

function renderCameraStatus(status) {
  Object.assign(statusCache, status);
  const idle = lastState === "idle";
  $("#camera").textContent = !status.camera_available
    ? "Offline"
    : status.live_camera
      ? idle ? "Screen preview" : "Preview paused"
      : status.camera_flipped ? "Flipped" : "Normal";
  $("#liveCamera").classList.toggle("on", !!status.live_camera);
  $("#liveCamera").textContent = status.live_camera
    ? idle ? "Stop screen preview" : "Preview paused"
    : "Screen preview";
  $("#cameraFlip").classList.toggle("on", !!status.camera_flipped);
  $("#cameraHealth").textContent = status.camera_available ? "Ready" : "Offline";
}

function renderAudioStatus(status) {
  setRange($("#speakerVolume"), status.speaker_volume, $("#speakerValue"), "%");
  setRange($("#microphoneGain"), status.microphone_gain, $("#microphoneValue"), "×");
  $("#micLevel").style.width = Math.max(0, Math.min(100, status.microphone_level || 0)) + "%";
  $("#micClip").textContent = status.microphone_clipping ? "CLIP" : "LIVE";
  $("#micClip").classList.toggle("on", !!status.microphone_clipping);
}

function renderSystemStatus(status) {
  $("#uptime").textContent = fmtTime(status.uptime_sec);
  $("#version").textContent = "v" + (status.version || "—");
  $("#ssid").textContent = status.ssid || "—";
  $("#rssi").textContent = status.rssi ? status.rssi + " dBm" : "—";
  $("#ip").textContent = status.ip || "—";
  $("#sram").textContent = fmtBytes(status.free_internal_bytes);
  $("#psram").textContent = fmtBytes(status.free_psram_bytes);
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
  $("#aht20Status").textContent = [environmentStateLabel(aht20.state), temperature, humidity]
    .filter(Boolean).join(" · ");
  $("#bmp280Status").textContent = [environmentStateLabel(bmp280.state), pressure]
    .filter(Boolean).join(" · ");
  $("#bh1750Status").textContent = [environmentStateLabel(bh1750.state), illuminance]
    .filter(Boolean).join(" · ");
}

function scheduleStatusLoop(delay = 0) {
  clearTimeout(statusTimer);
  statusTimer = setTimeout(runStatusScheduler, Math.max(0, delay));
}

function queueDomains(names, delay = 0) {
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
}

function markCoreFailure() {
  coreFailures++;
  if (coreFailures < 3) return;
  $("#online").classList.remove("ok");
  $("#online span").textContent = "Offline";
}

async function runStatusScheduler() {
  if (statusPending) return;
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
    const response = await fetch(config.path, { cache: "no-store" });
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

function startStatusPolling() {
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
  Object.entries(initialOffsets).forEach(([name, offset]) => domainSchedule.set(name, now + offset));
  scheduleStatusLoop(0);
}
