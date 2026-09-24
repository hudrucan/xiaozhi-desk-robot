const CHAT_MAX_CHARS = 512;
const $ = (selector) => document.querySelector(selector);
const $all = (selector) => document.querySelectorAll(selector);
const toast = $("#toast");

let toastTimer;
let logCursor = 0;
let logStarted = false;
let logPaused = false;
let logPending = false;
let logRenderPending = false;
let allLogs = "";
let snapshotUrl = "";
let browserLive = false;
let browserLiveConfirmed = false;
let snapshotPending = false;
let lastState = "starting";
let oledWidgetSignature = "";
let oledPreviewSignature = "";
let conversationSignature = "";
let chatSubmitting = false;
let chatBackendState = "Ready";
let asrEditing = false;
let asrSaving = false;

async function fetchWithTimeout(url, options = {}, timeoutMs = 5000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

function notify(text) {
  toast.textContent = text;
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 1800);
}

function fmtTime(sec) {
  sec = Math.max(0, sec | 0);
  const days = Math.floor(sec / 86400);
  const hours = Math.floor((sec % 86400) / 3600);
  const minutes = Math.floor((sec % 3600) / 60);
  return days
    ? days + "d " + hours + "h"
    : hours
      ? hours + "h " + minutes + "m"
      : minutes + "m";
}

function fmtCapacityTime(sec) {
  sec = Math.max(0, sec | 0);
  const hours = Math.floor(sec / 3600);
  const minutes = Math.floor((sec % 3600) / 60);
  const seconds = sec % 60;
  return hours
    ? hours + "h " + minutes + "m"
    : minutes
      ? minutes + "m " + seconds + "s"
      : seconds + "s";
}

function fmtBytes(bytes) {
  if (!Number.isFinite(bytes)) return "—";
  return bytes > 1048576
    ? (bytes / 1048576).toFixed(1) + " MB"
    : Math.round(bytes / 1024) + " KB";
}

function fmtMs(ms) {
  return ms >= 1000 ? (ms / 1000).toFixed(1) + " s" : ms + " ms";
}

function setRange(input, value, label, suffix) {
  if (document.activeElement !== input && Number.isFinite(value)) {
    input.value = value;
    label.textContent = value + suffix;
  }
}

function domainsForAction(name) {
  if (["forward", "backward", "left", "right", "stop", "turn_relative", "dance",
    "motor_speed", "drive_duration"].includes(name)) return ["motors"];
  if (name === "cliff_threshold" || name === "motion_emotions") return ["sensors"];
  if (name.startsWith("oled_") || name.startsWith("auto_brightness") ||
    ["display_flip", "screen_brightness",
    "status_light_brightness", "lights_toggle"].includes(name)) return ["display"];
  if (["speaker_volume", "microphone_gain", "microphone_mute", "audio_test"].includes(name)) {
    return ["audio"];
  }
  if (name === "camera_flip" || name === "live_camera") return ["camera"];
  if (name.startsWith("battery_")) return ["battery"];
  return ["core"];
}

async function action(name, extra = {}) {
  try {
    const response = await fetch("/api/action", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: name, ...extra }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Command failed");
    notify(result.message || "Done");
    queueDomains(domainsForAction(name), 120);
    if (name === "camera_flip") loadCameraSettings();
  } catch (error) {
    notify(error.message || "Robot is offline");
  }
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (character) =>
    character === "&" ? "&amp;"
      : character === "<" ? "&lt;"
        : character === ">" ? "&gt;"
          : character === '"' ? "&quot;" : "&#39;",
  );
}
