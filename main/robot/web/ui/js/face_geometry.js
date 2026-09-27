const faceGeometryEmotions = [
  "neutral", "happy", "bored", "laughing", "funny", "sad", "angry", "crying",
  "loving", "embarrassed", "surprised", "shocked", "thinking", "winking", "cool",
  "relaxed", "delicious", "kissy", "confident", "sleepy", "silly", "confused",
  "suspicious", "shake",
];

const faceEyeFields = [
  ["width", "Width", 8, 96, 1], ["height", "Height", 7, 88, 1],
  ["x", "X", -100, 100, 1], ["y", "Y", -110, 80, 1],
  ["rotation", "Rotation · 0.1°", -1800, 1800, 1],
  ["top_curve", "Top curve", -44, 44, 1],
  ["bottom_curve", "Bottom curve", -44, 44, 1],
  ["slope", "Slope", -44, 44, 1], ["water", "Water", 0, 40, 1],
];

const faceMouthFields = [
  ["width", "Width", 8, 120, 1], ["height", "Height", 7, 80, 1],
  ["top_curve", "Top curve", -40, 40, 0.5],
  ["bottom_curve", "Bottom curve", -40, 40, 0.5],
  ["slope", "Slope", -40, 40, 0.5], ["gap", "Gap", 20, 110, 1],
];

let faceGeometryValue = null;
let faceGeometryCustomized = false;
let faceGeometryLoaded = false;
let faceGeometryLayoutYOffset = 0;

function faceGeometryField(group, definition) {
  const [key, label, minimum, maximum, step] = definition;
  const row = document.createElement("label");
  row.className = "face-geometry-field";
  row.innerHTML = `<span>${label}</span><input type="range" min="${minimum}" max="${maximum}" step="${step}"><input class="face-geometry-number" type="number" min="${minimum}" max="${maximum}" step="${step}">`;
  const range = row.children[1];
  const number = row.children[2];
  range.dataset.faceGroup = group;
  range.dataset.faceField = key;
  number.dataset.faceGroup = group;
  number.dataset.faceField = key;
  const update = (source, target) => {
    if (!faceGeometryValue) return;
    const value = Math.max(minimum, Math.min(maximum, Number(source.value)));
    if (!Number.isFinite(value)) return;
    source.value = value;
    target.value = value;
    faceGeometryValue[group][key] = value;
    $("#faceGeometryState").textContent = "Unsaved";
    $("#faceGeometryHint").textContent = `Editing local ${$("#faceGeometryEmotion").value} geometry`;
    renderFaceGeometryPreview();
  };
  range.oninput = () => update(range, number);
  number.oninput = () => update(number, range);
  return row;
}

function buildFaceGeometryControls() {
  [["left_eye", "#faceGeometryLeft", faceEyeFields],
    ["right_eye", "#faceGeometryRight", faceEyeFields],
    ["mouth", "#faceGeometryMouth", faceMouthFields]].forEach(([group, selector, fields]) => {
    const target = $(selector);
    fields.forEach((field) => target.appendChild(faceGeometryField(group, field)));
  });
}

function fillFaceGeometryControls() {
  $all("[data-face-group]").forEach((input) => {
    input.value = faceGeometryValue[input.dataset.faceGroup][input.dataset.faceField];
  });
}

function roundedFaceDistance(x, y, halfWidth, halfHeight, radius) {
  const qx = Math.abs(x) - halfWidth + radius;
  const qy = Math.abs(y) - halfHeight + radius;
  const dx = Math.max(qx, 0);
  const dy = Math.max(qy, 0);
  return (dx > 0 && dy > 0 ? Math.hypot(dx, dy) : dx + dy) +
    Math.min(Math.max(qx, qy), 0) - radius;
}

function faceSmoothStep(value) {
  return value * value * (3 - 2 * value);
}

function mixFaceColor(first, second, amount) {
  return [0, 1, 2].map((index) => Math.round(first[index] +
    (second[index] - first[index]) * amount));
}

