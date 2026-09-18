let serverConfigSaving = false;
let defaultServerUrl = "";

function setServerControlsDisabled(disabled) {
  $("#serverUrl").disabled = disabled;
  $("#saveServer").disabled = disabled;
  $("#defaultServer").disabled = disabled;
}

function applyServerConfig(config) {
  defaultServerUrl = String(config.default_url || "");
  $("#serverUrl").value = String(config.effective_url || defaultServerUrl);
  $("#serverStatus").textContent = config.using_default ? "Firmware default" : "Custom server";
}

async function loadServerConfig() {
  try {
    const response = await fetch("/api/server", { cache: "no-store" });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Unable to load server");
    applyServerConfig(result);
  } catch (error) {
    $("#serverStatus").textContent = "Unavailable";
    notify(error.message || "Unable to load server");
  }
}

async function saveServerConfig(useDefault = false) {
  if (serverConfigSaving) return;
  const url = useDefault ? "" : $("#serverUrl").value.trim();
  if (!useDefault && !/^https?:\/\/[^/\s]+/i.test(url)) {
    notify("Enter a valid http:// or https:// URL");
    return;
  }
  const shownUrl = useDefault ? defaultServerUrl : url;
  if (!confirm("Save " + shownUrl + " and restart the robot?")) return;

  serverConfigSaving = true;
  let rebooting = false;
  setServerControlsDisabled(true);
  $("#serverStatus").textContent = "Saving…";
  try {
    const response = await fetch("/api/server", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) throw Error(result.message || "Could not save server");
    applyServerConfig(result);
    rebooting = !!result.restart_required;
    $("#serverStatus").textContent = "Restarting…";
    notify("Server saved; robot is restarting");
  } catch (error) {
    $("#serverStatus").textContent = "Save failed";
    notify(error.message || "Could not save server");
  } finally {
    serverConfigSaving = false;
    if (!rebooting) setServerControlsDisabled(false);
  }
}

function bindServerConfigControls() {
  $("#saveServer").onclick = () => saveServerConfig(false);
  $("#defaultServer").onclick = () => saveServerConfig(true);
  $("#serverUrl").onkeydown = (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      saveServerConfig(false);
    }
  };
}
