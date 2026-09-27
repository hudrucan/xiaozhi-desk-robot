function cameraMotionClamp(value) {
  return Math.min(1, Math.max(0, value));
}

const cameraMotionSettingIds = ["cameraMotionCellThreshold", "cameraMotionActivityRatio",
  "cameraMotionEnterRatio", "cameraMotionExitRatio", "cameraMotionEnterSamples",
  "cameraMotionExitSamples", "cameraMotionActivityHold"];

function cameraMotionRadioValue(name, allowed, label) {
  const selected = $(`input[name="${name}"]:checked`);
  const value = selected ? Number(selected.value) : 0;
  if (!allowed.includes(value)) throw Error("Unsupported " + label);
  return value;
}

function cameraMotionInteger(id, label, minimum, maximum) {
  const value = Number($("#" + id).value);
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw Error(label + " must be an integer from " + minimum + " to " + maximum);
  }
  return value;
}

function cameraMotionRatioBp(id, label) {
  const percent = Number($("#" + id).value);
  const basisPoints = Math.round(percent * 100);
  if (!Number.isFinite(percent) || basisPoints < 1 || basisPoints > 10000) {
    throw Error(label + " must be from 0.01% to 100%");
  }
  return basisPoints;
}

function populateCameraMotionSettings(vision = {}) {
  const quiet = [500, 1000, 2000].includes(Number(vision.quiet_interval_ms))
    ? Number(vision.quiet_interval_ms) : 1000;
  const active = [100, 250, 500].includes(Number(vision.active_interval_ms))
    ? Number(vision.active_interval_ms) : 250;
  const quietControl = $(`input[name="cameraObserverQuietInterval"][value="${quiet}"]`);
  const activeControl = $(`input[name="cameraObserverActiveInterval"][value="${active}"]`);
  if (quietControl) quietControl.checked = true;
  if (activeControl) activeControl.checked = true;
  $("#cameraMotionCellThreshold").value = Number(vision.cell_threshold) || 18;
  $("#cameraMotionActivityRatio").value = (Number(vision.activity_ratio_bp) || 200) / 100;
  $("#cameraMotionEnterRatio").value = (Number(vision.enter_ratio_bp) || 500) / 100;
  $("#cameraMotionExitRatio").value = (Number(vision.exit_ratio_bp) || 300) / 100;
  $("#cameraMotionEnterSamples").value = Number(vision.enter_samples) || 2;
  $("#cameraMotionExitSamples").value = Number(vision.exit_samples) || 3;
  $("#cameraMotionActivityHold").value = Number(vision.activity_hold_ms) || 1500;
}

function collectCameraMotionSettings(enabled) {
  const vision = {
    enabled,
    quiet_interval_ms: cameraMotionRadioValue(
      "cameraObserverQuietInterval", [500, 1000, 2000], "quiet interval"),
    active_interval_ms: cameraMotionRadioValue(
      "cameraObserverActiveInterval", [100, 250, 500], "active interval"),
    cell_threshold: cameraMotionInteger(
      "cameraMotionCellThreshold", "Cell threshold", 1, 255),
    activity_ratio_bp: cameraMotionRatioBp("cameraMotionActivityRatio", "Activity"),
    enter_ratio_bp: cameraMotionRatioBp("cameraMotionEnterRatio", "Motion enter"),
    exit_ratio_bp: cameraMotionRatioBp("cameraMotionExitRatio", "Motion exit"),
    enter_samples: cameraMotionInteger(
      "cameraMotionEnterSamples", "Enter samples", 1, 10),
    exit_samples: cameraMotionInteger(
      "cameraMotionExitSamples", "Exit samples", 1, 10),
    activity_hold_ms: cameraMotionInteger(
      "cameraMotionActivityHold", "Activity hold", 250, 10000),
  };
  if (vision.active_interval_ms > vision.quiet_interval_ms) {
    throw Error("Active interval must not exceed quiet interval");
  }
  if (vision.activity_ratio_bp > vision.enter_ratio_bp) {
    throw Error("Activity threshold must not exceed motion enter threshold");
  }
  if (vision.exit_ratio_bp > vision.enter_ratio_bp) {
    throw Error("Motion exit threshold must not exceed motion enter threshold");
  }
  return vision;
}

function setCameraMotionSettingsDisabled(disabled) {
  $("#cameraObserverEnabled").disabled = disabled;
  $all('input[name="cameraObserverQuietInterval"], ' +
    'input[name="cameraObserverActiveInterval"]').forEach((input) => {
    input.disabled = disabled;
  });
  cameraMotionSettingIds.forEach((id) => { $("#" + id).disabled = disabled; });
}

function bindCameraMotionSettings(onchange) {
  $all('input[name="cameraObserverQuietInterval"], ' +
    'input[name="cameraObserverActiveInterval"]').forEach((input) => {
    input.onchange = onchange;
  });
  cameraMotionSettingIds.forEach((id) => { $("#" + id).onchange = onchange; });
}

