# Xiaozhi Desk Robot — Phase 6 Agent Handoff

Repo: `hudrucan/xiaozhi-desk-robot`

## Purpose

Continue the Gemini ASR integration from the latest repository state.

**Do not treat this document as newer than the repository.** Start by inspecting the latest local/Git state, recent commits, and relevant source files. Keep changes minimal and do not refactor unrelated code.

The user builds and flashes the ESP32-S3 hardware themselves unless explicitly asked otherwise.

---

## Architecture

Xiaozhi remains responsible for:

- LLM
- memory
- MCP tools
- vision
- emotions
- TTS
- conversation/session handling

Gemini is **ASR only**.

Current Gemini transcription model:

```text
models/gemini-3.5-transcribe-live
```

Language:

```text
vi-VN
```

Mode:

```text
SMART
```

Processed microphone audio routed to Gemini is:

- mono
- 16 kHz PCM
- AFE-processed
- tapped before Opus encoding

The original Xiaozhi ASR path must remain intact.

---

## Completed phases

### Phase 1 — integration inspection
DONE.

Important findings:

- AFE processed output is mono 16 kHz.
- Existing VAD callback already exists.
- Existing text-chat route exists in `Application`.
- WebSocket callbacks receive fully assembled messages.
- NVS key names have a 15-character limit.

### Phase 2 — ASR settings / NVS
DONE.

Existing concepts include:

```cpp
AsrProvider::kXiaozhi
AsrProvider::kGemini
```

Settings include provider, Gemini API key, language, mode, and vocabulary.

Important behavior:

- default provider remains Xiaozhi
- Gemini cannot be selected without a configured key
- key is never returned by Web API
- key must never be logged
- provider configuration is snapshotted per listening turn

### Phase 3 — Gemini transcription client
DONE.

`GeminiTranscribeClient` already provides:

- worker task
- TLS/WebSocket connection
- setup request
- setup timeout
- bounded PCM queue
- drop-oldest PCM policy
- `PushPcm()`
- `SendAudioStreamEnd()`
- final-transcript callback
- error callback
- state callback
- stale/session-safe teardown
- API-key log protection

The client has been verified on real hardware.

### Phase 4 — processed PCM routing
DONE.

`AudioService` routes AFE-processed PCM according to ASR provider.

Gemini PCM handoff is bounded/non-blocking.

The original Xiaozhi Opus path must remain unchanged.

### Phase 5 — Application lifecycle
DONE for standalone Gemini ASR validation.

Verified runtime sequence:

```text
State: connecting -> listening
Application: ASR turn start provider=gemini configured=1
GeminiASR: Gemini ASR connecting
GeminiASR: Gemini ASR connected
GeminiASR: Gemini ASR setup complete
Application: Gemini ASR ready; voice processing enabled
GeminiASR: Gemini ASR final transcript received
Application: Gemini ASR final received bytes=...
```

Real hardware confirmed:

- provider load works
- API key works
- TLS works
- WebSocket works
- Gemini returns JSON inside a WebSocket frame marked `binary`
- parser correctly treats valid UTF-8 JSON as JSON regardless of WS opcode
- `setupComplete` works
- Ready callback works
- PCM reaches Gemini
- final transcript reaches `Application`
- teardown works

### Phase 5.5 — Web UI/API
Essentially DONE, but inspect latest code before starting Phase 6.

Frontend behavior is intended to be:

- provider dropdown changes provider immediately
- `Save Gemini key` only saves the API key
- provider change applies from the next listening turn
- selecting Gemini without a saved key is rejected and UI reverts
- `/api/status` polling must never create duplicate loops

The duplicate `/api/status` polling race has already been fixed with serialized polling (`statusPending` plus deferred refresh).

---

# Mandatory pre-Phase-6 audit

Before implementing Phase 6, inspect latest code for the following two known cleanup items.

## 1. `/api/asr` partial-update compatibility

Frontend sends provider and API-key updates independently.

Provider change:

```json
{"provider":"gemini"}
```

API-key save:

```json
{"api_key":"..."}
```

`RobotWebControlServer::HandleSaveAsrConfig()` must therefore support:

- provider only
- API key only
- both
- reject neither

Required semantics:

### Provider only

Update provider and leave saved key unchanged.

