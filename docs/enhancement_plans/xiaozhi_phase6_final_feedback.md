# Xiaozhi Desk Robot — Phase 6 Final Feedback

Repo: `hudrucan/xiaozhi-desk-robot`

This feedback is based on the latest real-hardware Phase 6 tests.

The core Gemini -> Xiaozhi multi-turn bridge is working. Do **not** redesign it. Focus on the remaining lifecycle semantics and timeout behavior described below.

---

# Current runtime status

The important happy-path behavior is now confirmed on hardware.

Observed sequence:

```text
speaking -> listening
Gemini ASR turn starts
Gemini connects
setupComplete
Gemini Ready
user speaks
Gemini final transcript received
TextChat MCP bridge armed
original transcript substituted into bridge
Xiaozhi processes request
listening -> speaking
TextChat completed
speaking -> listening
new Gemini ASR turn starts
```

Most importantly, after a successful Gemini final:

```text
Gemini final
-> current Xiaozhi conversation continues
-> speaking
-> listening
-> new Gemini ASR turn
```

There is no longer an unwanted:

```text
Gemini final
-> idle
```

on the successful path.

Multi-turn conversation context has also been confirmed in practice: a follow-up utterance referring to the previous answer was understood without another wake word.

Therefore:

```text
Phase 6 active-conversation bridge: PASS
Phase 6 multi-turn continuation: PASS
```

Do not replace or redesign the bridge unless a concrete bug requires it.

---

# Deferred UI cleanup

The Conversation UI currently exposes internal control/MCP text such as bridge/tool traces.

This is known.

Do **not** prioritize or redesign the Phase 6 bridge for this issue right now.

Treat UI/tool-trace cleanup as a later polish task.

---

# Remaining issue 1 — "Listening" is announced before Gemini can actually listen

This is currently the most obvious UX/state-semantics issue.

Example observed timing:

```text
183073  State: speaking -> listening
183273  Gemini ASR connecting
184343  Gemini ASR connected
184793  Gemini ASR setup complete
184793  Gemini ASR ready; voice processing enabled
```

Effective gap:

```text
speaking -> listening          t = 183073
Gemini actually ready          t = 184793

deaf / not-really-listening window ~= 1720 ms
```

During this period the system/UI already says `listening`, but Gemini is still connecting/setup and PCM is not yet actually accepted by the ASR path.

This creates a real UX bug:

```text
status says Listening
but user speech may still be missed
```

## Required semantic invariant

For Gemini provider, user-visible `Listening` should mean:

```text
Gemini setupComplete received
AND
Gemini ASR Ready
AND
PCM / voice processing actually enabled
```

Do not equate:

```text
conversation entered listening phase
```

with:

```text
ASR can already hear the user
```

when Gemini still needs handshake/setup time.

## Preferred implementation direction

Avoid a large state-machine redesign unless necessary.

A minimal design is acceptable:

```text
device state = listening
asr_ready = false
```

while Gemini is connecting/setup, with UI/status showing something like:

```text
Preparing...
Connecting ASR...
```

Then only expose actual `Listening` when:

```text
asr_ready == true
```

and PCM routing is enabled.

However, this only fixes semantics/UI. It does not remove the real deaf window.

## Later latency optimization

After Phase 6 correctness is stable, consider prewarming the next Gemini session while Xiaozhi is still speaking:

```text
speaking
-> start next Gemini connect/setup in background
-> Gemini Ready, but keep PCM detached
-> TTS finishes
-> transition to listening
-> attach PCM immediately
```

This could hide most of the ~1.5–1.7 s Gemini startup cost.

Do not implement this optimization until lifecycle correctness is stable, because it may increase simultaneous SRAM usage during:

```text
Xiaozhi TTS
+
Gemini TLS/WebSocket/setup
```

Existing hardware has previously shown approximately:

```text
free SRAM ~= 36 KB
minimum SRAM ~= 19 KB
```

So prewarm should be treated as a post-correctness optimization and measured carefully.

---

# Remaining issue 2 — timeout semantics are currently conflated

A real failure was observed:

```text
State: listening -> idle
Application: Alert [cancel] Error: Gemini ASR: Gemini final transcript timed out
...
Gemini ASR session closed
```

This is not the desired Phase 6 behavior.

The implementation must distinguish at least **three different timeout classes**.

---

# Timeout A — Gemini setup timeout

Scope:

```text
connect
-> WebSocket/TLS setup
-> setupComplete
```

Purpose:

Detect Gemini transport/setup failure.

This is independent from user listening duration.

