      const CHAT_MAX_CHARS = 512;
      const $ = (s) => document.querySelector(s),
        $$ = (s) => document.querySelectorAll(s),
        toast = $("#toast");
      let toastTimer,
        statusTimer,
        statusPending = false,
        statusQueuedDelay = null,
        logCursor = 0,
        logStarted = false,
        logPaused = false,
        logPending = false,
        logRenderPending = false,
        allLogs = "",
        snapshotUrl = "",
        browserLive = false,
        snapshotPending = false,
        lastState = "starting",
        liveTimer,
        oledWidgetSignature = "",
        oledPreviewSignature = "",
        conversationSignature = "",
        chatSubmitting = false,
        chatBackendState = "Ready",
        asrEditing = false,
        asrSaving = false;
      function notify(text) {
        toast.textContent = text;
        toast.classList.add("show");
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => toast.classList.remove("show"), 1800);
      }
      function fmtTime(sec) {
        sec = Math.max(0, sec | 0);
        const d = Math.floor(sec / 86400),
          h = Math.floor((sec % 86400) / 3600),
          m = Math.floor((sec % 3600) / 60);
        return d ? d + "d " + h + "h" : h ? h + "h " + m + "m" : m + "m";
      }
      function fmtCapacityTime(sec) {
        sec = Math.max(0, sec | 0);
        const h = Math.floor(sec / 3600),
          m = Math.floor((sec % 3600) / 60),
          s = sec % 60;
        return h ? h + "h " + m + "m" : m ? m + "m " + s + "s" : s + "s";
      }
      function fmtBytes(n) {
        if (!Number.isFinite(n)) return "—";
        return n > 1048576
          ? (n / 1048576).toFixed(1) + " MB"
          : Math.round(n / 1024) + " KB";
      }
      function fmtMs(ms) {
        return ms >= 1000 ? (ms / 1000).toFixed(1) + " s" : ms + " ms";
      }
      async function action(name, extra = {}) {
        try {
          const r = await fetch("/api/action", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ action: name, ...extra }),
          });
          const j = await r.json();
          if (!r.ok || !j.ok) throw Error(j.message || "Command failed");
          notify(j.message || "Done");
          queueStatus(120);
        } catch (e) {
          notify(e.message || "Robot is offline");
        }
      }
      function setRange(input, value, label, suffix) {
        if (document.activeElement !== input && Number.isFinite(value)) {
          input.value = value;
          label.textContent = value + suffix;
        }
      }
      const oledMeta = {
        branding: ["Branding", "Robot identity", "Brand"],
        distance: ["Distance", "Floor range", "Dist"],
        power: ["Current / Power", "INA219 telemetry", "I / P"],
        motion: ["Motion sensor", "State and tilt", "Motion"],
        capacity: ["Battery measuring", "Capacity and time", "mAh"],
      };
      function oledModeOptions(type, selected) {
        const modes =
          type === "power"
            ? ["Current", "Power", "Current + Power"]
            : type === "motion"
              ? ["State", "Tilt", "State + Tilt"]
              : type === "capacity"
                ? ["mAh", "Elapsed", "mAh + elapsed"]
                : ["Default"];
        return modes
          .map(
            (name, index) =>
              '<option value="' +
              index +
              '" ' +
              (index === selected ? "selected" : "") +
              ">" +
              name +
              "</option>",
          )
          .join("");
      }
      function oledPages(widgets) {
        const enabled = widgets.filter((w) => w.enabled),
          pages = [];
        let cursor = 0;
        while (cursor < enabled.length) {
          const left = enabled.length - cursor;
          if (
            left >= 3 &&
            enabled.slice(cursor, cursor + 3).every((w) => w.size === 0)
          ) {
            pages.push(
              enabled.slice(cursor, cursor + 3).map((w, i) => ({
                w,
                x: i === 0 ? 0 : i === 1 ? 32.8125 : 66.40625,
                y: 0,
                width: i === 0 ? 32.8125 : 33.59375,
                height: 100,
              })),
            );
            cursor += 3;
          } else if (
            left >= 2 &&
            enabled[cursor].size < 2 &&
            enabled[cursor + 1].size < 2
          ) {
            pages.push([
              { w: enabled[cursor], x: 0, y: 0, width: 50, height: 100 },
              {
                w: enabled[cursor + 1],
                x: 50,
                y: 0,
                width: 50,
                height: 100,
              },
            ]);
            cursor += 2;
          } else {
            pages.push([
              { w: enabled[cursor], x: 0, y: 0, width: 100, height: 100 },
            ]);
            cursor++;
          }
        }
        return pages;
      }
      function escapeHtml(value) {
        return String(value ?? "").replace(/[&<>"']/g, (ch) =>
          ch === "&"
            ? "&amp;"
            : ch === "<"
              ? "&lt;"
              : ch === ">"
                ? "&gt;"
                : ch === '"'
                  ? "&quot;"
                  : "&#39;",
        );
      }
      function renderOledConfig(j) {
        if (!Array.isArray(j.oled_widgets)) return;
        const widgets = j.oled_widgets.map((w) => ({
          type: String(w.type || "branding"),
          enabled: !!w.enabled,
          size: Math.max(0, Math.min(2, Number(w.size) || 0)),
          mode: Math.max(0, Math.min(2, Number(w.mode) || 0)),
        }));
        const signature = JSON.stringify([
            widgets,
            j.oled_brand,
            j.oled_distance_prefix,
          ]),
          list = $("#oledWidgets");
        const activeElement = document.activeElement;
        const editingOledCustomText = !!(
          activeElement &&
          activeElement.matches &&
          activeElement.matches("[data-oled-custom]")
        );
        if (signature !== oledWidgetSignature && !editingOledCustomText) {
          oledWidgetSignature = signature;
          list.innerHTML = widgets
            .map((w, index) => {
              const meta = oledMeta[w.type] || [w.type, ""];
              const custom =
                w.type === "branding"
                  ? '<label class="oled-custom"><span>Custom text</span><input class="text-input" data-oled-custom="brand" maxlength="20" value="' +
                    escapeHtml(j.oled_brand || "") +
                    '"></label>'
                  : w.type === "distance"
                    ? '<label class="oled-custom"><span>Custom prefix</span><input class="text-input" data-oled-custom="prefix" maxlength="10" value="' +
                      escapeHtml(j.oled_distance_prefix || "") +
                      '"></label>'
                    : "";
              return (
                '<div class="oled-widget"><div class="oled-order"><button data-oled-up="' +
                index +
                '" ' +
                (index === 0 ? "disabled" : "") +
                ' aria-label="Move up">↑</button><button data-oled-down="' +
                index +
                '" ' +
                (index === widgets.length - 1 ? "disabled" : "") +
                ' aria-label="Move down">↓</button></div><label class="toggle"><input type="checkbox" data-oled-on="' +
                index +
                '" ' +
                (w.enabled ? "checked" : "") +
                '></label><div class="oled-widget-name"><b>' +
                escapeHtml(meta[0]) +
                "</b><small>" +
                escapeHtml(meta[1]) +
                '</small></div><select data-oled-size="' +
                index +
                '"><option value="0" ' +
                (w.size === 0 ? "selected" : "") +
                '>S · compact</option><option value="1" ' +
                (w.size === 1 ? "selected" : "") +
                '>M · medium</option><option value="2" ' +
                (w.size === 2 ? "selected" : "") +
                '>L · full</option></select><select data-oled-mode="' +
                index +
                '" ' +
                (w.type === "branding" || w.type === "distance"
                  ? "disabled"
                  : "") +
                ">" +
                oledModeOptions(w.type, w.mode) +
                "</select>" +
                custom +
                "</div>"
              );
            })
            .join("");
          $$("[data-oled-on]").forEach(
            (el) =>
              (el.onchange = () =>
                action("oled_widget_on_" + el.dataset.oledOn, {
                  value: el.checked ? 1 : 0,
                })),
          );
          $$("[data-oled-size]").forEach(
            (el) =>
              (el.onchange = () =>
                action("oled_widget_size_" + el.dataset.oledSize, {
                  value: +el.value,
                })),
          );
          $$("[data-oled-mode]").forEach(
            (el) =>
              (el.onchange = () =>
                action("oled_widget_mode_" + el.dataset.oledMode, {
                  value: +el.value,
                })),
          );
          $$("[data-oled-up]").forEach(
            (el) =>
              (el.onclick = () =>
                action("oled_widget_up_" + el.dataset.oledUp)),
          );
          $$("[data-oled-down]").forEach(
            (el) =>
              (el.onclick = () =>
                action("oled_widget_down_" + el.dataset.oledDown)),
          );
          $$("[data-oled-custom]").forEach((el) => {
            el.onchange = () =>
              action(
                el.dataset.oledCustom === "brand"
                  ? "oled_brand"
                  : "oled_prefix",
                {
                  text: el.value,
                },
              );
            el.onkeydown = (e) => {
              if (e.key === "Enter") el.blur();
            };
          });
        }
        const pages = oledPages(widgets),
          preview = $("#oledPreview"),
          reported = Number(j.oled_page_count),
          pageCount =
            Number.isFinite(reported) && reported > 0 ? reported : pages.length;
        $("#oledPageCount").textContent =
          pageCount + " page" + (pageCount === 1 ? "" : "s");
        const previewSignature = JSON.stringify([widgets, pageCount]);
        if (previewSignature !== oledPreviewSignature) {
          oledPreviewSignature = previewSignature;
          preview.innerHTML = pages.length
            ? pages
                .map(
                  (page, pageIndex) =>
                    '<div class="oled-page">' +
                    page
                      .map(
                        (cell) =>
                          '<div class="oled-cell" style="left:' +
                          cell.x +
                          "%;top:" +
                          cell.y +
                          "%;width:" +
                          cell.width +
                          "%;height:" +
                          cell.height +
                          '%">' +
                          escapeHtml(
                            (oledMeta[cell.w.type] || [
                              cell.w.type,
                              "",
                              cell.w.type,
                            ])[2],
                          ) +
                          "</div>",
                      )
                      .join("") +
                    '<span class="oled-page-num">' +
                    (pageIndex + 1) +
                    "/" +
                    pages.length +
                    "</span></div>",
                )
                .join("")
            : '<div class="oled-empty">No widgets enabled</div>';
        }
      }
      function resizeChatInput() {
        const input = $("#chatInput");
        input.style.height = "36px";
        const height = Math.min(68, input.scrollHeight);
        input.style.height = height + "px";
        input.style.overflowY = input.scrollHeight > 68 ? "auto" : "hidden";
      }
      function updateChatInput() {
        const input = $("#chatInput"),
          count = Array.from(input.value).length,
          empty = !input.value.trim(),
          busy = ["Sending", "Waiting", "Speaking"].includes(chatBackendState);
        resizeChatInput();
        $("#chatCount").textContent = count + " / " + CHAT_MAX_CHARS;
        $("#chatCount").classList.toggle("over", count > CHAT_MAX_CHARS);
        $("#chatSend").disabled =
          chatSubmitting || busy || empty || count > CHAT_MAX_CHARS;
      }
      function renderConversation(conversation) {
        const state = String(conversation.state || "Ready"),
          messages = Array.isArray(conversation.messages)
            ? conversation.messages
            : [],
          error = String(conversation.error || "");
        chatBackendState = state;
        const stateEl = $("#chatState");
        stateEl.textContent = state;
        stateEl.classList.toggle("error", state === "Error");
        const errorEl = $("#chatError");
        errorEl.textContent = error;
        errorEl.classList.toggle("show", state === "Error" && !!error);
        const signature = JSON.stringify(messages);
        if (signature !== conversationSignature) {
          conversationSignature = signature;
          const history = $("#conversationHistory"),
            pinned =
              history.scrollHeight - history.scrollTop - history.clientHeight <
              30;
          history.innerHTML = messages.length
            ? messages
                .map(
                  (message) =>
                    '<div class="chat-row ' +
                    (message.role === "user" ? "user" : "assistant") +
                    '"><span class="chat-role">' +
                    (message.role === "user" ? "You" : "Xiaozhi") +
                    '</span><div class="chat-bubble">' +
                    escapeHtml(message.text || "") +
                    "</div></div>",
                )
                .join("")
            : '<div class="conversation-empty">Type or speak to chat with Xiaozhi</div>';
          if (pinned) history.scrollTop = history.scrollHeight;
        }
        updateChatInput();
      }
      async function submitChat() {
        const input = $("#chatInput"),
          text = input.value,
          count = Array.from(text).length;
        if (chatSubmitting || !text.trim() || count > CHAT_MAX_CHARS) return;
        chatSubmitting = true;
        chatBackendState = "Sending";
        $("#chatState").textContent = "Sending";
        updateChatInput();
        try {
          const response = await fetch("/api/chat", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ text }),
          });
          const result = await response.json();
          if (!response.ok || !result.ok)
            throw Error(result.message || "Message rejected");
          input.value = "";
          updateChatInput();
          queueStatus(40);
        } catch (error) {
          chatBackendState = "Error";
          $("#chatState").textContent = "Error";
          $("#chatState").classList.add("error");
          const errorEl = $("#chatError");
          errorEl.textContent = error.message || "Robot is offline";
          errorEl.classList.add("show");
        } finally {
          chatSubmitting = false;
          updateChatInput();
        }
      }
      async function clearConversation() {
        const button = $("#chatClear");
        button.disabled = true;
        try {
          const response = await fetch("/api/chat", { method: "DELETE" });
          const result = await response.json();
          if (!response.ok || !result.ok)
            throw Error(result.message || "Clear failed");
          conversationSignature = "";
          queueStatus(0);
        } catch (error) {
          notify(error.message || "Robot is offline");
        } finally {
          button.disabled = false;
        }
      }
      $("#chatInput").oninput = updateChatInput;
      $("#chatInput").onkeydown = (event) => {
        if (event.key === "Enter" && !event.shiftKey) {
          event.preventDefault();
          submitChat();
        }
      };
      $("#chatSend").onclick = submitChat;
      $("#chatClear").onclick = clearConversation;
      updateChatInput();
      function applyAsrStatus(asr, forceProvider = false) {
        if (
          (forceProvider || !asrEditing) &&
          (asr.provider === "xiaozhi" || asr.provider === "gemini")
        ) {
          $("#asrProvider").value = asr.provider;
        }
        const providerLabel = asr.provider === "gemini" ? "Gemini active" : "Xiaozhi active";
        $("#asrStatus").textContent =
          providerLabel +
          (asr.gemini_configured ? " · Gemini key saved" : " · Gemini key not set");
        $("#geminiApiKey").placeholder = asr.gemini_configured
          ? "Leave blank to keep saved key"
          : "Enter Gemini API key";
        $("#clearGeminiKey").disabled =
          asrSaving || !asr.gemini_configured;
      }
      function renderStatus(j) {
        renderConversation(j.conversation || {});
        applyAsrStatus(j.asr || {});
        const idle = j.state === "idle",
          m = j.motors || {},
          active = !!(m.moving || m.queued || m.sequence_active),
          cliff = !!j.cliff_detected,
          chatActive = ["connecting", "listening", "speaking"].includes(
            j.state,
          );
        lastState = j.state || "unknown";
        if (!idle && browserLive) stopBrowserLive("Paused until Idle", true);
        $("#online").classList.add("ok");
        $("#online span").textContent = "Online";
        const visibleState =
          j.state === "listening" && !j.asr_ready
            ? j.asr_preparing
              ? "preparing ASR"
              : "processing"
            : (j.state || "idle").replaceAll("_", " ");
        $("#state").textContent = visibleState;
        $("#camera").textContent = !j.camera_available
          ? "Offline"
          : j.live_camera
            ? idle
              ? "Screen preview"
              : "Preview paused"
            : j.camera_flipped
              ? "Flipped"
              : "Normal";
        $("#distance").textContent = j.distance_valid
          ? j.distance_mm + " mm"
          : "—";
        $("#rangeState").textContent = cliff
          ? "Edge · reverse only"
          : j.distance_valid
            ? "Floor detected"
            : "No floor return";
        const batteryReady = !!j.battery_available && !!j.battery_valid,
          signedCurrent = Number(j.battery_signed_current_ma),
          currentMa = Number.isFinite(signedCurrent)
            ? signedCurrent
            : Number(j.battery_current_ma),
          flow = j.battery_charging
            ? "Charging"
            : j.battery_discharging
              ? "Discharging"
              : "Near zero";
        $("#battery").textContent = !j.battery_available
          ? "Offline"
          : batteryReady
            ? flow +
              " · " +
              j.battery_percent +
              "% · " +
              Number(j.battery_voltage_v).toFixed(2) +
              " V"
            : "Waiting";
        $("#power").textContent = batteryReady
          ? (currentMa >= 0 ? "+" : "") +
            Math.round(currentMa) +
            " mA · " +
            (Number(j.battery_power_mw) / 1000).toFixed(2) +
            " W"
          : j.battery_available
            ? "Invalid · CNVR " +
              (j.battery_conversion_ready ? "yes" : "no") +
              " · OVF " +
              (j.battery_math_overflow ? "yes" : "no")
            : "—";
        const capacityMah = Number(j.battery_capacity_test_mah) || 0,
          capacityActive = !!j.battery_capacity_test_active,
          capacityMeasuring = !!j.battery_capacity_test_measuring;
        $("#capacityResult").textContent =
          capacityMah.toFixed(1) +
          " mAh · " +
          fmtCapacityTime(j.battery_capacity_test_seconds || 0);
        $("#capacityState").textContent = !j.battery_available
          ? "INA219 unavailable"
          : capacityMeasuring
            ? "Measuring discharge"
            : capacityActive
              ? "Paused · unplug USB/charger"
              : capacityMah > 0
                ? "Stopped · result saved"
                : "Capacity test not started";
        const remainingMah = Number(j.battery_remaining_mah) || 0,
          totalMah = Number(j.battery_capacity_mah) || 0,
          restState = j.battery_soc_quasi_resting
            ? " · quasi-rest correcting"
            : "",
          anchorState = j.battery_soc_full_anchored
            ? " · full anchored"
            : j.battery_soc_empty_anchored
              ? " · empty anchored"
              : "",
          recoveryState = j.battery_soc_bootstrap_voltage_rebased
            ? " · startup voltage estimate"
            : "";
        $("#socState").textContent = batteryReady
          ? remainingMah.toFixed(0) +
            " mAh remaining · " +
            totalMah.toFixed(1) +
            " mAh capacity · Coulomb + quasi-rest + anchors · " +
            flow +
            restState +
            anchorState +
            recoveryState +
            (j.battery_soc_tracking_degraded ? " · tracking degraded" : "")
          : "SoC waiting for valid measurement";
        $("#capacityStart").disabled = !j.battery_available || capacityActive;
        $("#capacityStop").disabled = !capacityActive;
        $("#capacityReset").disabled =
          !j.battery_available || (!capacityActive && capacityMah === 0);
        $("#capacityStart").textContent = capacityMah > 0 ? "Resume" : "Start";
        $("#capacityStart").classList.toggle("on", capacityMeasuring);
        const motionReady =
          !!j.motion_sensor_available && !!j.motion_sensor_valid;
        $("#motionSensor").textContent = !j.motion_sensor_available
          ? "Offline"
          : motionReady
            ? (j.motion_gesture || "steady") +
              " · " +
              Number(j.motion_roll_deg).toFixed(0) +
              "° / " +
              Number(j.motion_pitch_deg).toFixed(0) +
              "°"
            : "Calibrating";
        $("#motionEmotions").disabled = !j.motion_sensor_available;
        if (document.activeElement !== $("#motionEmotions")) {
          $("#motionEmotions").checked = !!j.motion_emotions_enabled;
        }
        if (document.activeElement !== $("#emotionMovement")) {
          $("#emotionMovement").checked = !!j.emotion_movement_enabled;
        }
        if (
          document.activeElement !== $("#duration") &&
          Number.isFinite(j.drive_duration_ms)
        ) {
          $("#duration").value = j.drive_duration_ms;
          $("#durationLabel").textContent = fmtMs(j.drive_duration_ms);
        }
        $$("[data-turn]").forEach(
          (b) => (b.disabled = cliff || j.gyro_turn_pending || active),
        );
        setRange($("#motorSpeed"), j.motor_speed, $("#motorSpeedValue"), "%");
        $("#safety").classList.toggle("show", cliff);
        $$("[data-drive]").forEach(
          (b) => (b.disabled = cliff && b.dataset.drive !== "backward"),
        );
        const oled = $("#oledState");
        oled.className = "value health " + (j.oled_available ? "good" : "bad");
        oled.querySelector("span").textContent = j.oled_available
          ? "Ready"
          : "Offline";
        setRange(
          $("#cliffThreshold"),
          j.cliff_edge_mm,
          $("#cliffValue"),
          " mm",
        );
        setRange(
          $("#speakerVolume"),
          j.speaker_volume,
          $("#speakerValue"),
          "%",
        );
        setRange(
          $("#microphoneGain"),
          j.microphone_gain,
          $("#microphoneValue"),
          "×",
        );
        setRange(
          $("#screenBrightness"),
          j.screen_brightness,
          $("#screenValue"),
          "%",
        );
        setRange(
          $("#statusLightBrightness"),
          j.status_light_brightness,
          $("#statusLightValue"),
          "%",
        );
        if (Number.isFinite(j.oled_contrast)) {
          const contrastPercent = Math.round(
            (Math.max(0, Math.min(255, j.oled_contrast)) * 100) / 255,
          );
          setRange(
            $("#oledContrast"),
            contrastPercent,
            $("#oledContrastValue"),
            "%",
          );
        }
        $("#micLevel").style.width =
          Math.max(0, Math.min(100, j.microphone_level || 0)) + "%";
        $("#micClip").textContent = j.microphone_clipping ? "CLIP" : "LIVE";
        $("#micClip").classList.toggle("on", !!j.microphone_clipping);
        $("#liveCamera").classList.toggle("on", !!j.live_camera);
        $("#liveCamera").textContent = j.live_camera
          ? idle
            ? "Stop screen preview"
            : "Preview paused"
          : "Screen preview";
        $("#cameraFlip").classList.toggle("on", !!j.camera_flipped);
        $("#wakeAction").classList.toggle("on", chatActive);
        $("#wakeLabel").textContent = chatActive ? "End chat" : "Wake";
        $("#danceAction").classList.toggle("on", !!m.sequence_active);
        $("#lightsAction").classList.toggle(
          "on",
          (j.status_light_brightness || 0) > 0,
        );
        $$("[data-emotion]").forEach((b) =>
          b.classList.toggle(
            "on",
            b.dataset.emotion === (j.emotion || "neutral"),
          ),
        );
        $("#displayFlip").classList.toggle("on", !!j.display_flipped);
        $("#oledFlip").classList.toggle("on", !!j.oled_flipped);
        renderOledConfig(j);
        $("#motion").textContent = m.faulted
          ? "Fault"
          : active
            ? (m.direction || "moving") + (m.sequence_active ? " · dance" : "")
            : "Stopped";
        $("#queue").textContent = m.queued || 0;
        $("#motorTime").textContent = fmtMs(m.remaining_ms || 0);
        $("#panicStop").classList.toggle("show", active);
        const total = m.sequence_total || 0,
          done = m.sequence_completed || 0,
          pct = total ? Math.min(100, (done * 100) / total) : 0;
        $("#danceProgress").style.width = pct + "%";
        $("#danceState").textContent = m.sequence_active
          ? done + "/" + total + " steps"
          : "Ready";
        $("#uptime").textContent = fmtTime(j.uptime_sec);
        $("#version").textContent = "v" + (j.version || "—");
        $("#ssid").textContent = j.ssid || "—";
        $("#rssi").textContent = j.rssi ? j.rssi + " dBm" : "—";
        $("#ip").textContent = j.ip || "—";
        $("#sram").textContent = fmtBytes(j.free_internal_bytes);
        $("#psram").textContent = fmtBytes(j.free_psram_bytes);
        $("#cameraHealth").textContent = j.camera_available
          ? "Ready"
          : "Offline";
        return active;
      }
      async function pollStatus() {
        if (statusPending) return;
        statusPending = true;
        let fast = false;
        try {
          const r = await fetch("/api/status", { cache: "no-store" });
          if (!r.ok) throw Error();
          fast = renderStatus(await r.json());
        } catch (e) {
          $("#online").classList.remove("ok");
          $("#online span").textContent = "Offline";
        } finally {
          statusPending = false;
          const delay =
            statusQueuedDelay !== null ? statusQueuedDelay : fast ? 300 : 1000;
          statusQueuedDelay = null;
          clearTimeout(statusTimer);
          statusTimer = setTimeout(pollStatus, delay);
        }
      }
      function queueStatus(delay = 0) {
        clearTimeout(statusTimer);
        statusTimer = null;
        if (statusPending) {
          statusQueuedDelay =
            statusQueuedDelay === null ? delay : Math.min(statusQueuedDelay, delay);
          return;
        }
        statusTimer = setTimeout(pollStatus, delay);
      }
      async function setAsrProvider(provider) {
        if (asrSaving) return;
        asrSaving = true;
        asrEditing = true;
        const select = $("#asrProvider"),
          keyInput = $("#geminiApiKey"),
          saveButton = $("#saveAsr"),
          clearButton = $("#clearGeminiKey");
        $("#asrStatus").textContent = "Switching ASR…";
        select.disabled = true;
        keyInput.disabled = true;
        saveButton.disabled = true;
        clearButton.disabled = true;
        try {
          const response = await fetch("/api/asr", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ provider }),
          });
          const result = await response.json();
          asrEditing = false;
          if (result.provider) applyAsrStatus(result, true);
          if (!response.ok || !result.ok) {
            throw Error(result.message || "Could not change ASR provider");
          }
          notify(
            result.provider === "gemini"
              ? "Gemini ASR active from next listening turn"
              : "Xiaozhi ASR active from next listening turn",
          );
        } catch (error) {
          asrEditing = false;
          notify(error.message || "Could not change ASR provider");
        } finally {
          asrSaving = false;
          select.disabled = false;
          keyInput.disabled = false;
          saveButton.disabled = false;
          queueStatus(0);
        }
      }
      async function saveGeminiApiKey() {
        if (asrSaving) return;
        const keyInput = $("#geminiApiKey"),
          key = keyInput.value.trim();
        if (!key) {
          notify("Enter a Gemini API key first");
          return;
        }
        asrSaving = true;
        asrEditing = true;
        const select = $("#asrProvider"),
          saveButton = $("#saveAsr"),
          clearButton = $("#clearGeminiKey");
        $("#asrStatus").textContent = "Saving Gemini key…";
        select.disabled = true;
        keyInput.disabled = true;
        saveButton.disabled = true;
        clearButton.disabled = true;
        try {
          const response = await fetch("/api/asr", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ api_key: key }),
          });
          const result = await response.json();
          asrEditing = false;
          if (result.provider) applyAsrStatus(result, true);
          if (!response.ok || !result.ok) {
            throw Error(result.message || "Could not save Gemini API key");
          }
          keyInput.value = "";
          notify(result.message || "Gemini API key saved");
        } catch (error) {
          asrEditing = false;
          notify(error.message || "Could not save Gemini API key");
        } finally {
          asrSaving = false;
          select.disabled = false;
          keyInput.disabled = false;
          saveButton.disabled = false;
          queueStatus(0);
        }
      }
      async function clearGeminiApiKey() {
        if (asrSaving || !confirm("Clear the saved Gemini API key?")) return;
        asrSaving = true;
        const select = $("#asrProvider"),
          keyInput = $("#geminiApiKey"),
          saveButton = $("#saveAsr"),
          clearButton = $("#clearGeminiKey");
        $("#asrStatus").textContent = "Clearing Gemini key…";
        select.disabled = true;
        keyInput.disabled = true;
        saveButton.disabled = true;
        clearButton.disabled = true;
        try {
          const response = await fetch("/api/asr", { method: "DELETE" });
          const result = await response.json();
          if (result.provider) {
            asrEditing = false;
            applyAsrStatus(result, true);
          }
          if (!response.ok || !result.ok) {
            throw Error(result.message || "Could not clear Gemini API key");
          }
          $("#geminiApiKey").value = "";
          notify(result.message || "Gemini API key cleared");
        } catch (error) {
          asrEditing = false;
          notify(error.message || "Could not clear Gemini API key");
        } finally {
          asrSaving = false;
          select.disabled = false;
          keyInput.disabled = false;
          saveButton.disabled = false;
          queueStatus(0);
        }
      }
      $("#asrProvider").onchange = (event) =>
        setAsrProvider(event.target.value);
      $("#geminiApiKey").oninput = () => (asrEditing = true);
      $("#saveAsr").onclick = saveGeminiApiKey;
      $("#clearGeminiKey").onclick = clearGeminiApiKey;
      const duration = $("#duration");
      duration.oninput = () =>
        ($("#durationLabel").textContent = fmtMs(+duration.value));
      duration.onchange = () =>
        action("drive_duration", { value: +duration.value });
      const motorSpeed = $("#motorSpeed");
      motorSpeed.oninput = () =>
        ($("#motorSpeedValue").textContent = motorSpeed.value + "%");
      motorSpeed.onchange = () =>
        action("motor_speed", { value: +motorSpeed.value });
      const oledContrast = $("#oledContrast");
      oledContrast.oninput = () =>
        ($("#oledContrastValue").textContent = oledContrast.value + "%");
      oledContrast.onchange = () =>
        action("oled_contrast", {
          value: Math.round((+oledContrast.value * 255) / 100),
        });
      [
        ["speakerVolume", "speakerValue", "speaker_volume", "%"],
        ["microphoneGain", "microphoneValue", "microphone_gain", "×"],
        ["screenBrightness", "screenValue", "screen_brightness", "%"],
        [
          "statusLightBrightness",
          "statusLightValue",
          "status_light_brightness",
          "%",
        ],
        ["cliffThreshold", "cliffValue", "cliff_threshold", " mm"],
      ].forEach(([i, l, a, s]) => {
        const input = $("#" + i),
          label = $("#" + l);
        input.oninput = () => (label.textContent = input.value + s);
        input.onchange = () => action(a, { value: +input.value });
      });
      $("#motionEmotions").onchange = (e) =>
        action("motion_emotions", { value: e.target.checked ? 1 : 0 });
      $("#emotionMovement").onchange = (e) =>
        action("emotion_movement", { value: e.target.checked ? 1 : 0 });
      $("#capacityStart").onclick = () => action("battery_capacity_start");
      $("#capacityStop").onclick = () => action("battery_capacity_stop");
      $("#capacityReset").onclick = () => {
        if (confirm("Reset the saved battery capacity measurement?"))
          action("battery_capacity_reset");
      };
      $$("[data-drive]").forEach((btn) => {
        btn.onclick = () => {
          if (!btn.disabled)
            action(btn.dataset.drive, { duration_ms: +duration.value });
        };
        btn.oncontextmenu = (e) => e.preventDefault();
      });
      $$("[data-turn]").forEach((btn) => {
        btn.onclick = () => {
          if (!btn.disabled)
            action("turn_relative", { value: +btn.dataset.turn });
        };
        btn.oncontextmenu = (e) => e.preventDefault();
      });
      function emergencyStop() {
        $$("[data-drive].active").forEach((b) => b.classList.remove("active"));
        fetch("/api/action", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: '{"action":"stop"}',
          keepalive: true,
        }).catch(() => {});
        queueStatus(120);
      }
      window.addEventListener("blur", emergencyStop);
      window.addEventListener("pagehide", emergencyStop);
      document.addEventListener("visibilitychange", () => {
        if (document.hidden) emergencyStop();
      });
      $$("[data-stop]").forEach((b) => (b.onclick = () => action("stop")));
      $$("[data-action]").forEach(
        (btn) =>
          (btn.onclick = () => {
            const name = btn.dataset.action;
            if (
              name === "wifi_config" &&
              !confirm("Open Wi-Fi setup mode? This page will disconnect.")
            )
              return;
            if (
              name === "reboot" &&
              !confirm(
                "Reboot the robot now? Motors will stop and this page will disconnect briefly.",
              )
            )
              return;
            action(name);
          }),
      );
      $$("[data-emotion]").forEach(
        (btn) =>
          (btn.onclick = () =>
            action("emotion", {
              text: btn.dataset.emotion,
              duration_ms: 5000,
            })),
      );
      function clearSnapshot() {
        if (snapshotUrl) {
          URL.revokeObjectURL(snapshotUrl);
          snapshotUrl = "";
        }
        const img = $("#snapshotImage");
        img.removeAttribute("src");
        $("#snapshot").classList.remove("has-image");
      }
      async function captureSnapshot(silent = false) {
        if (snapshotPending) return;
        snapshotPending = true;
        const button = $("#takeSnapshot"),
          meta = $("#snapshotMeta");
        button.disabled = true;
        if (!silent) {
          button.textContent = "Capturing…";
          meta.textContent = "Waiting for camera";
        }
        try {
          const r = await fetch("/api/camera/snapshot?ts=" + Date.now(), {
            cache: "no-store",
          });
          if (!r.ok) {
            let msg = "Capture failed";
            try {
              msg = (await r.json()).message || msg;
            } catch (_) {}
            throw Error(msg);
          }
          const blob = await r.blob();
          if (silent && !browserLive) return;
          if (snapshotUrl) URL.revokeObjectURL(snapshotUrl);
          snapshotUrl = URL.createObjectURL(blob);
          const img = $("#snapshotImage");
          img.onload = () => {
            meta.textContent =
              img.naturalWidth +
              " × " +
              img.naturalHeight +
              " · " +
              Math.round(blob.size / 1024) +
              " KB";
          };
          img.src = snapshotUrl;
          $("#snapshot").classList.add("has-image");
        } catch (e) {
          meta.textContent = e.message;
          if (!silent) notify(e.message);
          stopBrowserLive(e.message, true);
        } finally {
          snapshotPending = false;
          button.disabled = false;
          button.textContent = "Take photo";
          if (browserLive)
            liveTimer = setTimeout(() => captureSnapshot(true), 850);
        }
      }
      function stopBrowserLive(message, clear = false) {
        browserLive = false;
        clearTimeout(liveTimer);
        $("#browserLive").classList.remove("on");
        $("#browserLive").textContent = "Live";
        if (clear) clearSnapshot();
        if (message) $("#snapshotMeta").textContent = message;
      }
      $("#takeSnapshot").onclick = () => captureSnapshot(false);
      $("#browserLive").onclick = () => {
        if (browserLive) {
          stopBrowserLive("Live preview stopped", true);
          return;
        }
        if (lastState !== "idle") {
          notify("Browser live view is available only while Idle");
          return;
        }
        browserLive = true;
        $("#browserLive").classList.add("on");
        $("#browserLive").textContent = "Stop live";
        captureSnapshot(true);
      };
      const logOutput = $("#logOutput"),
        pauseLog = $("#pauseLog"),
        logState = $("#logState"),
        autoScroll = $("#autoScroll");
      function stripAnsi(text) {
        return text.replace(/\u001b\[[0-9;]*[A-Za-z]/g, "");
      }
      function logLevel(line) {
        if (/(^|\s)E \(|\*\*\*ERROR|Guru Meditation|panic/i.test(line))
          return "error";
        if (/(^|\s)W \(/.test(line)) return "warn";
        return "info";
      }
      function hasLogSelection() {
        const sel = window.getSelection();
        if (!sel || sel.isCollapsed || !sel.rangeCount) return false;
        return (
          logOutput.contains(sel.anchorNode) ||
          logOutput.contains(sel.focusNode)
        );
      }
      function renderLogs(force = false) {
        if (!force && hasLogSelection()) {
          logRenderPending = true;
          return;
        }
        const q = $("#logSearch").value.toLowerCase(),
          filter = $("#logFilter").value,
          lines = allLogs.split("\n"),
          visible = lines.filter(
            (line) =>
              (filter === "all" || logLevel(line) === filter) &&
              (!q || line.toLowerCase().includes(q)),
          ),
          pinned =
            logOutput.scrollHeight -
              logOutput.scrollTop -
              logOutput.clientHeight <
            30;
        logOutput.textContent = visible.join("\n") || "No matching logs";
        const errors = lines.filter((l) => logLevel(l) === "error").length,
          badge = $("#errorBadge");
        badge.textContent = errors;
        badge.classList.toggle("show", errors > 0);
        $("#logSize").textContent =
          Math.round(allLogs.length / 1024) + " KB cached · 16 KB device";
        if (autoScroll.checked && pinned)
          logOutput.scrollTop = logOutput.scrollHeight;
        logRenderPending = false;
      }
      async function fetchLogs() {
        if (logPaused || logPending) return;
        logPending = true;
        try {
          const r = await fetch("/api/log?since=" + logCursor, {
            cache: "no-store",
          });
          if (!r.ok) throw Error();
          const next = Number(r.headers.get("X-Log-Cursor") || logCursor),
            reset = r.headers.get("X-Log-Reset") === "1",
            text = stripAnsi(await r.text()),
            resetView = !logStarted || reset;
          if (resetView) {
            allLogs = "";
            logStarted = true;
          }
          if (text) allLogs += text;
          if (allLogs.length > 65536) allLogs = allLogs.slice(-65536);
          logCursor = next;
          logState.textContent = "Live system log";
          const changed = resetView || !!text;
          if (changed) {
            try {
              localStorage.setItem("xiaozhiLogs", allLogs);
            } catch (_) {}
          }
          if (changed || logRenderPending) renderLogs();
        } catch (e) {
          logState.textContent = "Log disconnected";
        } finally {
          logPending = false;
        }
      }
      pauseLog.onclick = () => {
        logPaused = !logPaused;
        pauseLog.textContent = logPaused ? "Resume" : "Pause";
        logState.textContent = logPaused ? "Log paused" : "Live system log";
        if (!logPaused) fetchLogs();
      };
      $("#clearLog").onclick = () => {
        allLogs = "";
        logStarted = true;
        logRenderPending = false;
        try {
          localStorage.removeItem("xiaozhiLogs");
        } catch (_) {}
        renderLogs(true);
      };
      $("#downloadLog").onclick = () => {
        const a = document.createElement("a"),
          blob = new Blob([allLogs], { type: "text/plain" });
        a.href = URL.createObjectURL(blob);
        a.download =
          "xiaozhi-" + new Date().toISOString().replaceAll(":", "-") + ".log";
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      };
      $("#logSearch").oninput = () => renderLogs(true);
      $("#logFilter").onchange = () => renderLogs(true);
      try {
        allLogs = localStorage.getItem("xiaozhiLogs") || "";
        if (allLogs) {
          logStarted = true;
          renderLogs(true);
        }
      } catch (_) {}
      pollStatus();
      fetchLogs();
      setInterval(fetchLogs, 650);