If selecting Gemini without a configured key:

- reject request
- keep effective persisted provider
- return effective provider in response

### API key only

Update Gemini key only.

Do **not** change provider.

### Both

Valid.

### Neither

Return `400`.

Never return or log the API key.

If latest backend still requires `provider` on every POST, fix this before Phase 6.

Suggested commit:

```text
fix(asr): support independent ASR config updates
```

---

## 2. Remove temporary first-response diagnostic

If still present, remove the temporary hardware-debug code similar to:

```text
Gemini first response binary=...
first8=...
preview=...
```

Hardware has already confirmed that Gemini can return JSON in a WS frame with `binary=true`.

Remove only the temporary diagnostic.

**Do not revert the parser behavior that accepts valid UTF-8 JSON regardless of WebSocket text/binary opcode.**

Suggested commit:

```text
chore(asr): remove verified Gemini response diagnostic
```

---

# Phase 6 goal

Turn Gemini ASR from a standalone transcription test into the ASR frontend of the **existing Xiaozhi conversation**.

The final Gemini transcript must enter the already-open Xiaozhi session without ending that conversation.

---

# Critical conversation invariant

A Gemini transcription result is **not** the end of the conversation.

One wake should start one Xiaozhi conversation.

Correct multi-turn lifecycle:

```text
wake
  -> Xiaozhi conversation/audio channel opens
  -> listening
  -> Gemini ASR turn starts
  -> user speaks
  -> speech ends
  -> Gemini final transcript
  -> stop Gemini ASR turn only
  -> send transcript into CURRENT Xiaozhi conversation
  -> Xiaozhi LLM / MCP / emotion / TTS
  -> speaking
  -> response finishes
  -> listening
  -> fresh Gemini ASR turn
  -> next user sentence
```

The user must be able to speak sentence B **without another wake word**.

Only explicit End Chat, normal session termination, disconnect, fatal error, reboot/reset, etc. should terminate the Xiaozhi conversation and return to Idle.

---

# Current Phase-5 behavior that must change

Current standalone validation behavior is approximately:

```cpp
StopGeminiAsrTurn();

if (GetDeviceState() == kDeviceStateListening) {
    SetDeviceState(kDeviceStateIdle);
}
```

That was acceptable for Phase-5 hardware testing.

It is **wrong for Phase 6**.

Do not keep:

```text
Gemini final
-> listening -> idle
-> start a new text-chat conversation
```

That would break multi-turn context after every spoken sentence.

---

# Phase 6A — inspect the existing typed-chat lifecycle first

Do not implement the Gemini bridge until the current text-chat path is understood.

Trace the actual latest call graph around:

```text
RobotWebControlServer /api/chat
-> Application::SubmitTextChat(...)
-> RunTextChat(...) or current equivalent
-> protocol send
-> Xiaozhi response handling
-> TTS / speaking
-> next listening state
```

Inspect at least:

```text
SubmitTextChat
RunTextChat
text_chat_pending_
text_chat_resume_listening_
HandleStateChangedEvent
SetDeviceState
protocol_->OpenAudioChannel
protocol_->CloseAudioChannel
protocol text send
speaking -> listening behavior
conversation UI/history integration
```

Important existing fact:

The public typed-chat path was designed to support **idle-origin web text chat** and may:

- pre-arm listening
- open/reuse the channel
- prime transport
- perform lifecycle setup intended for a new typed turn

Therefore:

**Do not blindly call the public `SubmitTextChat()` from Gemini final if it bootstraps a new conversation or requires an Idle transition.**

Identify the lowest reusable internal operation that sends user text into Xiaozhi while the **current conversation is already active**.

A minimal refactor may be appropriate, for example conceptually:

```cpp
SubmitTextChat(text);                 // Web UI / idle-origin semantics
SubmitActiveConversationText(text);   // Gemini / already-active session
```

Names are illustrative only. Follow the existing codebase conventions.

Share the protocol text-send core where possible, but preserve the different lifecycle semantics.

If the lifecycle is ambiguous, stop after inspection and report the exact call graph before modifying behavior.

---

# Phase 6B — local VAD and utterance completion

The project already has an AFE/VAD signal.

Reuse it.

Do **not** add a second VAD implementation.

For an active Gemini ASR turn:

