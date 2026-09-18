const LIVE_DRIVE_INTERVAL_MS = 100;
const JOYSTICK_DEAD_ZONE = 0.12;

let driveMode = "pad";
let liveDriveActive = false;
let liveDrivePointer = null;
let liveDriveTimer = null;
let liveDrivePending = false;
let liveDriveDirty = false;
let liveDriveLeft = 0;
let liveDriveRight = 0;
let liveDriveErrorShown = false;

function renderJoystick(left = liveDriveLeft, right = liveDriveRight) {
  $("#joystickOutput").textContent = "L " + left + "% · R " + right + "%";
}

function postLiveDrive(left, right, keepalive = false) {
  if (!keepalive && liveDrivePending) {
    liveDriveDirty = true;
    return;
  }
  if (!keepalive) {
    liveDrivePending = true;
    liveDriveDirty = false;
  }
  fetch("/api/drive/live", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ left, right }),
    keepalive,
  }).then(async (response) => {
    if (response.ok) {
      liveDriveErrorShown = false;
      return;
    }
    const result = await response.json().catch(() => ({}));
    throw Error(result.message || "Live drive rejected");
  }).catch((error) => {
    if (!keepalive && liveDriveActive) stopLiveDrive(false);
    if (!keepalive && !liveDriveErrorShown) {
      liveDriveErrorShown = true;
      notify(error.message || "Robot is offline");
    }
  }).finally(() => {
    if (keepalive) return;
    liveDrivePending = false;
    if (liveDriveActive && liveDriveDirty) {
      postLiveDrive(liveDriveLeft, liveDriveRight);
    }
  });
}

function sendLiveDrive() {
  if (liveDriveActive) postLiveDrive(liveDriveLeft, liveDriveRight);
}

function stopLiveDrive(sendStop = true) {
  liveDriveActive = false;
  liveDrivePointer = null;
  liveDriveLeft = 0;
  liveDriveRight = 0;
  liveDriveDirty = false;
  clearInterval(liveDriveTimer);
  liveDriveTimer = null;
  const joystick = $("#joystick");
  if (joystick) joystick.classList.remove("active");
  const knob = $("#joystickKnob");
  if (knob) knob.style.transform = "translate(-50%, -50%)";
  if ($("#joystickOutput")) renderJoystick();
  if (sendStop) postLiveDrive(0, 0, true);
}

function updateJoystick(event) {
  const joystick = $("#joystick");
  const bounds = joystick.getBoundingClientRect();
  const radius = Math.max(1, bounds.width / 2 - 43);
  let x = (event.clientX - (bounds.left + bounds.width / 2)) / radius;
  let y = (event.clientY - (bounds.top + bounds.height / 2)) / radius;
  const rawMagnitude = Math.hypot(x, y);
  if (rawMagnitude > 1) {
    x /= rawMagnitude;
    y /= rawMagnitude;
  }

  const magnitude = Math.min(1, rawMagnitude);
  let scaledX = 0;
  let scaledY = 0;
  if (magnitude > JOYSTICK_DEAD_ZONE) {
    const scaledMagnitude = (magnitude - JOYSTICK_DEAD_ZONE) / (1 - JOYSTICK_DEAD_ZONE);
    const divisor = Math.hypot(x, y) || 1;
    scaledX = x / divisor * scaledMagnitude;
    scaledY = y / divisor * scaledMagnitude;
  }

  const throttle = -scaledY;
  const turn = scaledX;
  let left = throttle + turn;
  let right = throttle - turn;
  const peak = Math.max(1, Math.abs(left), Math.abs(right));
  left = Math.round(left / peak * 100);
  right = Math.round(right / peak * 100);

  if (cliffDetected && (left > 0 || right > 0)) {
    left = 0;
    right = 0;
  }
  liveDriveLeft = left;
  liveDriveRight = right;
  $("#joystickKnob").style.transform =
    "translate(calc(-50% + " + Math.round(scaledX * radius) + "px), " +
    "calc(-50% + " + Math.round(scaledY * radius) + "px))";
  renderJoystick();
}

function setDriveMode(mode) {
  const next = mode === "joystick" ? "joystick" : "pad";
  if (driveMode !== next) emergencyStop();
  driveMode = next;
  try {
    localStorage.setItem("xiaozhiDriveMode", driveMode);
  } catch (_) {}
  $("#padMode").classList.toggle("active", driveMode === "pad");
  $("#joystickMode").classList.toggle("active", driveMode === "joystick");
  $("#padDrivePanel").hidden = driveMode !== "pad";
  $("#joystickDrivePanel").hidden = driveMode !== "joystick";
}

function bindLiveDriveControls() {
  $("#padMode").onclick = () => setDriveMode("pad");
  $("#joystickMode").onclick = () => setDriveMode("joystick");
  const joystick = $("#joystick");
  joystick.oncontextmenu = (event) => event.preventDefault();
  joystick.onpointerdown = (event) => {
    if (driveMode !== "joystick" || liveDrivePointer !== null) return;
    event.preventDefault();
    liveDrivePointer = event.pointerId;
    liveDriveActive = true;
    liveDriveErrorShown = false;
    joystick.setPointerCapture(event.pointerId);
    joystick.classList.add("active");
    updateJoystick(event);
    sendLiveDrive();
    clearInterval(liveDriveTimer);
    liveDriveTimer = setInterval(sendLiveDrive, LIVE_DRIVE_INTERVAL_MS);
  };
  joystick.onpointermove = (event) => {
    if (event.pointerId === liveDrivePointer) updateJoystick(event);
  };
  const release = (event) => {
    if (event.pointerId === liveDrivePointer) stopLiveDrive(true);
  };
  joystick.onpointerup = release;
  joystick.onpointercancel = release;
  joystick.onlostpointercapture = (event) => {
    if (event.pointerId === liveDrivePointer) stopLiveDrive(true);
  };

  let savedMode = "pad";
  try {
    savedMode = localStorage.getItem("xiaozhiDriveMode") || "pad";
  } catch (_) {}
  setDriveMode(savedMode);
}