function paintFacePixel(data, index, color, coverage) {
  const alpha = Math.max(0, Math.min(1, coverage));
  data[index] = Math.round(data[index] * (1 - alpha) + color[0] * alpha);
  data[index + 1] = Math.round(data[index + 1] * (1 - alpha) + color[1] * alpha);
  data[index + 2] = Math.round(data[index + 2] * (1 - alpha) + color[2] * alpha);
  data[index + 3] = 255;
}

function rasterFaceShape(image, geometry, centerX, centerY, rotation, waterSide = 0) {
  const width = image.width;
  const height = image.height;
  const halfWidth = geometry.width * 0.5;
  const shapeHeight = Math.max(7, geometry.height);
  const radians = rotation * Math.PI / 1800;
  const cosine = Math.cos(radians);
  const sine = Math.sin(radians);
  for (let screenY = 0; screenY < height; screenY += 1) {
    for (let screenX = 0; screenX < width; screenX += 1) {
      const dx = screenX + 0.5 - centerX;
      const dy = screenY + 0.5 - centerY;
      const px = cosine * dx + sine * dy;
      const py = -sine * dx + cosine * dy;
      if (Math.abs(px) > halfWidth + 1 || Math.abs(py) > shapeHeight * 0.5 + 45) continue;
      const u = Math.max(-1, Math.min(1, px / halfWidth));
      const curve = 1 - u * u;
      const top = -shapeHeight * 0.5 + geometry.top_curve * curve + geometry.slope * u;
      const bottom = shapeHeight * 0.5 + geometry.bottom_curve * curve;
      const halfHeight = Math.max(3.5, (bottom - top) * 0.5);
      const shapeCenter = (top + bottom) * 0.5;
      const radius = Math.min(18, halfWidth, halfHeight);
      const distance = roundedFaceDistance(px, py - shapeCenter, halfWidth, halfHeight, radius);
      const coverage = Math.max(0, Math.min(1, 0.5 - distance));
      if (!coverage) continue;

      const layerScale = Math.max(0, Math.min(1, shapeHeight / 54));
      const innerX = px - 5 * layerScale;
      const innerU = Math.max(-1, Math.min(1, innerX / halfWidth));
      const innerCurve = 1 - innerU * innerU;
      const innerTop = -shapeHeight * 0.5 +
        geometry.top_curve * innerCurve + geometry.slope * innerU;
      const innerBottom = shapeHeight * 0.5 + geometry.bottom_curve * innerCurve;
      const innerHalfHeight = Math.max(3.5, (innerBottom - innerTop) * 0.5);
      const innerCenter = (innerTop + innerBottom) * 0.5 - 6 * layerScale;
      const innerRadius = Math.min(18, halfWidth, innerHalfHeight);
      const innerDistance = roundedFaceDistance(
        innerX, py - innerCenter, halfWidth, innerHalfHeight, innerRadius,
      );
      const blend = faceSmoothStep(Math.max(0, Math.min(1, (1 - innerDistance) / 2)));
      let color = mixFaceColor([137, 106, 54], [198, 161, 91], blend);
      if (geometry.water > 0 && waterSide) {
        const outer = waterSide < 0 ? -u : u;
        const spread = Math.max(0, Math.min(1, (outer + 0.35) / 1.35));
        const waterline = bottom - geometry.water * faceSmoothStep(spread);
        const waterMix = Math.max(0, Math.min(1, (py - waterline + 1) / 2));
        color = mixFaceColor(color, [128, 100, 59], waterMix);
      }
      paintFacePixel(image.data, (screenY * width + screenX) * 4, color, coverage);
    }
  }
}