A setup timeout may fail the Gemini ASR turn/session, but it must not be confused with conversation inactivity or final-transcript waiting.

---

# Timeout B — final transcript timeout

This timeout applies only after a real utterance has ended.

It should start only after all of the following are true:

```text
speech was actually observed
AND
end-of-speech was detected
AND
audioStreamEnd was actually sent
```

Conceptual flow:

```text
speech begins
-> speech ends
-> stop/detach PCM for this utterance
-> SendAudioStreamEnd()
-> start final deadline
-> wait for finalized inputTranscription
```

A final timeout must **not** start merely because the device entered `listening`.

If the user never spoke, there is no utterance waiting for a final transcript.

## Final timeout failure semantics

If the final deadline expires:

```text
abort current Gemini ASR utterance
discard failed utterance
recover cleanly
keep the Xiaozhi conversation alive if possible
start/re-arm a fresh Gemini listening turn
```

Do not automatically interpret this as:

```text
conversation finished
```

and do not force:

```text
listening -> idle
```

just because one Gemini utterance failed to finalize.

A final timeout is an ASR-turn failure, not automatically a conversation-end event.

The current ~10 s final deadline may be reasonable for this purpose. Do not increase it to the full conversation inactivity duration without evidence.

---

# Timeout C — conversation listening inactivity timeout

This is a separate concept.

It applies when:

```text
Gemini is actually Ready
AND
the system is genuinely listening
AND
the user remains silent for the normal conversation inactivity period
```

The existing Xiaozhi provider appears to use a much longer conversation timeout than the current Gemini final timeout. The exact duration must be verified from the latest source rather than hardcoded from memory.

Inspect the original Xiaozhi ASR/listening lifecycle and find:

- the existing inactivity timeout duration
- the existing timeout callback/path
- the normal goodbye/session-termination semantics

Then reuse that behavior for Gemini where possible.

## Required behavior

Do not do:

```cpp
if (gemini_timeout) {
    StopGeminiAsrTurn();
    SetDeviceState(kDeviceStateIdle);
}
```

for normal conversation inactivity.

Instead, mirror the original Xiaozhi termination flow:

```text
real listening inactivity expires
-> stop current Gemini capture/turn
-> invoke existing Xiaozhi normal conversation timeout/end path
-> preserve existing goodbye semantics
-> close conversation normally
-> only then return Idle
```

If the original Xiaozhi path generates or recognizes a goodbye before closing the session, Gemini mode should use the same lifecycle instead of bypassing it.

Do not implement an independent Gemini-specific goodbye system.

---

# Required timeout model

The final code should conceptually behave like this:

```text
CONNECTING / SETUP
    |
    | setup timeout
    v
setup failure

READY + LISTENING
    |
    | user remains silent
    | normal Xiaozhi conversation inactivity timeout
    v
normal conversation termination / goodbye
    |
    v
idle
```

For an actual utterance:

```text
READY + LISTENING
    |
    | speech observed
    v
speech active
    |
    | VAD end
    v
audioStreamEnd sent
    |
    | final-transcript deadline
    v
final transcript
    |
    v
bridge into current Xiaozhi conversation
    |
    v
speaking
    |
    v
next listening turn
```

If the final deadline fails:

```text
audioStreamEnd
-> final timeout
-> fail/recover this ASR turn
-> do NOT automatically terminate the Xiaozhi conversation
```

---

# Important VAD/final rule

Do not start a final-transcript timeout if no speech was observed.

Required invariant:

```cpp
if (speech_seen && vad_end && !audio_stream_end_sent) {
    DisableGeminiPcmForCurrentUtterance();
    SendAudioStreamEnd();
    StartFinalTranscriptDeadline();
}
```

The exact code structure may differ, but the semantics must remain.

Also handle both race orders safely:

```text
VAD end -> audioStreamEnd -> final
```

and:

```text
Gemini final -> stale/local VAD-end event arrives later
```

Exactly one final and exactly one `audioStreamEnd` should be accepted/sent per utterance.

---

# Empty-final edge case to inspect

Inspect the current parser logic carefully.

If it currently only treats a non-empty `inputTranscription` as final, e.g.:

```cpp
if (!transcript.empty()) {
    final_callback_sent_ = true;
    ...
}
```

then verify what Gemini does when a finalized utterance contains an empty transcript.

Potential bad case:

```text
Gemini sends finalized inputTranscription with text=""
-> client ignores it
-> client waits forever / until final timeout
```

Do not change behavior speculatively, but instrument/inspect this path so a valid empty finalized result cannot masquerade as "no final ever arrived".