1. Wait for Gemini Ready/Streaming.
2. Track whether real speech has started.
3. Ignore initial silence.
4. After speech has started, detect end-of-speech using the existing VAD path.
5. Stop/detach further PCM capture for that utterance.
6. Request `GeminiTranscribeClient::SendAudioStreamEnd()` exactly once.
7. Wait for final transcript.

Do not repeatedly send `audioStreamEnd`.

Do not perform heavy work or WebSocket operations inside the realtime audio callback.

If VAD is emitted from the audio/AFE task, schedule/post a lightweight event to `Application` or the main task and perform lifecycle work there.

Preserve turn IDs and stale-callback guards.

---

# Important race: final may arrive before local VAD end

Gemini itself can detect a pause and emit finalized transcription before the local VAD path sends `audioStreamEnd`.

Both orders must be safe:

```text
VAD end
-> audioStreamEnd
-> final
```

and:

```text
Gemini final
-> later stale local VAD-end event
```

Exactly one finalized transcript may be accepted and bridged per Gemini turn.

Existing client/application mechanisms already include:

- `final_callback_sent_`
- turn IDs
- stale callback rejection
- `SendAudioStreamEnd()`
- `kEnding`

Reuse them instead of creating a second lifecycle system.

---

# Phase 6C — final transcript timeout

After `audioStreamEnd` has actually been sent, do not wait forever.

Add a bounded timeout for the final transcript.

Transport-level timeout ownership should preferably live in `GeminiTranscribeClient`, because it already owns:

- WebSocket
- worker loop
- `kEnding`
- final reception
- transport errors

Conceptual behavior:

```text
audioStreamEnd sent
-> state kEnding
-> start final deadline
-> final before deadline -> normal
-> deadline exceeded -> error callback
```

Do not block `Application` or the main task.

Do not confuse this with the existing setup timeout.

Use a named constant rather than a scattered magic number.

No infinite retry.

---

# Phase 6D — bridge final transcript into the active Xiaozhi session

This is the most important part of Phase 6.

Normal successful Gemini final must **not**:

- call `SetDeviceState(kDeviceStateIdle)`
- close the Xiaozhi audio/conversation channel
- send Xiaozhi stop-listening merely because Gemini finalized
- run wake bootstrap again
- create a second conversation
- feed Gemini audio into Xiaozhi ASR
- enable Xiaozhi microphone Opus ASR while Gemini is the provider

Desired behavior:

```text
Gemini final callback
-> validate turn ID / active provider / state
-> preserve transcript
-> stop Gemini PCM capture/session
-> keep Xiaozhi conversation/channel alive
-> inject transcript into current Xiaozhi conversation
-> wait for normal Xiaozhi response lifecycle
```

The final transcript should semantically mean:

> The user said this text inside the conversation that is already open.

While waiting for the Xiaozhi response:

- Gemini/microphone capture stays disabled
- stale Gemini callbacks are ignored

Expected normal state flow:

```text
listening
-> speaking
-> listening
```

There must be **no `idle` state between Gemini final and Xiaozhi response** in a normal successful turn.

If the current state machine makes this impossible without an intermediate state, explain the constraint before introducing a workaround. Do not use Idle as a shortcut.

---

# Phase 6E — next Gemini turn

After Xiaozhi TTS finishes and the conversation resumes Listening:

- begin a fresh ASR turn
- use the correct provider snapshot for that new turn
- if provider is still Gemini:
  - create/start new Gemini session
  - wait for setupComplete/Ready
  - attach PCM
- if provider was changed to Xiaozhi:
  - use the original Xiaozhi ASR path

Changing the configured provider must not mutate an ASR turn already in progress.

It may apply from the next listening turn according to the existing per-turn snapshot design.

Note:

`active_asr_provider_ = kXiaozhi` inside Gemini teardown must not be interpreted as changing the user's persisted ASR provider. It represents the currently active runtime route/client, not stored configuration.

---

# Gemini transcription semantics

For the current live transcription API:

```text
interimInputTranscription
```

is speculative/partial.

```text
inputTranscription
```

is finalized transcription.

Only finalized `inputTranscription` should be sent to Xiaozhi as the user's message.

Do not bridge interim transcription into the conversation.

For client-side end-of-audio, use the existing:

```text
realtimeInput.audioStreamEnd = true
```

