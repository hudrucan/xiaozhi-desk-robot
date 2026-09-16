#pragma once

inline constexpr char kRobotWebControlPage[] = R"CONTROL(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta
      name="viewport"
      content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"
    />
    <meta name="theme-color" content="#0b0906" />
    <title>Xiaozhi Desk Robot</title>
    <style>
      .conversation-card {
        padding: 0;
        display: flex;
        gap: 13px;
        min-height: 360px;
        flex-direction: column;
      }
      .conversation-head {
        display: flex;
        align-items: center;
        justify-content: space-between;
        border-bottom: 1px solid var(--line2);
      }
      .conversation-head strong {
        font-size: 12px;
        letter-spacing: 0.8px;
        text-transform: uppercase;
      }
      .conversation-actions {
        display: flex;
        align-items: center;
        gap: 9px;
      }
      .chat-state {
        font-size: 10px;
        color: var(--green);
        text-transform: uppercase;
        letter-spacing: 0.65px;
      }
      .chat-state.error {
        color: #ff8d82;
      }
      .chat-clear {
        border: 1px solid var(--line);
        border-radius: 8px;
        background: #17120b;
        color: var(--hi);
        padding: 6px 9px;
        font-size: 9px;
        cursor: pointer;
      }
      .chat-clear:disabled {
        opacity: 0.35;
        cursor: not-allowed;
      }
      .conversation-history {
        height: 245px;
        overflow: auto;
        display: flex;
        flex-direction: column;
        gap: 9px;
        background: rgba(6, 5, 3, 0.34);
      }
      .conversation-empty {
        margin: auto;
        color: var(--muted);
        font-size: 10px;
      }
      .chat-row {
        display: flex;
        flex-direction: column;
        max-width: 86%;
        gap: 4px;
      }
      .chat-row.user {
        align-self: flex-end;
        align-items: flex-end;
      }
      .chat-row.assistant {
        align-self: flex-start;
      }
      .chat-role {
        font-size: 8px;
        color: var(--muted);
        letter-spacing: 0.7px;
        text-transform: uppercase;
      }
      .chat-bubble {
        padding: 9px 11px;
        border: 1px solid var(--line2);
        border-radius: 12px;
        background: #100d08;
        color: #e8dcc6;
        font-size: 11px;
        line-height: 1.45;
        white-space: pre-wrap;
        overflow-wrap: anywhere;
      }
      .chat-row.user .chat-bubble {
        background: #2a2114;
        border-color: #6b5432;
        color: var(--hi);
      }
      .chat-error {
        display: none;
        padding: 8px 0 0;
        color: #ff8d82;
        font-size: 9px;
      }
      .chat-error.show {
        display: block;
      }
      .chat-compose {
        display: grid;
        grid-template-columns: minmax(0, 1fr) auto;
        gap: 9px;
        border-top: 1px solid var(--line2);
      }
      .chat-input {
        height: 36px;
        min-height: 36px;
        max-height: 68px;
        resize: none;
        overflow-y: hidden;
        border: 1px solid var(--line);
        border-radius: 11px;
        background: #0d0a06;
        color: var(--text);
        padding: 9px 10px;
        outline: 0;
        font-size: 11px;
        line-height: 1.4;
      }
      .chat-input:focus {
        border-color: var(--brass);
      }
      .chat-send {
        align-self: stretch;
        min-width: 72px;
        border: 1px solid var(--brass);
        border-radius: 11px;
        background: var(--brass);
        color: #171006;
        font-size: 11px;
        font-weight: 800;
        cursor: pointer;
      }
      .chat-send:disabled {
        opacity: 0.35;
        cursor: not-allowed;
      }
      .chat-meta {
        grid-column: 1/-1;
        display: flex;
        justify-content: space-between;
        color: var(--muted);
        font-size: 9px;
      }
      .chat-count.over {
        color: #ff8d82;
      }
      :root {
        --bg: #090806;
        --panel: #17120b;
        --line: #4b3b25;
        --line2: #302617;
        --brass: #c6a15b;
        --hi: #e3c27b;
        --text: #f2eadc;
        --muted: #8f7b5a;
        --red: #d8655b;
        --green: #53c58b;
        --yellow: #d8af52;
        --blue: #559bd8;
        --gap: 10px;
        --shadow: rgba(0, 0, 0, 0.42);
      }
      * {
        box-sizing: border-box;
        -webkit-tap-highlight-color: transparent;
      }
      html,
      body {
        margin: 0;
        min-height: 100%;
        background: var(--bg);
        color: var(--text);
        font-family: ui-rounded, "SF Pro Rounded", "Segoe UI", sans-serif;
      }
      body {
        min-height: 100dvh;
        background:
          radial-gradient(circle at 45% -25%, #3a2d19 0, transparent 42%),
          linear-gradient(180deg, #0e0b07, var(--bg));
        padding: clamp(12px, 1.3vw, 22px);
      }
      button,
      input,
      select,
      textarea {
        font: inherit;
      }
      .app {
        width: 100%;
        min-height: calc(100dvh - 24px);
      }
      header {
        height: 50px;
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin: 0 2px 13px;
      }
      .brand {
        display: flex;
        align-items: center;
        gap: 12px;
      }
      .mark {
        width: 42px;
        height: 42px;
        border: 1px solid var(--line);
        border-radius: 14px;
        background: linear-gradient(145deg, #2a2114, #100d08);
        display: grid;
        place-items: center;
        box-shadow: 0 8px 24px var(--shadow);
      }
      .eyes {
        display: flex;
        gap: 4px;
      }
      .eyes i {
        display: block;
        width: 9px;
        height: 7px;
        border-radius: 3px;
        background: var(--hi);
        box-shadow: 0 0 10px rgba(227, 194, 123, 0.28);
      }
      h1 {
        font-size: 18px;
        line-height: 1.1;
        margin: 0;
      }
      .sub {
        font-size: 10px;
        color: var(--muted);
        margin-top: 4px;
        letter-spacing: 0.8px;
        text-transform: uppercase;
      }
      .online {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 12px;
        border: 1px solid var(--line);
        border-radius: 999px;
        color: var(--muted);
        font-size: 11px;
        background: rgba(20, 16, 10, 0.8);
      }
      .online i,
      .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: var(--muted);
        box-shadow: 0 0 0 3px rgba(143, 123, 90, 0.12);
      }
      .online.ok {
        color: #a9d9bd;
        border-color: #315f48;
      }
      .online.ok i {
        background: var(--green);
      }
      .dashboard {
        display: grid;
        grid-template-columns: minmax(280px, 0.74fr) minmax(
            390px,
            0.96fr
          ) minmax(390px, 0.96fr);
        gap: var(--gap);
        align-items: start;
        min-height: calc(100dvh - 87px);
      }
      .column,
      .control-stack {
        display: flex;
        min-width: 0;
        flex-direction: column;
        gap: var(--gap);
      }
      .right-column {
        min-height: calc(100dvh - 87px);
        height: auto;
      }
      .card {
        position: relative;
        border: 1px solid var(--line);
        border-radius: 18px;
        background: linear-gradient(
          160deg,
          rgba(31, 24, 15, 0.96),
          rgba(17, 14, 9, 0.98)
        );
        box-shadow:
          0 14px 34px var(--shadow),
          inset 0 1px rgba(255, 255, 255, 0.025);
        padding: 17px;
        overflow: hidden;
      }
      .card:before {
        content: "";
        position: absolute;
        inset: 0 auto auto 15%;
        width: 70%;
        height: 1px;
        background: linear-gradient(
          90deg,
          transparent,
          var(--brass),
          transparent
        );
        opacity: 0.32;
      }
      .section-title {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 10px;
        margin-bottom: 13px;
      }
      .section-title strong {
        font-size: 12px;
        letter-spacing: 0.8px;
        text-transform: uppercase;
      }
      .section-title span {
        color: var(--hi);
        font-size: 11px;
      }
      .status {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 9px;
      }
      .metric {
        padding: 11px 12px;
        border-radius: 12px;
        background: rgba(6, 5, 3, 0.48);
        border: 1px solid var(--line2);
        min-width: 0;
      }
      .metric.wide {
        grid-column: 1/-1;
      }
      .label {
        font-size: 9px;
        letter-spacing: 0.9px;
        text-transform: uppercase;
        color: var(--muted);
      }
      .value {
        margin-top: 5px;
        font-size: 13px;
        color: var(--hi);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .health {
        display: flex;
        gap: 7px;
        align-items: center;
      }
      .health .dot {
        width: 7px;
        height: 7px;
        box-shadow: none;
      }
      .health.good .dot {
        background: var(--green);
      }
      .health.bad .dot {
        background: var(--red);
      }
      .safety {
        display: none;
        border-color: #81453e;
        background: linear-gradient(145deg, #321713, #1a0d09);
        color: #ffc0b8;
      }
      .safety.show {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 13px;
      }
      .safety b {
        display: block;
        font-size: 13px;
      }
      .safety small {
        display: block;
        color: #cf8b82;
        font-size: 10px;
        margin-top: 4px;
      }
      .safety button {
        border: 1px solid #a85a50;
        border-radius: 10px;
        background: #491e19;
        color: #ffd4ce;
        padding: 9px 11px;
        font-size: 11px;
        font-weight: 800;
      }
      .dpad {
        display: grid;
        grid-template-columns: repeat(3, 74px);
        grid-template-rows: repeat(3, 63px);
        justify-content: center;
        gap: 8px;
        margin: 0 0 13px;
      }
      .control {
        border: 1px solid #5e492b;
        color: var(--hi);
        background: linear-gradient(145deg, #282014, #141008);
        border-radius: 16px;
        box-shadow:
          0 6px 14px var(--shadow),
          inset 0 1px rgba(255, 255, 255, 0.04);
        cursor: pointer;
        touch-action: none;
        transition:
          transform 0.12s,
          border-color 0.12s,
          background 0.12s,
          opacity 0.12s;
      }
      .control:active,
      .control.active {
        transform: scale(0.94);
        border-color: var(--hi);
        background: #322716;
      }
      .control:disabled {
        cursor: not-allowed;
        opacity: 0.28;
        filter: saturate(0.3);
      }
      .control svg {
        width: 23px;
        height: 23px;
        fill: currentColor;
      }
      .control[data-turn] {
        position: relative;
      }
      .control[data-turn]:after {
        content: attr(data-label);
        position: absolute;
        right: 7px;
        bottom: 5px;
        color: var(--muted);
        font-size: 8px;
      }
      .up {
        grid-column: 2;
      }
      .rotate-left-90 {
        grid-column: 1;
        grid-row: 1;
      }
      .rotate-right-90 {
        grid-column: 3;
        grid-row: 1;
      }
      .left {
        grid-column: 1;
        grid-row: 2;
      }
      .stop {
        grid-column: 2;
        grid-row: 2;
        border-color: #753a33;
        color: #ffaaa2;
        background: linear-gradient(145deg, #351815, #1b0c0a);
        font-size: 11px;
        font-weight: 800;
        letter-spacing: 1.2px;
      }
      .right {
        grid-column: 3;
        grid-row: 2;
      }
      .rotate-left-180 {
        grid-column: 1;
        grid-row: 3;
      }
      .down {
        grid-column: 2;
        grid-row: 3;
      }
      .rotate-right-180 {
        grid-column: 3;
        grid-row: 3;
      }
      .down svg {
        transform: rotate(180deg);
      }
      .left svg {
        transform: rotate(-90deg);
      }
      .right svg {
        transform: rotate(90deg);
      }
      .rotate-right-90 svg,
      .rotate-right-180 svg {
        transform: scaleX(-1);
      }
      .range-row {
        display: grid;
        grid-template-columns: 42px 1fr 47px;
        align-items: center;
        gap: 9px;
        color: var(--muted);
        font-size: 10px;
      }
      input[type="range"] {
        appearance: none;
        width: 100%;
        height: 5px;
        border-radius: 4px;
        background: #3a2d1c;
        outline: 0;
      }
      input[type="range"]::-webkit-slider-thumb {
        appearance: none;
        width: 19px;
        height: 19px;
        border-radius: 50%;
        background: var(--hi);
        border: 4px solid #392b18;
        box-shadow: 0 2px 8px #000;
      }
      .motor-state {
        display: flex;
        justify-content: space-between;
        gap: 8px;
        margin-top: 13px;
        padding-top: 11px;
        border-top: 1px solid var(--line2);
        color: var(--muted);
        font-size: 10px;
      }
      .motor-state b {
        color: var(--hi);
        font-weight: 600;
      }
      .progress {
        height: 5px;
        border-radius: 4px;
        background: #2e2417;
        overflow: hidden;
        margin-top: 9px;
      }
      .progress i {
        display: block;
        width: 0;
        height: 100%;
        background: linear-gradient(90deg, var(--brass), var(--hi));
        transition: width 0.25s;
      }
      .audio-row {
        display: grid;
        grid-template-columns: 102px 1fr 40px;
        align-items: center;
        gap: 9px;
        padding: 10px 0;
      }
      .audio-row + .audio-row {
        border-top: 1px solid var(--line2);
      }
      .audio-name b {
        display: block;
        font-size: 11px;
      }
      .audio-name small {
        display: block;
        color: var(--muted);
        font-size: 9px;
        margin-top: 3px;
      }
      .audio-value {
        text-align: right;
        color: var(--hi);
        font-size: 11px;
        font-variant-numeric: tabular-nums;
      }
      .vu-wrap {
        grid-column: 2/4;
        display: flex;
        align-items: center;
        gap: 7px;
        margin-top: -2px;
      }
      .vu {
        flex: 1;
        height: 6px;
        border-radius: 5px;
        background: #2e2417;
        overflow: hidden;
      }
      .vu i {
        display: block;
        width: 0;
        height: 100%;
        background: linear-gradient(
          90deg,
          var(--green) 0 68%,
          var(--yellow) 82%,
          var(--red)
        );
        transition: width 0.09s;
      }
      .clip {
        font-size: 9px;
        color: var(--muted);
      }
      .clip.on {
        color: #ff8d82;
        font-weight: 800;
      }
      .actions {
        display: grid;
        grid-template-columns: repeat(2, 1fr);
        gap: 9px;
      }
      .action {
        min-height: 58px;
        border: 1px solid var(--line);
        border-radius: 13px;
        background: #17120b;
        color: #e8dcc6;
        padding: 10px 11px;
        display: flex;
        align-items: center;
        gap: 10px;
        text-align: left;
        cursor: pointer;
        transition: 0.14s;
      }
      .action:active {
        transform: scale(0.97);
        background: #2a2114;
      }
      .action .icon {
        width: 31px;
        height: 31px;
        border-radius: 10px;
        background: #2b2113;
        color: var(--hi);
        display: grid;
        place-items: center;
        font-size: 15px;
        flex: 0 0 auto;
      }
      .action b {
        display: block;
        font-size: 11px;
      }
      .action small {
        display: block;
        color: var(--muted);
        font-size: 9px;
        margin-top: 3px;
      }
      .action.danger {
        border-color: #643a30;
      }
      .action.danger .icon {
        color: #ff9c90;
        background: #321713;
      }
      .action.on {
        border-color: #7c6337;
        background: #241c10;
      }
      .action.on .icon {
        background: var(--brass);
        color: #130f08;
        box-shadow: 0 0 16px rgba(198, 161, 91, 0.2);
      }
      .threshold {
        margin-top: 11px;
        padding-top: 11px;
        border-top: 1px solid var(--line2);
      }
      .threshold-head {
        display: flex;
        justify-content: space-between;
        margin-bottom: 9px;
        font-size: 10px;
        color: var(--muted);
      }
      .threshold-head b {
        color: var(--hi);
      }
      .capacity-test {
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 10px;
        align-items: center;
        margin-top: 11px;
        padding: 11px 12px;
        border: 1px solid var(--line2);
        border-radius: 12px;
        background: rgba(6, 5, 3, 0.48);
      }
      .capacity-result b {
        display: block;
        color: var(--hi);
        font-size: 13px;
        font-variant-numeric: tabular-nums;
      }
      .capacity-result small {
        display: block;
        margin-top: 4px;
        color: var(--muted);
        font-size: 9px;
      }
      .capacity-buttons {
        display: flex;
        gap: 6px;
      }
      .capacity-buttons button {
        border: 1px solid var(--line);
        border-radius: 9px;
        background: #21190f;
        color: var(--hi);
        padding: 8px 10px;
        font-size: 9px;
        cursor: pointer;
      }
      .capacity-buttons button:disabled {
        opacity: 0.35;
        cursor: not-allowed;
      }
      .capacity-buttons button.on {
        background: var(--brass);
        color: #171006;
      }
      .capacity-buttons .reset {
        color: #ffaaa2;
        border-color: #643a30;
        background: #28110f;
      }
      .camera-body {
        display: grid;
        grid-template-columns: minmax(170px, 1.15fr) minmax(140px, 0.7fr);
        gap: 12px;
        align-items: start;
      }
      .snapshot {
        width: 100%;
        min-width: 0;
        min-height: 0;
        aspect-ratio: 4/3;
        border: 1px dashed #55442a;
        border-radius: 13px;
        background: #080705;
        display: grid;
        place-items: center;
        overflow: hidden;
        color: var(--muted);
        font-size: 10px;
        text-align: center;
      }
      .snapshot img {
        display: none;
        width: 100%;
        height: 100%;
        object-fit: cover;
      }
      .snapshot.has-image img {
        display: block;
      }
      .snapshot.has-image span {
        display: none;
      }
      .camera-copy {
        display: flex;
        min-width: 0;
        flex-direction: column;
        justify-content: center;
        gap: 10px;
      }
      .camera-copy p {
        margin: 0;
        color: var(--muted);
        font-size: 10px;
        line-height: 1.5;
      }
      .camera-buttons {
        display: grid;
        grid-template-columns: 1fr;
        gap: 8px;
      }
      .mini-button {
        width: 100%;
        border: 1px solid var(--line);
        border-radius: 10px;
        background: #21190f;
        color: var(--hi);
        padding: 10px 12px;
        font-size: 10px;
        cursor: pointer;
      }
      .mini-button.on {
        background: var(--brass);
        color: #171006;
      }
      .mini-button:disabled {
        opacity: 0.45;
      }
      .camera-meta {
        font-size: 9px;
        color: var(--muted);
      }
      .emotion-grid {
        display: grid;
        grid-template-columns: repeat(4, minmax(0, 1fr));
        gap: 7px;
      }
      .emotion {
        border: 1px solid var(--line2);
        border-radius: 10px;
        background: #100d08;
        color: #d9c8a8;
        padding: 9px 6px;
        font-size: 9px;
        text-transform: capitalize;
        cursor: pointer;
        transition: 0.14s;
      }
      .emotion:hover {
        border-color: #6b5432;
        color: var(--hi);
      }
      .emotion:active {
        transform: scale(0.96);
      }
      .emotion.on {
        border-color: var(--brass);
        background: #2a2114;
        color: var(--hi);
        box-shadow: inset 0 0 0 1px rgba(227, 194, 123, 0.12);
      }
      .device-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 9px;
      }
      .device-row {
        min-width: 0;
        padding: 9px 10px;
        border: 1px solid var(--line2);
        border-radius: 11px;
        background: rgba(6, 5, 3, 0.42);
      }
      .device-row span {
        display: block;
        color: var(--muted);
        font-size: 9px;
        text-transform: uppercase;
        letter-spacing: 0.6px;
      }
      .device-row b {
        display: block;
        margin-top: 5px;
        color: #d9c8a8;
        font-size: 11px;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .display-actions {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 9px;
        margin-bottom: 12px;
      }
      .display-settings {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 9px;
      }
      .field {
        display: flex;
        min-width: 0;
        flex-direction: column;
        gap: 6px;
      }
      .field.wide {
        grid-column: 1/-1;
      }
      .field > span {
        color: var(--muted);
        font-size: 9px;
        text-transform: uppercase;
        letter-spacing: 0.6px;
      }
      .text-input,
      .field select {
        width: 100%;
        min-width: 0;
        border: 1px solid var(--line2);
        border-radius: 10px;
        background: #0d0a06;
        color: var(--text);
        padding: 9px 10px;
        font-size: 11px;
        outline: 0;
      }
      .text-input:focus,
      .field select:focus {
        border-color: var(--brass);
      }
      .asr-actions {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-top: 11px;
      }
      .asr-note {
        margin: 9px 0 0;
        color: var(--muted);
        font-size: 9px;
        line-height: 1.4;
      }
      .segment-toggles {
        grid-column: 1/-1;
        display: grid;
        grid-template-columns: repeat(3, 1fr);
        gap: 7px;
      }
      .toggle {
        display: flex;
        align-items: center;
        gap: 7px;
        border: 1px solid var(--line2);
        border-radius: 10px;
        background: #0d0a06;
        color: #d9c8a8;
        padding: 9px;
        font-size: 10px;
        cursor: pointer;
      }
      .toggle input {
        width: 15px;
        height: 15px;
        margin: 0;
        accent-color: var(--brass);
      }
      .oled-designer {
        grid-column: 1/-1;
        display: grid;
        gap: 9px;
        margin-top: 3px;
      }
      .oled-preview-head {
        display: flex;
        justify-content: space-between;
        align-items: center;
        color: var(--muted);
        font-size: 9px;
        text-transform: uppercase;
        letter-spacing: 0.6px;
      }
      .oled-preview-head b {
        color: var(--hi);
        font-weight: 600;
      }
      .oled-pages {
        display: grid;
        gap: 7px;
      }
      .oled-page {
        position: relative;
        width: 100%;
        height: clamp(48px, 5.5vw, 64px);
        border: 2px solid #716449;
        border-radius: 7px;
        background: #020202;
        overflow: hidden;
        color: #fff;
        font:
          600 clamp(8px, 1.7vw, 11px)/1 ui-monospace,
          SFMono-Regular,
          Menlo,
          monospace;
      }
      .oled-cell {
        position: absolute;
        display: grid;
        place-items: center;
        text-align: center;
        border-right: 1px solid #615b50;
        border-bottom: 1px solid #615b50;
        padding: 3px;
        overflow: hidden;
      }
      .oled-cell:last-child {
        border-right: 0;
      }
      .oled-page-num {
        position: absolute;
        right: 3px;
        bottom: 2px;
        color: #777;
        font-size: 7px;
        z-index: 2;
      }
      .oled-widget-list {
        display: grid;
        gap: 7px;
      }
      .oled-widget {
        display: grid;
        grid-template-columns: auto auto minmax(0, 1fr);
        align-items: center;
        gap: 6px;
        padding: 7px;
        border: 1px solid var(--line2);
        border-radius: 11px;
        background: #0d0a06;
      }
      .oled-order {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 3px;
      }
      .oled-order button {
        width: 23px;
        height: 25px;
        padding: 0;
        border: 1px solid var(--line);
        border-radius: 7px;
        background: #21190f;
        color: var(--hi);
        cursor: pointer;
      }
      .oled-order button:disabled {
        opacity: 0.25;
      }
      .oled-widget .toggle {
        border: 0;
        padding: 0;
        background: none;
      }
      .oled-widget-name {
        min-width: 0;
      }
      .oled-widget-name b {
        display: block;
        font-size: 10px;
      }
      .oled-widget-name small {
        display: block;
        color: var(--muted);
        font-size: 8px;
        margin-top: 3px;
      }
      .oled-widget select {
        width: 100%;
        min-width: 0;
        border: 1px solid var(--line2);
        border-radius: 8px;
        background: #151008;
        color: var(--text);
        padding: 6px 5px;
        font-size: 9px;
      }
      .oled-widget select[data-oled-size] {
        grid-column: 1/3;
      }
      .oled-widget select[data-oled-mode] {
        grid-column: 3;
      }
      .oled-widget select:disabled {
        opacity: 0.35;
      }
      .oled-custom {
        grid-column: 1/-1;
        display: grid;
        gap: 5px;
      }
      .oled-custom span {
        color: var(--muted);
        font-size: 8px;
        text-transform: uppercase;
        letter-spacing: 0.45px;
      }
      .oled-custom .text-input {
        width: 100%;
        padding: 7px 8px;
        font-size: 10px;
      }
      .oled-empty {
        padding: 14px;
        text-align: center;
        color: var(--muted);
        font-size: 10px;
        border: 1px dashed var(--line2);
        border-radius: 10px;
      }
      .log-card {
        padding: 0;
        overflow: hidden;
        display: flex;
        flex-direction: column;
        height: 55dvh;
        min-height: 420px;
      }
      .log-head {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 9px;
        padding: 12px 13px;
        border-bottom: 1px solid var(--line2);
      }
      .log-state {
        display: flex;
        align-items: center;
        gap: 8px;
        color: var(--muted);
        font-size: 10px;
        letter-spacing: 0.7px;
        text-transform: uppercase;
      }
      .log-state i {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: var(--green);
      }
      .log-tools {
        display: flex;
        gap: 7px;
        align-items: center;
      }
      .log-button,
      .log-select,
      .log-search {
        border: 1px solid var(--line);
        border-radius: 8px;
        background: #17120b;
        color: var(--hi);
        padding: 7px 9px;
        font-size: 10px;
      }
      .log-search {
        width: min(15vw, 165px);
        color: var(--text);
        outline: 0;
      }
      .log-search::placeholder {
        color: #66573e;
      }
      .log-button {
        cursor: pointer;
      }
      .badge {
        display: none;
        min-width: 19px;
        padding: 3px 5px;
        border-radius: 999px;
        background: #54231f;
        color: #ffaaa2;
        text-align: center;
        font-size: 9px;
      }
      .badge.show {
        display: inline-block;
      }
      .log-output {
        margin: 0;
        flex: 1;
        min-height: 0;
        padding: 13px;
        overflow: auto;
        background: #070604;
        color: #d9c8a8;
        font:
          10px/1.52 ui-monospace,
          SFMono-Regular,
          Menlo,
          Consolas,
          monospace;
        white-space: pre-wrap;
        overflow-wrap: anywhere;
        tab-size: 2;
      }
      .log-foot {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 10px 13px;
        color: var(--muted);
        font-size: 10px;
        border-top: 1px solid var(--line2);
      }
      .auto-scroll {
        display: flex;
        align-items: center;
        gap: 6px;
      }
      .auto-scroll input {
        accent-color: var(--brass);
      }
      .hint {
        text-align: center;
        color: #635640;
        font-size: 9px;
        line-height: 1.4;
        margin: 3px 8px 0;
      }
      .panic-stop {
        position: fixed;
        right: 18px;
        bottom: 18px;
        z-index: 9;
        display: none;
        border: 1px solid #a85a50;
        border-radius: 999px;
        background: #491e19;
        color: #ffd4ce;
        padding: 12px 17px;
        font-size: 11px;
        font-weight: 900;
        box-shadow: 0 10px 32px #000;
      }
      .panic-stop.show {
        display: block;
        animation: pulse 1s infinite alternate;
      }
      @keyframes pulse {
        to {
          box-shadow:
            0 0 0 5px rgba(216, 101, 91, 0.14),
            0 10px 32px #000;
        }
      }
      .toast {
        position: fixed;
        left: 50%;
        bottom: 22px;
        transform: translate(-50%, 16px);
        opacity: 0;
        pointer-events: none;
        background: #2a2114;
        border: 1px solid var(--brass);
        color: #f4e7ce;
        border-radius: 999px;
        padding: 10px 15px;
        font-size: 11px;
        box-shadow: 0 10px 30px #000;
        transition: 0.2s;
        max-width: calc(100% - 32px);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
        z-index: 10;
      }
      .toast.show {
        opacity: 1;
        transform: translate(-50%, 0);
      }
      @media (max-width: 1280px) {
        .dashboard {
          grid-template-columns: 1fr 1fr;
        }
        .right-column {
          display: grid;
          grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
          grid-column: 1/-1;
          align-items: start;
          min-height: 0;
          height: auto;
        }
        .log-card {
          height: min(80dvh, 760px);
          min-height: 380px;
        }
        .log-search {
          width: 165px;
        }
      }
      @media (max-width: 1100px) {
        .log-head {
          align-items: flex-start;
          flex-direction: column;
        }
        .log-tools {
          width: 100%;
          flex-wrap: wrap;
        }
        .log-search {
          width: auto;
          min-width: 120px;
          flex: 1;
        }
      }
      @media (max-width: 900px) {
        .camera-body {
          grid-template-columns: 1fr;
        }
      }
      @media (max-width: 700px) {
        body {
          padding: 12px 10px 74px;
        }
        .app {
          width: 100%;
        }
        header {
          margin-bottom: 10px;
        }
        .sub {
          letter-spacing: 0.45px;
        }
        .dashboard {
          grid-template-columns: 1fr;
          gap: 10px;
        }
        .right-column {
          display: flex;
          grid-column: auto;
          height: auto;
          min-height: 0;
        }
        .log-card {
          height: 54dvh;
          min-height: 380px;
        }
        .card {
          border-radius: 16px;
        }
        .actions {
          grid-template-columns: 1fr 1fr;
        }
        .snapshot {
          min-height: 170px;
        }
        .emotion-grid {
          grid-template-columns: repeat(3, minmax(0, 1fr));
        }
        .log-head {
          align-items: flex-start;
        }
        .log-tools {
          flex-wrap: wrap;
          justify-content: flex-end;
        }
        .log-search {
          width: 120px;
        }
        .panic-stop {
          right: 12px;
          bottom: 12px;
        }
        .hint {
          margin-bottom: 8px;
        }
      }
      @media (max-width: 380px) {
        .dpad {
          grid-template-columns: repeat(3, 62px);
          grid-template-rows: repeat(3, 54px);
        }
        .actions,
        .display-actions,
        .display-settings {
          grid-template-columns: 1fr;
        }
        .status,
        .device-grid {
          grid-template-columns: 1fr;
        }
        .metric.wide,
        .field.wide,
        .segment-toggles {
          grid-column: auto;
        }
        .segment-toggles {
          grid-template-columns: 1fr;
        }
        .emotion-grid {
          grid-template-columns: repeat(2, minmax(0, 1fr));
        }
        .audio-row {
          grid-template-columns: 82px 1fr 34px;
        }
        .capacity-test {
          grid-template-columns: 1fr;
        }
        .capacity-buttons {
          display: grid;
          grid-template-columns: repeat(3, 1fr);
        }
      }
    </style>
  </head>
  <body>
    <main class="app">
      <header>
        <div class="brand">
          <div class="mark">
            <span class="eyes"><i></i><i></i></span>
          </div>
          <div>
            <h1>Xiaozhi Desk Robot</h1>
            <div class="sub">Local control · port 8080</div>
          </div>
        </div>
        <div id="online" class="online"><i></i><span>Connecting</span></div>
      </header>
      <div class="dashboard">
        <div class="column left-column">
          <section class="card">
            <div class="section-title">
              <strong>Device</strong><span id="version">—</span>
            </div>
            <div class="device-grid">
              <div class="device-row">
                <span>Network</span><b id="ssid">—</b>
              </div>
              <div class="device-row">
                <span>Signal</span><b id="rssi">—</b>
              </div>
              <div class="device-row">
                <span>IP address</span><b id="ip">—</b>
              </div>
              <div class="device-row">
                <span>Internal RAM</span><b id="sram">—</b>
              </div>
              <div class="device-row">
                <span>PSRAM free</span><b id="psram">—</b>
              </div>
              <div class="device-row">
                <span>Camera health</span><b id="cameraHealth">—</b>
              </div>
            </div>
          </section>
          <section class="card">
            <div class="section-title">
              <strong>Robot status</strong><span id="uptime">—</span>
            </div>
            <div class="status">
              <div class="metric">
                <div class="label">Assistant</div>
                <div class="value" id="state">—</div>
              </div>
              <div class="metric">
                <div class="label">Camera</div>
                <div class="value" id="camera">—</div>
              </div>
              <div class="metric">
                <div class="label">Floor distance</div>
                <div class="value" id="distance">—</div>
              </div>
              <div class="metric">
                <div class="label">Cliff sensor</div>
                <div class="value" id="rangeState">Checking</div>
              </div>
              <div class="metric">
                <div class="label">Battery</div>
                <div class="value" id="battery">Checking</div>
              </div>
              <div class="metric">
                <div class="label">Current · power</div>
                <div class="value" id="power">—</div>
              </div>
              <div class="metric wide">
                <div class="label">Motion sensor</div>
                <div class="value" id="motionSensor">Checking</div>
              </div>
              <div class="metric wide">
                <div class="label">Secondary OLED</div>
                <div class="value health" id="oledState">
                  <i class="dot"></i><span>Checking</span>
                </div>
              </div>
            </div>
            <div class="capacity-test">
              <div class="capacity-result">
                <b id="capacityResult">0.0 mAh</b
                ><small id="capacityState">Capacity test not started</small
                ><small id="socState">SoC waiting for INA219</small>
              </div>
              <div class="capacity-buttons">
                <button id="capacityStart">Start</button
                ><button id="capacityStop">Stop</button
                ><button id="capacityReset" class="reset">Reset</button>
              </div>
            </div>
            <div class="threshold">
              <div class="threshold-head">
                <span>Cliff threshold</span><b id="cliffValue">150 mm</b>
              </div>
              <input
                id="cliffThreshold"
                type="range"
                min="50"
                max="500"
                step="5"
                value="150"
              />
            </div>
            <div
              class="segment-toggles"
              style="margin-top: 11px; grid-template-columns: 1fr 1fr"
            >
              <label class="toggle"
                ><input id="motionEmotions" type="checkbox" checked />MPU face
                reactions</label
              ><label class="toggle"
                ><input id="emotionMovement" type="checkbox" />Emotion
                movement</label
              >
            </div>
          </section>
          <section class="card">
            <div class="section-title">
              <strong>Audio &amp; lighting</strong>
            </div>
            <div class="audio-row">
              <span class="audio-name"
                ><b>Speaker</b><small>Output volume</small></span
              ><input
                id="speakerVolume"
                type="range"
                min="0"
                max="100"
                step="5"
                value="70"
              /><span class="audio-value" id="speakerValue">70%</span>
            </div>
            <div class="audio-row">
              <span class="audio-name"
                ><b>Microphone</b><small>Input gain</small></span
              ><input
                id="microphoneGain"
                type="range"
                min="1"
                max="3"
                step="1"
                value="1"
              /><span class="audio-value" id="microphoneValue">1×</span>
              <div class="vu-wrap">
                <div class="vu"><i id="micLevel"></i></div>
                <span class="clip" id="micClip">LIVE</span>
              </div>
            </div>
            <div class="audio-row">
              <span class="audio-name"
                ><b>Screen</b><small>Backlight</small></span
              ><input
                id="screenBrightness"
                type="range"
                min="10"
                max="100"
                step="5"
                value="75"
              /><span class="audio-value" id="screenValue">75%</span>
            </div>
            <div class="audio-row">
              <span class="audio-name"
                ><b>Status light</b><small>Edison LED · GPIO48</small></span
              ><input
                id="statusLightBrightness"
                type="range"
                min="0"
                max="100"
                step="5"
                value="100"
              /><span class="audio-value" id="statusLightValue">100%</span>
            </div>
          </section>
          <section class="card">
            <div class="section-title">
              <strong>ASR</strong><span id="asrStatus">Checking</span>
            </div>
            <div class="display-settings">
              <label class="field">
                <span>ASR provider</span>
                <select id="asrProvider">
                  <option value="xiaozhi">Xiaozhi ASR</option>
                  <option value="gemini">Gemini ASR</option>
                </select>
              </label>
              <label class="field wide">
                <span>Gemini API key</span>
                <input
                  id="geminiApiKey"
                  class="text-input"
                  type="password"
                  autocomplete="new-password"
                  placeholder="Leave blank to keep saved key"
                />
              </label>
            </div>
            <div class="asr-actions">
              <button id="saveAsr" class="log-button">Save Gemini key</button>
              <button id="clearGeminiKey" class="log-button">
                Clear Gemini key
              </button>
            </div>
            <p class="asr-note">
              Provider switches are saved immediately and apply from the next listening turn.
              The API key is saved separately.
            </p>
          </section>
          <section class="card">
            <div class="section-title">
              <strong>Emotions</strong><span>5-second preview</span>
            </div>
            <div class="emotion-grid">
              <button class="emotion" data-emotion="neutral">Neutral</button
              ><button class="emotion" data-emotion="happy">Happy</button
              ><button class="emotion" data-emotion="bored">Bored</button
              ><button class="emotion" data-emotion="laughing">Laughing</button
              ><button class="emotion" data-emotion="funny">Funny</button
              ><button class="emotion" data-emotion="sad">Sad</button
              ><button class="emotion" data-emotion="angry">Angry</button
              ><button class="emotion" data-emotion="crying">Crying</button
              ><button class="emotion" data-emotion="loving">Loving</button
              ><button class="emotion" data-emotion="embarrassed">
                Embarrassed</button
              ><button class="emotion" data-emotion="surprised">
                Surprised</button
              ><button class="emotion" data-emotion="shocked">Shocked</button
              ><button class="emotion" data-emotion="thinking">Thinking</button
              ><button class="emotion" data-emotion="winking">Winking</button
              ><button class="emotion" data-emotion="cool">Cool</button
              ><button class="emotion" data-emotion="relaxed">Relaxed</button
              ><button class="emotion" data-emotion="delicious">
                Delicious</button
              ><button class="emotion" data-emotion="kissy">Kissy</button
              ><button class="emotion" data-emotion="confident">
                Confident</button
              ><button class="emotion" data-emotion="sleepy">Sleepy</button
              ><button class="emotion" data-emotion="silly">Silly</button
              ><button class="emotion" data-emotion="confused">Confused</button
              ><button class="emotion" data-emotion="suspicious">
                Suspicious</button
              ><button class="emotion" data-emotion="shake">Shake</button>
            </div>
          </section>
        </div>
        <div class="column center-column">
          <section class="card">
            <div class="section-title">
              <strong>Drive</strong><span id="durationLabel">250 ms</span>
            </div>
            <div class="dpad">
              <button
                class="control rotate-left-90"
                data-turn="-90"
                data-label="90°"
                aria-label="Rotate left 90 degrees"
                title="Rotate left 90°"
              >
                <svg viewBox="0 0 24 24">
                  <path
                    d="M7.4 6.4 6 5 2 9l4 4 1.4-1.4L5.8 10H13a5 5 0 1 1-4.6 7H6.2a7 7 0 1 0 6.8-9H5.8z"
                  />
                </svg></button
              ><button
                class="control up"
                data-drive="forward"
                aria-label="Forward"
              >
                <svg viewBox="0 0 24 24">
                  <path d="M12 4 4 12h5v8h6v-8h5z" />
                </svg></button
              ><button
                class="control rotate-right-90"
                data-turn="90"
                data-label="90°"
                aria-label="Rotate right 90 degrees"
                title="Rotate right 90°"
              >
                <svg viewBox="0 0 24 24">
                  <path
                    d="M7.4 6.4 6 5 2 9l4 4 1.4-1.4L5.8 10H13a5 5 0 1 1-4.6 7H6.2a7 7 0 1 0 6.8-9H5.8z"
                  />
                </svg></button
              ><button class="control left" data-drive="left" aria-label="Left">
                <svg viewBox="0 0 24 24">
                  <path d="M12 4 4 12h5v8h6v-8h5z" />
                </svg></button
              ><button class="control stop" data-stop>STOP</button
              ><button
                class="control right"
                data-drive="right"
                aria-label="Right"
              >
                <svg viewBox="0 0 24 24">
                  <path d="M12 4 4 12h5v8h6v-8h5z" />
                </svg></button
              ><button
                class="control rotate-left-180"
                data-turn="-180"
                data-label="180°"
                aria-label="Rotate left 180 degrees"
                title="Rotate left 180°"
              >
                <svg viewBox="0 0 24 24">
                  <path
                    d="M7.4 6.4 6 5 2 9l4 4 1.4-1.4L5.8 10H13a5 5 0 1 1-4.6 7H6.2a7 7 0 1 0 6.8-9H5.8z"
                  />
                </svg></button
              ><button
                class="control down"
                data-drive="backward"
                aria-label="Backward"
              >
                <svg viewBox="0 0 24 24">
                  <path d="M12 4 4 12h5v8h6v-8h5z" />
                </svg></button
              ><button
                class="control rotate-right-180"
                data-turn="180"
                data-label="180°"
                aria-label="Rotate right 180 degrees"
                title="Rotate right 180°"
              >
                <svg viewBox="0 0 24 24">
                  <path
                    d="M7.4 6.4 6 5 2 9l4 4 1.4-1.4L5.8 10H13a5 5 0 1 1-4.6 7H6.2a7 7 0 1 0 6.8-9H5.8z"
                  />
                </svg>
              </button>
            </div>
            <div class="range-row">
              <span>50 ms</span
              ><input
                id="duration"
                type="range"
                min="50"
                max="5000"
                step="50"
                value="250"
              /><span>5 sec</span>
            </div>
            <div class="range-row" style="margin-top: 12px">
              <span>Speed</span
              ><input
                id="motorSpeed"
                type="range"
                min="55"
                max="100"
                step="5"
                value="100"
              /><span id="motorSpeedValue">100%</span>
            </div>
            <div class="motor-state">
              <span>Motion <b id="motion">Stopped</b></span
              ><span>Queue <b id="queue">0</b></span
              ><span>Time <b id="motorTime">0 ms</b></span>
            </div>
            <div class="progress"><i id="danceProgress"></i></div>
          </section>
          <section class="card">
            <div class="section-title">
              <strong>Displays</strong><span>Saved automatically</span>
            </div>
            <div class="display-actions">
              <button
                class="action"
                id="displayFlip"
                data-action="display_flip"
              >
                <span class="icon">↕</span
                ><span
                  ><b>Main display</b><small>Rotate 180°</small></span
                ></button
              ><button class="action" id="oledFlip" data-action="oled_flip">
                <span class="icon">↕</span
                ><span><b>Secondary OLED</b><small>Rotate 180°</small></span>
              </button>
            </div>
            <div class="threshold">
              <div class="threshold-head">
                <span>OLED contrast</span><b id="oledContrastValue">50%</b>
              </div>
              <input
                id="oledContrast"
                type="range"
                min="0"
                max="100"
                step="1"
                value="50"
              />
            </div>
            <div class="display-settings">
              <div class="oled-designer">
                <div class="oled-preview-head">
                  <span>Layout preview</span><b id="oledPageCount">—</b>
                </div>
                <div id="oledPreview" class="oled-pages">
                  <div class="oled-empty">Waiting for OLED configuration</div>
                </div>
                <div class="oled-preview-head">
                  <span>Widgets · display order</span
                  ><span>Use arrows to reorder</span>
                </div>
                <div id="oledWidgets" class="oled-widget-list"></div>
              </div>
            </div>
          </section>
          <p class="hint">
            Local network only · tap a direction to run for the selected time ·
            STOP cancels
          </p>
        </div>
        <div class="column right-column">
          <div class="control-stack">
            <section class="card conversation-card">
              <div class="conversation-head">
                <strong>Conversation</strong>
                <div class="conversation-actions">
                  <span id="chatState" class="chat-state">Ready</span
                  ><button
                    id="chatClear"
                    class="chat-clear"
                    title="Clear local conversation history"
                  >
                    Clear
                  </button>
                </div>
              </div>
              <div id="conversationHistory" class="conversation-history">
                <div class="conversation-empty">
                  Type or speak to chat with Xiaozhi
                </div>
              </div>
              <div id="chatError" class="chat-error"></div>
              <div class="chat-compose">
                <textarea
                  id="chatInput"
                  class="chat-input"
                  rows="1"
                  placeholder="Message Xiaozhi…"
                  aria-label="Chat message"
                ></textarea
                ><button id="chatSend" class="chat-send" disabled>Send</button>
                <div class="chat-meta">
                  <span>Enter to send · Shift+Enter for newline</span
                  ><span id="chatCount" class="chat-count">0 / 512</span>
                </div>
              </div>
            </section>
            <section id="safety" class="card safety">
              <div>
                <b>Table edge detected</b
                ><small>Forward and turns are locked. Reverse only.</small>
              </div>
              <button data-stop>STOP</button>
            </section>
            <section class="card">
              <div class="section-title">
                <strong>Browser camera</strong><span>VGA · JPEG</span>
              </div>
              <div class="camera-body">
                <div id="snapshot" class="snapshot">
                  <span>No snapshot yet</span
                  ><img id="snapshotImage" alt="Camera snapshot" />
                </div>
                <div class="camera-copy">
                  <p>
                    Still capture or a light ~1 FPS browser preview. Live mode
                    pauses outside Idle.
                  </p>
                  <div class="camera-buttons">
                    <button id="takeSnapshot" class="mini-button">
                      Take photo</button
                    ><button id="browserLive" class="mini-button">Live</button
                    ><button
                      id="liveCamera"
                      class="mini-button"
                      data-action="live_camera"
                    >
                      Screen preview</button
                    ><button
                      id="cameraFlip"
                      class="mini-button"
                      data-action="camera_flip"
                    >
                      Flip camera
                    </button>
                  </div>
                  <div id="snapshotMeta" class="camera-meta">Camera ready</div>
                </div>
              </div>
            </section>
            <section class="card">
              <div class="section-title">
                <strong>Quick actions</strong><span id="danceState">Ready</span>
              </div>
              <div class="actions">
                <button class="action" id="wakeAction" data-action="wake">
                  <span class="icon">◉</span
                  ><span
                    ><b id="wakeLabel">Wake</b
                    ><small>Start or stop conversation</small></span
                  ></button
                ><button
                  class="action"
                  id="idleAction"
                  data-action="return_idle"
                >
                  <span class="icon">○</span
                  ><span
                    ><b>Return to idle</b
                    ><small>Stop activity safely</small></span
                  ></button
                ><button class="action" id="danceAction" data-action="dance">
                  <span class="icon">♪</span
                  ><span
                    ><b>Dance</b><small>30–50 randomized steps</small></span
                  ></button
                ><button
                  class="action"
                  id="lightsAction"
                  data-action="lights_toggle"
                >
                  <span class="icon">☀</span
                  ><span
                    ><b>Status lights</b><small>Toggle Edison LEDs</small></span
                  ></button
                ><button class="action" data-action="audio_test">
                  <span class="icon">♫</span
                  ><span
                    ><b>Audio test</b><small>Play a speaker tone</small></span
                  ></button
                ><button class="action danger" data-action="wifi_config">
                  <span class="icon">⌁</span
                  ><span
                    ><b>Wi-Fi config</b
                    ><small>Reconnect this robot</small></span
                  ></button
                ><button class="action danger" data-action="reboot">
                  <span class="icon">↯</span
                  ><span><b>Reboot</b><small>Restart this robot</small></span>
                </button>
              </div>
            </section>
          </div>
          <section class="card log-card">
            <div class="log-head">
              <div class="log-state">
                <i></i><span id="logState">Live system log</span
                ><span id="errorBadge" class="badge">0</span>
              </div>
              <div class="log-tools">
                <input
                  id="logSearch"
                  class="log-search"
                  placeholder="Search logs"
                /><select id="logFilter" class="log-select">
                  <option value="all">All</option>
                  <option value="error">Errors</option>
                  <option value="warn">Warnings</option>
                  <option value="info">Info</option></select
                ><button id="pauseLog" class="log-button">Pause</button
                ><button id="downloadLog" class="log-button">Save</button
                ><button id="clearLog" class="log-button">Clear</button>
              </div>
            </div>
            <pre id="logOutput" class="log-output">
Waiting for system logs…</pre
            >
            <div class="log-foot">
              <span id="logSize">16 KB device buffer</span
              ><label class="auto-scroll"
                ><input id="autoScroll" type="checkbox" checked />
                Auto-scroll</label
              >
            </div>
          </section>
        </div>
      </div>
    </main>
    <button id="panicStop" class="panic-stop" data-stop>STOP MOTORS</button>
    <div class="toast" id="toast"></div>
    <script>
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
    </script>
  </body>
</html>
)CONTROL";
