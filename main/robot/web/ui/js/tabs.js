const validTabs = new Set(["overview", "control", "chat", "camera", "device", "diagnostics"]);
const singleColumnTabs = new Set(["overview", "chat", "camera", "diagnostics"]);
const tabMeta = {
  overview: {
    kicker: "System snapshot",
    title: "Overview",
    description: "Live health and essential robot actions.",
  },
  control: {
    kicker: "Motion & expression",
    title: "Robot control",
    description: "Drive, safety, and Mochan behavior in one workspace.",
  },
  chat: {
    kicker: "Assistant session",
    title: "Conversation",
    description: "Typed chat, speech recognition, and server connection.",
  },
  camera: {
    kicker: "Vision",
    title: "Camera",
    description: "Capture, preview, and tune the OV2640 camera.",
  },
  device: {
    kicker: "Hardware",
    title: "Device settings",
    description: "Audio, displays, lighting, and environment sensors.",
  },
  diagnostics: {
    kicker: "System health",
    title: "Diagnostics",
    description: "Memory, reset state, vision health, and live device logs.",
  },
};
let activeTab = "overview";
let cameraTabLoaded = false;
let serverTabLoaded = false;
let tabPanels = [];
let layoutLanes = [];
let inactivePanelStore = null;
let compactLayoutQuery = null;

function tabFromHash() {
  const candidate = location.hash.replace(/^#/, "");
  return validTabs.has(candidate) ? candidate : null;
}

function updateTabPanels(tab) {
  tabPanels.forEach((panel) => {
    panel.hidden = !panel.dataset.tabs.split(/\s+/).includes(tab);
    inactivePanelStore.appendChild(panel);
  });
  layoutLanes.forEach((lane) => { lane.hidden = false; });

  const visiblePanels = tabPanels.filter((panel) => !panel.hidden);
  const useSingleColumn = compactLayoutQuery.matches ||
    visiblePanels.length <= 2 || singleColumnTabs.has(tab);
  if (useSingleColumn) {
    visiblePanels.forEach((panel) => layoutLanes[0].appendChild(panel));
  } else {
    visiblePanels.forEach((panel) => {
      const target = layoutLanes[0].scrollHeight <= layoutLanes[1].scrollHeight
        ? layoutLanes[0] : layoutLanes[1];
      target.appendChild(panel);
    });
  }
  layoutLanes.forEach((lane) => { lane.hidden = !lane.children.length; });
  $(".dashboard").dataset.columns = useSingleColumn ? "1" : "2";
}

function initializePanelLayout() {
  const dashboard = $(".dashboard");
  tabPanels = [...$all("[data-tabs]")];
  layoutLanes = [document.createElement("div"), document.createElement("div")];
  layoutLanes.forEach((lane) => { lane.className = "layout-lane"; });
  inactivePanelStore = document.createElement("div");
  inactivePanelStore.hidden = true;
  dashboard.replaceChildren(...layoutLanes, inactivePanelStore);
  tabPanels.forEach((panel) => inactivePanelStore.appendChild(panel));

  compactLayoutQuery = window.matchMedia("(max-width: 700px)");
  const handleLayoutBreakpoint = () => updateTabPanels(activeTab);
  if (compactLayoutQuery.addEventListener) {
    compactLayoutQuery.addEventListener("change", handleLayoutBreakpoint);
  } else {
    compactLayoutQuery.addListener(handleLayoutBreakpoint);
  }
}

function loadTabResources(tab) {
  if (tab === "camera" && !cameraTabLoaded) {
    cameraTabLoaded = true;
    loadCameraSettings().then((loaded) => { cameraTabLoaded = loaded; });
  }
  if (tab === "chat" && !serverTabLoaded) {
    serverTabLoaded = true;
    loadServerConfig().then((loaded) => { serverTabLoaded = loaded; });
  }
}

function applyTab(tab, updateHash = true) {
  if (!validTabs.has(tab)) tab = "overview";
  const previousTab = activeTab;
  activeTab = tab;
  $(".dashboard").dataset.activeTab = tab;
  $("#pageKicker").textContent = tabMeta[tab].kicker;
  $("#pageTitle").textContent = tabMeta[tab].title;
  $("#pageDescription").textContent = tabMeta[tab].description;

  if (previousTab === "control" && tab !== "control" && liveDriveActive) {
    stopLiveDrive(true);
  }
  if (previousTab === "camera" && tab !== "camera") {
    stopBrowserLiveForNavigation("Live preview stopped after leaving Camera");
  }

  $all('.tab-nav [data-tab]').forEach((button) => {
    const selected = button.dataset.tab === tab;
    button.classList.toggle("active", selected);
    button.setAttribute("aria-selected", selected ? "true" : "false");
    button.tabIndex = selected ? 0 : -1;
  });
  updateTabPanels(tab);
  loadTabResources(tab);

  if (!document.hidden) {
    setStatusTab(tab);
    setLogPollingEnabled(tab === "diagnostics");
  }

  try {
    localStorage.setItem("xiaozhiActiveTab", tab);
  } catch (_) {}
  if (updateHash && location.hash !== "#" + tab) {
    history.replaceState(null, "", "#" + tab);
  }
}

function handlePageVisibility() {
  if (document.hidden) {
    pauseStatusPolling();
    setLogPollingEnabled(false);
    stopBrowserLiveForNavigation("Live preview stopped while page is hidden");
    return;
  }
  setStatusTab(activeTab);
  setLogPollingEnabled(activeTab === "diagnostics");
}

function bindTabs() {
  initializePanelLayout();
  const buttons = [...$all(".tab-nav [data-tab]")];
  buttons.forEach((button) => {
    button.onclick = () => applyTab(button.dataset.tab);
  });
  $(".tab-nav").onkeydown = (event) => {
    if (!["ArrowLeft", "ArrowRight"].includes(event.key)) return;
    event.preventDefault();
    const current = Math.max(0, buttons.indexOf(document.activeElement));
    const direction = event.key === "ArrowRight" ? 1 : -1;
    const next = buttons[(current + direction + buttons.length) % buttons.length];
    applyTab(next.dataset.tab);
    next.focus();
  };
  window.addEventListener("hashchange", () => applyTab(tabFromHash() || "overview", false));
  document.addEventListener("visibilitychange", handlePageVisibility);
  window.addEventListener("pagehide", () => {
    pauseStatusPolling();
    setLogPollingEnabled(false);
    stopBrowserLiveForNavigation("Live preview stopped");
  });

  let initialTab = tabFromHash();
  if (!initialTab) {
    try {
      const saved = localStorage.getItem("xiaozhiActiveTab");
      if (validTabs.has(saved)) initialTab = saved;
    } catch (_) {}
  }
  applyTab(initialTab || "overview");
}
