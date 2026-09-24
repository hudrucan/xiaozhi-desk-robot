function bindChatAndAsrControls() {
  $("#chatInput").oninput = updateChatInput;
  $("#chatInput").onkeydown = (event) => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      submitChat();
    }
  };
  $("#chatSend").onclick = submitChat;
  $("#chatCamera").onclick = submitCameraChat;
  $("#chatClear").onclick = clearConversation;
  $("#asrProvider").onchange = (event) => setAsrProvider(event.target.value);
  $("#geminiApiKey").oninput = () => { asrEditing = true; };
  $("#saveAsr").onclick = saveGeminiApiKey;
  $("#clearGeminiKey").onclick = clearGeminiApiKey;
  updateChatInput();
}

function bindRangeControls() {
  const duration = $("#duration");
  duration.oninput = () => { $("#durationLabel").textContent = fmtMs(+duration.value); };
  duration.onchange = () => action("drive_duration", { value: +duration.value });
  const motorSpeed = $("#motorSpeed");
  motorSpeed.oninput = () => { $("#motorSpeedValue").textContent = motorSpeed.value + "%"; };
  motorSpeed.onchange = () => action("motor_speed", { value: +motorSpeed.value });
  const oledContrast = $("#oledContrast");
  oledContrast.oninput = () => { $("#oledContrastValue").textContent = oledContrast.value + "%"; };
  oledContrast.onchange = () => action("oled_contrast", {
    value: Math.round(+oledContrast.value * 255 / 100),
  });
  [
    ["oledAutoContrastMinimum", "oledAutoContrastMinimumValue",
      "oled_auto_contrast_minimum"],
    ["oledAutoContrastMaximum", "oledAutoContrastMaximumValue",
      "oled_auto_contrast_maximum"],
  ].forEach(([inputId, labelId, actionName]) => {
    const input = $("#" + inputId);
    const label = $("#" + labelId);
    input.oninput = () => { label.textContent = input.value + "%"; };
    input.onchange = () => action(actionName, {
      value: Math.round(+input.value * 255 / 100),
    });
  });

  [
    ["speakerVolume", "speakerValue", "speaker_volume", "%"],
    ["microphoneGain", "microphoneValue", "microphone_gain", "×"],
    ["screenBrightness", "screenValue", "screen_brightness", "%"],
    ["autoBrightnessMinimum", "autoBrightnessMinimumValue", "auto_brightness_minimum", "%"],
    ["autoBrightnessMaximum", "autoBrightnessMaximumValue", "auto_brightness_maximum", "%"],
    ["statusLightBrightness", "statusLightValue", "status_light_brightness", "%"],
    ["cliffThreshold", "cliffValue", "cliff_threshold", " mm"],
  ].forEach(([inputId, labelId, actionName, suffix]) => {
    const input = $("#" + inputId);
    const label = $("#" + labelId);
    input.oninput = () => { label.textContent = input.value + suffix; };
    input.onchange = () => action(actionName, { value: +input.value });
  });
}

function emergencyStop() {
  stopLiveDrive(true);
  $all('[data-drive].active').forEach((button) => button.classList.remove("active"));
  fetch("/api/action", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: '{"action":"stop"}',
    keepalive: true,
  }).catch(() => {});
  queueDomains(["motors"], 120);
}

function bindRobotControls() {
  bindLiveDriveControls();
  const duration = $("#duration");
  $("#motionEmotions").onchange = (event) =>
    action("motion_emotions", { value: event.target.checked ? 1 : 0 });
  $("#emotionMovement").onchange = (event) =>
    action("emotion_movement", { value: event.target.checked ? 1 : 0 });
  $("#autoBrightness").onchange = (event) =>
    action("auto_brightness", { value: event.target.checked ? 1 : 0 });
  $("#microphoneMute").onchange = (event) =>
    action("microphone_mute", { value: event.target.checked ? 1 : 0 });
  $("#oledAutoContrast").onchange = (event) =>
    action("oled_auto_contrast", { value: event.target.checked ? 1 : 0 });
  $("#capacityStart").onclick = () => action("battery_capacity_start");
  $("#capacityStop").onclick = () => action("battery_capacity_stop");
  $("#capacityReset").onclick = () => {
    if (confirm("Reset the saved battery capacity measurement?")) action("battery_capacity_reset");
  };
  $all('[data-drive]').forEach((button) => {
    button.onclick = () => {
      if (!button.disabled) action(button.dataset.drive, { duration_ms: +duration.value });
    };
    button.oncontextmenu = (event) => event.preventDefault();
  });
  $all('[data-turn]').forEach((button) => {
    button.onclick = () => {
      if (!button.disabled) action("turn_relative", { value: +button.dataset.turn });
    };
    button.oncontextmenu = (event) => event.preventDefault();
  });
  $all('[data-stop]').forEach((button) => { button.onclick = () => action("stop"); });
  $all('[data-action]').forEach((button) => {
    button.onclick = () => {
      const name = button.dataset.action;
      if (name === "wifi_config" && !confirm("Open Wi-Fi setup mode? This page will disconnect.")) return;
      if (name === "reboot" && !confirm(
        "Reboot the robot now? Motors will stop and this page will disconnect briefly.",
      )) return;
      if (name === "live_camera" && browserLive) {
        stopBrowserLive("Switching to screen preview", true);
      }
      action(name);
    };
  });
  $all('[data-emotion]').forEach((button) => {
    button.onclick = () => action("emotion", { text: button.dataset.emotion, duration_ms: 5000 });
  });
  window.addEventListener("blur", emergencyStop);
  window.addEventListener("pagehide", emergencyStop);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) emergencyStop();
  });
}

function bindCameraAndLogControls() {
  $("#takeSnapshot").onclick = captureSnapshot;
  $("#browserLive").onclick = toggleBrowserLive;
  $("#cameraProfile").onchange = setCameraAdvancedState;
  cameraAdvancedIds.forEach((id) => {
    $("#" + id).onchange = () => {
      $("#cameraProfile").value = "custom";
      setCameraAdvancedState();
    };
  });
  $("#cameraAutoExposure").onchange = () => {
    $("#cameraProfile").value = "custom";
    setCameraAdvancedState();
  };
  $("#cameraAutoGain").onchange = () => {
    $("#cameraProfile").value = "custom";
    setCameraAdvancedState();
  };
  $("#cameraSettingsApply").onclick = () => saveCameraSettings(false);
  $("#cameraSettingsReset").onclick = () => {
    if (confirm("Reset all camera settings to safe defaults?")) saveCameraSettings(true);
  };
  pauseLog.onclick = toggleLogPause;
  $("#clearLog").onclick = clearLogs;
  $("#downloadLog").onclick = downloadLogs;
  $("#logSearch").oninput = () => renderLogs(true);
  $("#logFilter").onchange = () => renderLogs(true);
}

bindChatAndAsrControls();
bindServerConfigControls();
bindRangeControls();
bindRobotControls();
bindCameraAndLogControls();
bindOledPreviewToggle();
restoreLogs();
bindTabs();
