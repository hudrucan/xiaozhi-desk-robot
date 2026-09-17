function resizeChatInput() {
  const input = $("#chatInput");
  input.style.height = "36px";
  const height = Math.min(68, input.scrollHeight);
  input.style.height = height + "px";
  input.style.overflowY = input.scrollHeight > 68 ? "auto" : "hidden";
}

function updateChatInput() {
  const input = $("#chatInput");
  const count = Array.from(input.value).length;
  const empty = !input.value.trim();
  const busy = ["Sending", "Waiting", "Speaking"].includes(chatBackendState);
  resizeChatInput();
  $("#chatCount").textContent = count + " / " + CHAT_MAX_CHARS;
  $("#chatCount").classList.toggle("over", count > CHAT_MAX_CHARS);
  $("#chatSend").disabled = chatSubmitting || busy || empty || count > CHAT_MAX_CHARS;
}

function renderConversation(conversation) {
  const state = String(conversation.state || "Ready");
  const messages = Array.isArray(conversation.messages) ? conversation.messages : [];
  const error = String(conversation.error || "");
  chatBackendState = state;
  const stateElement = $("#chatState");
  stateElement.textContent = state;
  stateElement.classList.toggle("error", state === "Error");
  const errorElement = $("#chatError");
  errorElement.textContent = error;
  errorElement.classList.toggle("show", state === "Error" && !!error);

  const signature = JSON.stringify(messages);
  if (signature !== conversationSignature) {
    conversationSignature = signature;
    const history = $("#conversationHistory");
    const pinned = history.scrollHeight - history.scrollTop - history.clientHeight < 30;
    history.innerHTML = messages.length
      ? messages.map((message) =>
        '<div class="chat-row ' + (message.role === "user" ? "user" : "assistant") + '">' +
        '<span class="chat-role">' + (message.role === "user" ? "You" : "Xiaozhi") +
        '</span><div class="chat-bubble">' + escapeHtml(message.text || "") + "</div></div>",
      ).join("")
      : '<div class="conversation-empty">Type or speak to chat with Xiaozhi</div>';
    if (pinned) history.scrollTop = history.scrollHeight;
  }
  updateChatInput();
}

async function submitChat() {
  const input = $("#chatInput");
  const text = input.value;
  const count = Array.from(text).length;
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
    if (!response.ok || !result.ok) throw Error(result.message || "Message rejected");
    input.value = "";
    updateChatInput();
    queueDomains(["chat", "core"], 40);
  } catch (error) {
    chatBackendState = "Error";
    $("#chatState").textContent = "Error";
    $("#chatState").classList.add("error");
    const errorElement = $("#chatError");
    errorElement.textContent = error.message || "Robot is offline";
    errorElement.classList.add("show");
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
    if (!response.ok || !result.ok) throw Error(result.message || "Clear failed");
    conversationSignature = "";
    queueDomains(["chat"], 0);
  } catch (error) {
    notify(error.message || "Robot is offline");
  } finally {
    button.disabled = false;
  }
}

function applyAsrStatus(asr, forceProvider = false) {
  if ((forceProvider || !asrEditing) && ["xiaozhi", "gemini"].includes(asr.provider)) {
    $("#asrProvider").value = asr.provider;
  }
  const label = asr.provider === "gemini" ? "Gemini active" : "Xiaozhi active";
  $("#asrStatus").textContent = label +
    (asr.gemini_configured ? " · Gemini key saved" : " · Gemini key not set");
  $("#geminiApiKey").placeholder = asr.gemini_configured
    ? "Leave blank to keep saved key"
    : "Enter Gemini API key";
  $("#clearGeminiKey").disabled = asrSaving || !asr.gemini_configured;
}

function setAsrControlsDisabled(disabled) {
  $("#asrProvider").disabled = disabled;
  $("#geminiApiKey").disabled = disabled;
  $("#saveAsr").disabled = disabled;
  $("#clearGeminiKey").disabled = disabled;
}

async function setAsrProvider(provider) {
  if (asrSaving) return;
  asrSaving = true;
  asrEditing = true;
  $("#asrStatus").textContent = "Switching ASR…";
  setAsrControlsDisabled(true);
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
    notify(result.provider === "gemini"
      ? "Gemini ASR active from next listening turn"
      : "Xiaozhi ASR active from next listening turn");
  } catch (error) {
    asrEditing = false;
    notify(error.message || "Could not change ASR provider");
  } finally {
    asrSaving = false;
    setAsrControlsDisabled(false);
    queueDomains(["asr"], 0);
  }
}

async function saveGeminiApiKey() {
  if (asrSaving) return;
  const keyInput = $("#geminiApiKey");
  const apiKey = keyInput.value.trim();
  if (!apiKey) {
    notify("Enter a Gemini API key first");
    return;
  }
  asrSaving = true;
  asrEditing = true;
  $("#asrStatus").textContent = "Saving Gemini key…";
  setAsrControlsDisabled(true);
  try {
    const response = await fetch("/api/asr", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ api_key: apiKey }),
    });
    const result = await response.json();
    asrEditing = false;
    if (result.provider) applyAsrStatus(result, true);
    if (!response.ok || !result.ok) throw Error(result.message || "Could not save Gemini API key");
    keyInput.value = "";
    notify(result.message || "Gemini API key saved");
  } catch (error) {
    asrEditing = false;
    notify(error.message || "Could not save Gemini API key");
  } finally {
    asrSaving = false;
    setAsrControlsDisabled(false);
    queueDomains(["asr"], 0);
  }
}

async function clearGeminiApiKey() {
  if (asrSaving || !confirm("Clear the saved Gemini API key?")) return;
  asrSaving = true;
  $("#asrStatus").textContent = "Clearing Gemini key…";
  setAsrControlsDisabled(true);
  try {
    const response = await fetch("/api/asr", { method: "DELETE" });
    const result = await response.json();
    if (result.provider) {
      asrEditing = false;
      applyAsrStatus(result, true);
    }
    if (!response.ok || !result.ok) throw Error(result.message || "Could not clear Gemini API key");
    $("#geminiApiKey").value = "";
    notify(result.message || "Gemini API key cleared");
  } catch (error) {
    asrEditing = false;
    notify(error.message || "Could not clear Gemini API key");
  } finally {
    asrSaving = false;
    setAsrControlsDisabled(false);
    queueDomains(["asr"], 0);
  }
}