Do not change model, language, or SMART mode as part of Phase 6.

---

# Error and cancellation behavior

Preserve existing behavior for:

- explicit End Chat
- wake/cancel behavior
- network disconnect
- reboot
- protocol reset
- Gemini connection/setup error
- stale callbacks

For explicit conversation termination it is correct to:

```text
stop Gemini
close Xiaozhi channel
return Idle
```

For a normal successful Gemini transcript it is **not**.

A final-transcript timeout/error must exit cleanly and must not leave the robot permanently stuck in Listening.

Do not automatically persist provider fallback to Xiaozhi for generic Gemini/network errors.

A network problem does not imply an invalid API key.

---

# Known non-blocking issue

A TLS receive error around:

```text
mbedtls_ssl_fetch_input error=76
read error :-0x004C
```

has been observed during intentional Gemini WebSocket teardown **after a successful final transcript**.

Current evidence indicates teardown noise rather than functional failure.

Do not broaden or suppress TLS errors during Phase 6 unless new runtime evidence shows a real regression.

---

# Resource constraint

Real hardware has previously shown approximately:

```text
free SRAM: ~36 KB
minimum SRAM: ~19 KB
```

Phase 6 may briefly overlap Gemini teardown with Xiaozhi text/LLM/TTS activity.

Monitor memory during hardware testing.

Avoid:

- unnecessary transcript copies
- larger PCM queues without evidence
- new large buffers
- blocking waits

---

# Test gates

Do not treat Phase 6 as complete after a single successful transcription.

## Gate 1 — static/build

Verify:

- compile succeeds
- original Xiaozhi provider path remains unchanged
- API key is never logged
- no blocking network work was added to audio callbacks
- no unrelated refactor

## Gate 2 — Gemini utterance finalization

Expected sequence approximately:

```text
ASR turn start provider=gemini
Gemini ASR connected
Gemini ASR setup complete
Gemini ASR ready
[speech begins]
[VAD speech end]
Gemini ASR audioStreamEnd sent
Gemini ASR final transcript received
```

Requirements:

- exactly one `audioStreamEnd`
- exactly one final per turn

Also test the race where final arrives before local VAD-end handling.

## Gate 3 — single-turn conversation bridge

Wake once and speak one sentence.

Expected:

```text
listening
Gemini final
text injected into current Xiaozhi session
speaking
```

There must be **no**:

```text
listening -> idle
```

between Gemini final and speaking.

MCP, emotion, and TTS behavior should match typed text chat.

## Gate 4 — true multi-turn

Wake **once**.

Speak sentence A.

Wait for Xiaozhi response.

Robot should return to Listening and start a fresh Gemini turn.

Then speak sentence B **without another wake word**.

Expected state sequence:

```text
idle
-> connecting
-> listening
-> speaking
-> listening
-> speaking
-> listening
```

There must be no Idle between A and B.

Sentence B must remain in the same Xiaozhi conversation context.

## Gate 5 — explicit end

After a response, while Listening, explicitly End Chat.

Only then should normal session termination occur:

```text
listening -> idle
```

and the Xiaozhi conversation/channel should close.

## Gate 6 — Xiaozhi ASR regression

Switch provider back to Xiaozhi.

Verify the original ASR path still functions normally.

---

# Commit strategy

Keep commits small and bisectable.

Suggested order if applicable:

```text
fix(asr): support independent ASR config updates
```

```text
chore(asr): remove verified Gemini response diagnostic
```

```text
feat(asr): finalize Gemini turns with local VAD
```

```text
feat(asr): bridge Gemini transcripts into active conversation
```

Do not mix unrelated cleanup into these commits.

---

# What to report after implementation

Stop after implementation/build review and report:

1. exact files changed
2. exact state-flow changes
3. how VAD/end-of-speech is wired
4. how `audioStreamEnd` is made exactly-once
5. final timeout behavior
6. how transcript is injected into the **existing** Xiaozhi conversation
7. how speaking returns to a fresh Gemini listening turn
8. any remaining race conditions
9. SRAM/resource concerns
10. exact hardware smoke-test checklist

Do not flash hardware unless explicitly asked.

---

# One-sentence architectural rule

**Gemini final ends one ASR utterance, not the Xiaozhi conversation.**