function parseCameraMotionHeaders(headers) {
  if (headers.get("X-Motion-Valid") !== "1") return { valid: false };
  const number = (name) => Number(headers.get(name));
  const motion = {
    valid: true,
    centroidX: number("X-Motion-Centroid-X"),
    centroidY: number("X-Motion-Centroid-Y"),
    bboxLeft: number("X-Motion-Bbox-Left"),
    bboxTop: number("X-Motion-Bbox-Top"),
    bboxRight: number("X-Motion-Bbox-Right"),
    bboxBottom: number("X-Motion-Bbox-Bottom"),
    horizontalRegion: headers.get("X-Motion-H-Region") || "none",
    verticalRegion: headers.get("X-Motion-V-Region") || "none",
  };
  const coordinates = [motion.centroidX, motion.centroidY, motion.bboxLeft,
    motion.bboxTop, motion.bboxRight, motion.bboxBottom];
  if (!coordinates.every(Number.isFinite)) return { valid: false };
  motion.centroidX = cameraMotionClamp(motion.centroidX);
  motion.centroidY = cameraMotionClamp(motion.centroidY);
  motion.bboxLeft = cameraMotionClamp(motion.bboxLeft);
  motion.bboxTop = cameraMotionClamp(motion.bboxTop);
  motion.bboxRight = cameraMotionClamp(motion.bboxRight);
  motion.bboxBottom = cameraMotionClamp(motion.bboxBottom);
  if (motion.bboxRight <= motion.bboxLeft || motion.bboxBottom <= motion.bboxTop) {
    return { valid: false };
  }
  return motion;
}

function drawCameraMotionOverlay(context, canvas, motion) {
  if (!motion?.valid) return;
  const left = motion.bboxLeft * canvas.width;
  const top = motion.bboxTop * canvas.height;
  const right = motion.bboxRight * canvas.width;
  const bottom = motion.bboxBottom * canvas.height;
  const centroidX = motion.centroidX * canvas.width;
  const centroidY = motion.centroidY * canvas.height;
  const label = motion.horizontalRegion + " / " + motion.verticalRegion;

  context.save();
  context.strokeStyle = "#f0c76a";
  context.fillStyle = "#f0c76a";
  context.lineWidth = 1.5;
  context.strokeRect(left + 0.5, top + 0.5,
    Math.max(0, right - left - 1), Math.max(0, bottom - top - 1));
  context.beginPath();
  context.arc(centroidX, centroidY, 3, 0, Math.PI * 2);
  context.fill();
  context.beginPath();
  context.moveTo(centroidX - 6, centroidY);
  context.lineTo(centroidX + 6, centroidY);
  context.moveTo(centroidX, centroidY - 6);
  context.lineTo(centroidX, centroidY + 6);
  context.stroke();

  context.font = "9px monospace";
  const labelWidth = Math.ceil(context.measureText(label).width) + 6;
  const labelX = Math.min(Math.max(0, left), canvas.width - labelWidth);
  const labelY = top >= 13 ? top - 12 : Math.min(canvas.height - 12, top + 2);
  context.fillStyle = "rgba(8, 7, 4, 0.82)";
  context.fillRect(labelX, labelY, labelWidth, 12);
  context.fillStyle = "#f0c76a";
  context.fillText(label, labelX + 3, labelY + 9);
  context.restore();
}

function renderCameraMotionTelemetry(status, hasSample) {
  const valid = hasSample && !!status.camera_motion_spatial_valid;
  const set = (id, value) => { $("#" + id).textContent = value; };
  const title = (value) => {
    const text = String(value || "none").replaceAll("_", " ");
    return text.charAt(0).toUpperCase() + text.slice(1);
  };
  const samplingState = String(
    status.camera_vision_observer_sampling_state || "quiet").replaceAll("_", " ");
  set("cameraObserverSamplingState",
    samplingState.charAt(0).toUpperCase() + samplingState.slice(1));
  set("cameraObserverCurrentInterval",
    fmtMs(Number(status.camera_vision_observer_current_interval_ms) || 0));
  const trackSamples = Number(status.camera_motion_track_samples) || 0;
  const trackValid = !!status.camera_motion_track_valid;
  set("cameraMotionTrack", trackValid ? "Tracked" : trackSamples > 0 ? "Collecting" : "None");
  set("cameraMotionDirection", title(status.camera_motion_direction));
  set("cameraMotionVelocity", trackValid
    ? Number(status.camera_motion_velocity_x).toFixed(2) + " / " +
      Number(status.camera_motion_velocity_y).toFixed(2) + " norm/s"
    : "—");
  set("cameraMotionHistory", trackSamples + " samples · " +
    fmtMs(Number(status.camera_motion_track_span_ms) || 0));
  const lastCrossing = String(status.camera_motion_last_crossing || "none");
  set("cameraMotionCrossing", lastCrossing !== "none"
    ? title(lastCrossing).replace(" to ", " → ") + " · " +
      fmtMs(Number(status.camera_motion_last_crossing_age_ms) || 0) + " ago"
    : "—");
  set("cameraMotionCrossingCount", Number(status.camera_motion_crossing_count) || 0);
  if (!valid) {
    ["cameraMotionActiveCells", "cameraMotionCentroid", "cameraMotionRegion",
      "cameraMotionBbox", "cameraMotionBboxArea"].forEach((id) => set(id, "—"));
    return;
  }
  const fixed = (value) => Number.isFinite(Number(value))
    ? Number(value).toFixed(2) : "—";
  set("cameraMotionActiveCells", Number(status.camera_motion_active_cells) || 0);
  set("cameraMotionCentroid", fixed(status.camera_motion_centroid_x) + ", " +
    fixed(status.camera_motion_centroid_y));
  set("cameraMotionRegion", String(status.camera_motion_horizontal_region || "none") +
    " / " + String(status.camera_motion_vertical_region || "none"));
  set("cameraMotionBbox", fixed(status.camera_motion_bbox_left) + ", " +
    fixed(status.camera_motion_bbox_top) + " → " +
    fixed(status.camera_motion_bbox_right) + ", " +
    fixed(status.camera_motion_bbox_bottom));
  const area = Number(status.camera_motion_bbox_area_ratio);
  set("cameraMotionBboxArea", Number.isFinite(area) ? (area * 100).toFixed(1) + "%" : "—");
}