If logging is added, log only metadata such as:

```text
inputTranscription received bytes=0
```

Do not log secrets.

---

# Useful temporary diagnostics

For the remaining timeout work, useful temporary logs include:

```text
GeminiASR: speech started turn=N
GeminiASR: VAD end turn=N
GeminiASR: audioStreamEnd sent turn=N
GeminiASR: awaiting final timeout_ms=XXXX
GeminiASR: inputTranscription received bytes=N
GeminiASR: final deadline exceeded elapsed=XXXX
```

Keep these concise and remove noisy debug logs after behavior is verified.

---

# TLS -0x004C observation

The following has repeatedly appeared during intentional Gemini teardown:

```text
mbedtls_ssl_fetch_input error=76
esp-tls-mbedtls: read error :-0x004C
EspSsl: SSL receive failed: -76
```

Current evidence still suggests this is teardown noise because it appears after cancellation/finalization and the Gemini session then closes normally.

Do not suppress all `-0x004C` errors globally.

A mid-session receive failure would still be a real transport error.

This is not currently the primary Phase 6 blocker.

---

# Xiaozhi ASR latency comparison

A Xiaozhi-provider test showed:

```text
251434  State: speaking -> listening
251624  ASR turn start provider=xiaozhi
```

Only about:

```text
190 ms
```

between entering listening and starting the Xiaozhi ASR turn.

This explains why Xiaozhi currently feels much more immediate than Gemini.

For Gemini, the main additional latency is connection/setup, not the conversation bridge itself.

Do not try to solve this by weakening timeout/lifecycle correctness.

Correctness first; prewarm/reuse optimization later.

---

# Do not regress the working bridge

The following behavior is already working and should be preserved:

```text
Gemini final
-> current Xiaozhi conversation
-> LLM / MCP / emotion / TTS
-> speaking
-> listening
-> fresh Gemini ASR turn
```

Do not reintroduce:

```text
Gemini final
-> idle
```

on successful turns.

Do not create a second Xiaozhi conversation for each Gemini utterance.

Do not reopen/wake/bootstrap unnecessarily.

---

# Updated Phase 6 completion criteria

Phase 6 should only be considered complete when all of the following pass.

## 1. Happy-path bridge

```text
Gemini final
-> Xiaozhi response
```

inside the existing conversation.

PASS already observed; preserve it.

## 2. Multi-turn

Wake once:

```text
utterance A
-> Xiaozhi response
-> listening
-> utterance B
-> Xiaozhi response
```

without another wake word and without Idle between turns.

PASS already observed; preserve it.

## 3. Real listening readiness

For Gemini, user-visible `Listening` must correspond to Gemini actually being Ready with PCM capture enabled, or the UI must explicitly show a separate preparing/connecting state until that point.

Currently NOT correct.

## 4. Final timeout

Starts only after:

```text
speech_seen
+
audioStreamEnd sent
```

and recovers an ASR-turn failure without automatically killing the whole conversation.

Currently needs correction/verification.

## 5. Conversation inactivity timeout

Must use the original Xiaozhi conversation timeout semantics/duration/path where possible.

On expiry:

```text
normal conversation termination / goodbye
-> idle
```

not:

```text
Gemini timer
-> force idle
```

Currently needs correction/verification.

## 6. Explicit End Chat

Explicit conversation termination should still:

```text
stop Gemini
close session normally
-> idle
```

## 7. Xiaozhi-provider regression

Original Xiaozhi ASR behavior must remain unchanged.

## 8. Resource stability

Monitor:

```text
free SRAM
minimum SRAM
task stack
PCM queue/drop behavior
```

especially if later attempting Gemini prewarm during TTS.

---

# Priority order from here

Do the remaining work in this order:

```text
1. Fix/verify timeout classification and recovery semantics
2. Fix Listening-ready semantics/status
3. Re-run multi-turn and End Chat regression tests
4. Re-run Xiaozhi provider regression
5. Only after correctness is stable:
   investigate Gemini startup latency optimization / prewarm
6. Later:
   clean Conversation UI internal MCP/control traces
```

Do not mix UI cleanup or major latency optimization into the timeout correctness patch unless necessary.

---

# Final architectural rules

Keep these three rules explicit in the code/review:

```text
1. Gemini final ends one ASR utterance, not the Xiaozhi conversation.

2. "Listening" should mean the active ASR path can actually hear the user.

3. Final-transcript timeout and conversation inactivity timeout are different
   lifecycle events and must not share the same semantics.
```