function renderFaceGeometryPreview() {
  if (!faceGeometryValue) return;
  const canvas = $("#faceGeometryCanvas");
  const context = canvas.getContext("2d");
  const image = context.createImageData(240, 240);
  for (let index = 3; index < image.data.length; index += 4) image.data[index] = 255;
  const left = faceGeometryValue.left_eye;
  const right = faceGeometryValue.right_eye;
  rasterFaceShape(
    image, left, 120 + left.x, 120 + left.y + faceGeometryLayoutYOffset, left.rotation, -1,
  );
  rasterFaceShape(
    image, right, 120 + right.x, 120 + right.y + faceGeometryLayoutYOffset, right.rotation, 1,
  );
  const mouth = faceGeometryValue.mouth;
  const mouthGeometry = { ...mouth, water: 0 };
  const mouthX = 120 + (left.x + right.x) * 0.5;
  const mouthY = 120 + (left.y + right.y) * 0.5 + faceGeometryLayoutYOffset + mouth.gap;
  rasterFaceShape(image, mouthGeometry, mouthX, mouthY, 0);
  context.putImageData(image, 0, 0);
}

async function loadFaceGeometry() {
  const emotion = $("#faceGeometryEmotion").value;
  $("#faceGeometryState").textContent = "Loading";
  try {
    const response = await fetchWithTimeout(
      `/api/face-geometry?emotion=${encodeURIComponent(emotion)}`,
    );
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Could not load geometry");
    faceGeometryValue = result.geometry;
    faceGeometryLayoutYOffset = Number(result.metadata.layout_y_offset);
    faceGeometryCustomized = !!result.customized;
    faceGeometryLoaded = true;
    fillFaceGeometryControls();
    renderFaceGeometryPreview();
    $("#faceGeometryState").textContent = result.state;
    $("#faceGeometryHint").textContent = faceGeometryCustomized
      ? "Saved custom geometry is active" : "Using exact compiled default";
  } catch (error) {
    $("#faceGeometryState").textContent = "Unavailable";
    $("#faceGeometryHint").textContent = error.message || "Robot is offline";
  }
}

async function saveFaceGeometry() {
  if (!faceGeometryLoaded || !faceGeometryValue) return;
  const emotion = $("#faceGeometryEmotion").value;
  try {
    const response = await fetchWithTimeout("/api/face-geometry", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ emotion, geometry: faceGeometryValue }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Could not save geometry");
    faceGeometryValue = result.geometry;
    faceGeometryLayoutYOffset = Number(result.metadata.layout_y_offset);
    faceGeometryCustomized = true;
    fillFaceGeometryControls();
    renderFaceGeometryPreview();
    $("#faceGeometryState").textContent = "Customized";
    $("#faceGeometryHint").textContent = "Saved and active on Mochan";
    notify(result.message || "Face geometry saved");
  } catch (error) {
    notify(error.message || "Robot is offline");
  }
}

async function resetFaceGeometry(resetAll) {
  const emotion = $("#faceGeometryEmotion").value;
  const message = resetAll
    ? "Reset saved geometry for all 24 emotions?"
    : `Reset ${emotion} to its compiled default?`;
  if (!confirm(message)) return;
  const url = resetAll ? "/api/face-geometry"
    : `/api/face-geometry?emotion=${encodeURIComponent(emotion)}`;
  try {
    const response = await fetchWithTimeout(url, { method: "DELETE" });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Could not reset geometry");
    notify(result.message || "Face geometry restored");
    await loadFaceGeometry();
  } catch (error) {
    notify(error.message || "Robot is offline");
  }
}

function bindFaceGeometryEditor() {
  const select = $("#faceGeometryEmotion");
  faceGeometryEmotions.forEach((emotion) => {
    const option = document.createElement("option");
    option.value = emotion;
    option.textContent = emotion.charAt(0).toUpperCase() + emotion.slice(1);
    select.appendChild(option);
  });
  buildFaceGeometryControls();
  $("#faceGeometryEditor").addEventListener("toggle", (event) => {
    if (event.target.open && !faceGeometryLoaded) loadFaceGeometry();
  });
  select.onchange = loadFaceGeometry;
  $("#faceGeometrySave").onclick = saveFaceGeometry;
  $("#faceGeometryReset").onclick = () => resetFaceGeometry(false);
  $("#faceGeometryResetAll").onclick = () => resetFaceGeometry(true);
}
