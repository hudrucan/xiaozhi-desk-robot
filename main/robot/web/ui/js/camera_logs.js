function clearSnapshot() {
  const image = $("#snapshotImage");
  image.onload = null;
  image.onerror = null;
  if (snapshotUrl) {
    URL.revokeObjectURL(snapshotUrl);
    snapshotUrl = "";
  }
  image.removeAttribute("src");
  $("#snapshot").classList.remove("has-image");
}

async function captureSnapshot() {
  if (snapshotPending) return;
  snapshotPending = true;
  const button = $("#takeSnapshot");
  const metadata = $("#snapshotMeta");
  button.disabled = true;
  button.textContent = "Capturing…";
  metadata.textContent = "Waiting for camera";
  try {
    const response = await fetch("/api/camera/snapshot?ts=" + Date.now(), { cache: "no-store" });
    if (!response.ok) {
      let message = "Capture failed";
      try {
        message = (await response.json()).message || message;
      } catch (_) {}
      throw Error(message);
    }
    const blob = await response.blob();
    if (snapshotUrl) URL.revokeObjectURL(snapshotUrl);
    snapshotUrl = URL.createObjectURL(blob);
    const image = $("#snapshotImage");
    image.onload = () => {
      metadata.textContent = image.naturalWidth + " × " + image.naturalHeight + " · " +
        Math.round(blob.size / 1024) + " KB";
    };
    image.src = snapshotUrl;
    $("#snapshot").classList.add("has-image");
  } catch (error) {
    metadata.textContent = error.message;
    notify(error.message);
  } finally {
    snapshotPending = false;
    button.disabled = false;
    button.textContent = "Take photo";
  }
}

function stopBrowserLive(message, clear = false) {
  browserLive = false;
  browserLiveConfirmed = false;
  $("#browserLive").classList.remove("on");
  $("#browserLive").textContent = "Live";
  if (clear) clearSnapshot();
  if (message) $("#snapshotMeta").textContent = message;
}

async function setWebCameraMode(mode) {
  try {
    const response = await fetch("/api/camera/mode", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Camera mode failed");
    statusCache.web_camera_live = mode === "web";
    queueDomains(["camera"], 120);
    return true;
  } catch (error) {
    notify(error.message || "Camera mode failed");
    return false;
  }
}

async function toggleBrowserLive() {
  if (browserLive || statusCache.web_camera_live) {
    stopBrowserLive("Live preview stopped", true);
    await setWebCameraMode("off");
    return;
  }
  if (lastState !== "idle") {
    notify("Browser live view is available only while Idle");
    return;
  }
  if (!await setWebCameraMode("web")) return;
  browserLive = true;
  browserLiveConfirmed = false;
  $("#browserLive").classList.add("on");
  $("#browserLive").textContent = "Stop live";
  clearSnapshot();
  const image = $("#snapshotImage");
  image.onerror = () => {
    if (browserLive) {
      stopBrowserLive("Live preview disconnected", true);
      setWebCameraMode("off");
    }
  };
  image.src = "/api/camera/stream?ts=" + Date.now();
  $("#snapshot").classList.add("has-image");
  $("#snapshotMeta").textContent = "Connecting to MJPEG stream";
}

const logOutput = $("#logOutput");
const pauseLog = $("#pauseLog");
const logState = $("#logState");
const autoScroll = $("#autoScroll");

function stripAnsi(text) {
  return text.replace(/\u001b\[[0-9;]*[A-Za-z]/g, "");
}

function logLevel(line) {
  if (/(^|\s)E \(|\*\*\*ERROR|Guru Meditation|panic/i.test(line)) return "error";
  if (/(^|\s)W \(/.test(line)) return "warn";
  return "info";
}

function hasLogSelection() {
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed || !selection.rangeCount) return false;
  return logOutput.contains(selection.anchorNode) || logOutput.contains(selection.focusNode);
}

function renderLogs(force = false) {
  if (!force && hasLogSelection()) {
    logRenderPending = true;
    return;
  }
  const query = $("#logSearch").value.toLowerCase();
  const filter = $("#logFilter").value;
  const lines = allLogs.split("\n");
  const visible = lines.filter((line) =>
    (filter === "all" || logLevel(line) === filter) &&
    (!query || line.toLowerCase().includes(query)),
  );
  const pinned = logOutput.scrollHeight - logOutput.scrollTop - logOutput.clientHeight < 30;
  logOutput.textContent = visible.join("\n") || "No matching logs";
  const errors = lines.filter((line) => logLevel(line) === "error").length;
  const badge = $("#errorBadge");
  badge.textContent = errors;
  badge.classList.toggle("show", errors > 0);
  $("#logSize").textContent = Math.round(allLogs.length / 1024) + " KB cached · 16 KB device";
  if (autoScroll.checked && pinned) logOutput.scrollTop = logOutput.scrollHeight;
  logRenderPending = false;
}

async function fetchLogs() {
  if (logPaused || logPending) return;
  logPending = true;
  try {
    const response = await fetch("/api/log?since=" + logCursor, { cache: "no-store" });
    if (!response.ok) throw Error("Log request failed");
    const nextCursor = Number(response.headers.get("X-Log-Cursor") || logCursor);
    const reset = response.headers.get("X-Log-Reset") === "1";
    const text = stripAnsi(await response.text());
    const resetView = !logStarted || reset;
    if (resetView) {
      allLogs = "";
      logStarted = true;
    }
    if (text) allLogs += text;
    if (allLogs.length > 65536) allLogs = allLogs.slice(-65536);
    logCursor = nextCursor;
    logState.textContent = "Live system log";
    const changed = resetView || !!text;
    if (changed) {
      try {
        localStorage.setItem("xiaozhiLogs", allLogs);
      } catch (_) {}
    }
    if (changed || logRenderPending) renderLogs();
  } catch (_) {
    logState.textContent = "Log disconnected";
  } finally {
    logPending = false;
  }
}

function toggleLogPause() {
  logPaused = !logPaused;
  pauseLog.textContent = logPaused ? "Resume" : "Pause";
  logState.textContent = logPaused ? "Log paused" : "Live system log";
  if (!logPaused) fetchLogs();
}

function clearLogs() {
  allLogs = "";
  logStarted = true;
  logRenderPending = false;
  try {
    localStorage.removeItem("xiaozhiLogs");
  } catch (_) {}
  renderLogs(true);
}

function downloadLogs() {
  const anchor = document.createElement("a");
  anchor.href = URL.createObjectURL(new Blob([allLogs], { type: "text/plain" }));
  anchor.download = "xiaozhi-" + new Date().toISOString().replaceAll(":", "-") + ".log";
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(anchor.href), 1000);
}

function restoreLogs() {
  try {
    allLogs = localStorage.getItem("xiaozhiLogs") || "";
    if (allLogs) {
      logStarted = true;
      renderLogs(true);
    }
  } catch (_) {}
}
