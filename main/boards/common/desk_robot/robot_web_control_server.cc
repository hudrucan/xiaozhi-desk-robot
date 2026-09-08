#include "robot_web_control_server.h"

#include <esp_log.h>
#include <cJSON.h>

#include <array>
#include <utility>

#define TAG "RobotWebControl"

namespace {

constexpr char kControlPage[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#0b0906">
<title>Xiaozhi Robot Control</title>
<style>
:root{--bg:#090806;--panel:#15110b;--panel2:#1d170e;--line:#4b3b25;--brass:#c6a15b;--hi:#e3c27b;--muted:#8f7b5a;--red:#d8655b;--green:#53c58b;--shadow:rgba(0,0,0,.42)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;min-height:100%;background:var(--bg);color:#f2eadc;font-family:ui-rounded,"SF Pro Rounded","Segoe UI",sans-serif}
body{background:radial-gradient(circle at 50% -20%,#3a2d19 0,transparent 42%),linear-gradient(180deg,#0e0b07,var(--bg));padding:22px 16px 36px}
button,input{font:inherit}.app{width:min(100%,460px);margin:auto}
header{display:flex;align-items:center;justify-content:space-between;margin:0 2px 20px}.brand{display:flex;align-items:center;gap:12px}.mark{width:42px;height:42px;border:1px solid var(--line);border-radius:14px;background:linear-gradient(145deg,#2a2114,#100d08);display:grid;place-items:center;box-shadow:0 8px 24px var(--shadow)}.eyes{display:flex;gap:4px}.eyes i{display:block;width:9px;height:7px;border-radius:3px;background:var(--hi);box-shadow:0 0 10px rgba(227,194,123,.28)}
h1{font-size:17px;line-height:1.1;margin:0;letter-spacing:.15px}.sub{font-size:11px;color:var(--muted);margin-top:4px;letter-spacing:.8px;text-transform:uppercase}
.online{display:flex;align-items:center;gap:7px;padding:8px 10px;border:1px solid var(--line);border-radius:999px;color:var(--muted);font-size:11px;background:rgba(20,16,10,.8)}.online i{width:7px;height:7px;border-radius:50%;background:var(--muted);box-shadow:0 0 0 3px rgba(143,123,90,.12)}.online.ok{color:#a9d9bd;border-color:#315f48}.online.ok i{background:var(--green);box-shadow:0 0 0 3px rgba(83,197,139,.13)}
.card{position:relative;border:1px solid var(--line);border-radius:22px;background:linear-gradient(160deg,rgba(31,24,15,.96),rgba(17,14,9,.98));box-shadow:0 16px 40px var(--shadow),inset 0 1px rgba(255,255,255,.025);padding:18px;margin-bottom:14px;overflow:hidden}.card:before{content:"";position:absolute;inset:0 auto auto 15%;width:70%;height:1px;background:linear-gradient(90deg,transparent,var(--brass),transparent);opacity:.35}
.status{display:grid;grid-template-columns:1fr 1fr;gap:10px}.metric{padding:12px 13px;border-radius:14px;background:rgba(6,5,3,.48);border:1px solid #302617}.label{font-size:10px;letter-spacing:1px;text-transform:uppercase;color:var(--muted)}.value{margin-top:5px;font-size:14px;color:var(--hi);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.section-title{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}.section-title strong{font-size:13px;letter-spacing:.8px;text-transform:uppercase}.duration{color:var(--hi);font-size:12px}
.dpad{display:grid;grid-template-columns:repeat(3,76px);grid-template-rows:repeat(3,66px);justify-content:center;gap:8px;margin:2px 0 17px}.control{border:1px solid #5e492b;color:var(--hi);background:linear-gradient(145deg,#282014,#141008);border-radius:17px;box-shadow:0 7px 15px var(--shadow),inset 0 1px rgba(255,255,255,.04);cursor:pointer;touch-action:none;transition:transform .12s,border-color .12s,background .12s}.control:active,.control.active{transform:scale(.94);border-color:var(--hi);background:#322716}.control svg{width:23px;height:23px;fill:currentColor}.up{grid-column:2}.left{grid-column:1;grid-row:2}.stop{grid-column:2;grid-row:2;border-color:#753a33;color:#ffaaa2;background:linear-gradient(145deg,#351815,#1b0c0a);font-size:11px;font-weight:800;letter-spacing:1.3px}.right{grid-column:3;grid-row:2}.down{grid-column:2;grid-row:3}.down svg{transform:rotate(180deg)}.left svg{transform:rotate(-90deg)}.right svg{transform:rotate(90deg)}
.range-row{display:grid;grid-template-columns:42px 1fr 48px;align-items:center;gap:10px;color:var(--muted);font-size:11px}input[type=range]{appearance:none;width:100%;height:4px;border-radius:4px;background:#3a2d1c;outline:0}input[type=range]::-webkit-slider-thumb{appearance:none;width:20px;height:20px;border-radius:50%;background:var(--hi);border:4px solid #392b18;box-shadow:0 2px 8px #000}
.audio-row{display:grid;grid-template-columns:106px 1fr 42px;align-items:center;gap:10px;padding:11px 0}.audio-row+.audio-row{border-top:1px solid #302617}.audio-name b{display:block;font-size:12px}.audio-name small{display:block;color:var(--muted);font-size:9px;margin-top:3px}.audio-value{text-align:right;color:var(--hi);font-size:12px;font-variant-numeric:tabular-nums}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px}.action{min-height:58px;border:1px solid #4b3b25;border-radius:15px;background:#17120b;color:#e8dcc6;padding:10px 12px;display:flex;align-items:center;gap:10px;text-align:left;cursor:pointer;transition:.14s}.action:active{transform:scale(.97);background:#2a2114}.action .icon{width:32px;height:32px;border-radius:10px;background:#2b2113;color:var(--hi);display:grid;place-items:center;font-size:16px}.action b{display:block;font-size:12px}.action small{display:block;color:var(--muted);font-size:9px;margin-top:3px}.action.danger{border-color:#643a30}.action.danger .icon{color:#ff9c90;background:#321713}
.action.on{border-color:#7c6337;background:#241c10}.action.on .icon{background:var(--brass);color:#130f08;box-shadow:0 0 16px rgba(198,161,91,.2)}
.hint{text-align:center;color:#635640;font-size:10px;line-height:1.5;margin:16px 16px 0}.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,18px);opacity:0;pointer-events:none;background:#2a2114;border:1px solid var(--brass);color:#f4e7ce;border-radius:999px;padding:10px 16px;font-size:12px;box-shadow:0 10px 30px #000;transition:.22s;max-width:calc(100% - 32px);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;z-index:5}.toast.show{opacity:1;transform:translate(-50%,0)}
@media(max-width:370px){.dpad{grid-template-columns:repeat(3,68px);grid-template-rows:repeat(3,60px)}.actions{grid-template-columns:1fr}.card{padding:15px}}
</style>
</head>
<body>
<main class="app">
<header><div class="brand"><div class="mark"><span class="eyes"><i></i><i></i></span></div><div><h1>Xiaozhi Desk Robot</h1><div class="sub">Local control · port 8080</div></div></div><div id="online" class="online"><i></i><span>Connecting</span></div></header>
<section class="card status"><div class="metric"><div class="label">Assistant</div><div class="value" id="state">—</div></div><div class="metric"><div class="label">Camera</div><div class="value" id="camera">Normal</div></div></section>
<section class="card"><div class="section-title"><strong>Drive</strong><span class="duration" id="durationLabel">500 ms</span></div><div class="dpad">
<button class="control up" data-drive="forward" aria-label="Forward"><svg viewBox="0 0 24 24"><path d="M12 4 4 12h5v8h6v-8h5z"/></svg></button>
<button class="control left" data-drive="left" aria-label="Left"><svg viewBox="0 0 24 24"><path d="M12 4 4 12h5v8h6v-8h5z"/></svg></button>
<button class="control stop" id="stop">STOP</button>
<button class="control right" data-drive="right" aria-label="Right"><svg viewBox="0 0 24 24"><path d="M12 4 4 12h5v8h6v-8h5z"/></svg></button>
<button class="control down" data-drive="backward" aria-label="Backward"><svg viewBox="0 0 24 24"><path d="M12 4 4 12h5v8h6v-8h5z"/></svg></button></div>
<div class="range-row"><span>50 ms</span><input id="duration" type="range" min="50" max="2000" step="50" value="500"><span>2 sec</span></div></section>
<section class="card"><div class="section-title"><strong>Audio</strong></div>
<div class="audio-row"><span class="audio-name"><b>Speaker</b><small>Output volume</small></span><input id="speakerVolume" type="range" min="0" max="100" step="5" value="70"><span class="audio-value" id="speakerValue">70%</span></div>
<div class="audio-row"><span class="audio-name"><b>Microphone</b><small>Input gain</small></span><input id="microphoneGain" type="range" min="1" max="3" step="1" value="1"><span class="audio-value" id="microphoneValue">1×</span></div></section>
<section class="card"><div class="section-title"><strong>Quick actions</strong></div><div class="actions">
<button class="action" data-action="wake"><span class="icon">◉</span><span><b>Wake</b><small>Start or stop conversation</small></span></button>
<button class="action" data-action="dance"><span class="icon">♪</span><span><b>Dance</b><small>Run a short movement</small></span></button>
<button class="action" id="liveCamera" data-action="live_camera"><span class="icon">▣</span><span><b>Live camera</b><small id="liveCameraText">Idle-only preview</small></span></button>
<button class="action" data-action="camera_flip"><span class="icon">↻</span><span><b>Flip camera</b><small>Rotate capture by 180°</small></span></button>
<button class="action danger" data-action="wifi_config"><span class="icon">⌁</span><span><b>Wi-Fi config</b><small>Reconnect this robot</small></span></button>
</div></section><p class="hint">Available only on your local network. Hold a direction to move; release to stop.</p>
</main><div class="toast" id="toast"></div>
<script>
const $=s=>document.querySelector(s),toast=$('#toast');let toastTimer,holding=false;
function notify(text){toast.textContent=text;toast.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>toast.classList.remove('show'),1800)}
async function action(name,extra={}){try{const r=await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:name,...extra})});const j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||'Command failed');notify(j.message||'Done');setTimeout(status,180)}catch(e){notify(e.message||'Robot is offline')}}
async function status(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const j=await r.json(),idle=j.state==='idle';$('#online').classList.add('ok');$('#online span').textContent='Online';$('#state').textContent=(j.state||'idle').replaceAll('_',' ');$('#camera').textContent=j.live_camera?(idle?'Live preview':'Live paused'):(j.camera_flipped?'Flipped':'Normal');liveCamera.classList.toggle('on',!!j.live_camera);liveCameraText.textContent=j.live_camera?(idle?'Enabled · idle only':'Paused until idle'):'Idle-only preview';if(document.activeElement!==speakerVolume){speakerVolume.value=j.speaker_volume;speakerValue.textContent=j.speaker_volume+'%'}if(document.activeElement!==microphoneGain){microphoneGain.value=j.microphone_gain;microphoneValue.textContent=j.microphone_gain+'×'}}catch(e){$('#online').classList.remove('ok');$('#online span').textContent='Offline'}}
const duration=$('#duration'),durationLabel=$('#durationLabel');duration.oninput=()=>durationLabel.textContent=duration.value<1000?duration.value+' ms':(duration.value/1000).toFixed(1)+' sec';
const speakerVolume=$('#speakerVolume'),speakerValue=$('#speakerValue'),microphoneGain=$('#microphoneGain'),microphoneValue=$('#microphoneValue'),liveCamera=$('#liveCamera'),liveCameraText=$('#liveCameraText');speakerVolume.oninput=()=>speakerValue.textContent=speakerVolume.value+'%';speakerVolume.onchange=()=>action('speaker_volume',{value:+speakerVolume.value});microphoneGain.oninput=()=>microphoneValue.textContent=microphoneGain.value+'×';microphoneGain.onchange=()=>action('microphone_gain',{value:+microphoneGain.value});
document.querySelectorAll('[data-drive]').forEach(btn=>{btn.onpointerdown=e=>{e.preventDefault();holding=true;btn.classList.add('active');btn.setPointerCapture(e.pointerId);action(btn.dataset.drive,{duration_ms:+duration.value})};const release=e=>{if(!holding)return;holding=false;btn.classList.remove('active');action('stop')};btn.onpointerup=release;btn.onpointercancel=release;btn.onlostpointercapture=release;btn.oncontextmenu=e=>e.preventDefault()});
$('#stop').onclick=()=>action('stop');document.querySelectorAll('[data-action]').forEach(btn=>btn.onclick=()=>{const name=btn.dataset.action;if(name==='wifi_config'&&!confirm('Open Wi-Fi setup mode? This page will disconnect.'))return;action(name)});
status();setInterval(status,1500);
</script></body></html>)HTML";

}  // namespace

RobotWebControlServer::RobotWebControlServer(ActionHandler action_handler,
                                             StatusHandler status_handler)
    : action_handler_(std::move(action_handler)), status_handler_(std::move(status_handler)) {}

RobotWebControlServer::~RobotWebControlServer() { Stop(); }

bool RobotWebControlServer::Start(int port) {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = 32770;
    config.max_uri_handlers = 6;
    config.stack_size = 6144;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start local control server on port %d", port);
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleRoot,
        .user_ctx = this,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = HandleStatus,
        .user_ctx = this,
    };
    const httpd_uri_t action = {
        .uri = "/api/action",
        .method = HTTP_POST,
        .handler = HandleAction,
        .user_ctx = this,
    };
    if (httpd_register_uri_handler(server_, &root) != ESP_OK ||
        httpd_register_uri_handler(server_, &status) != ESP_OK ||
        httpd_register_uri_handler(server_, &action) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register local control routes");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "Local control UI started on port %d", port);
    return true;
}

void RobotWebControlServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

esp_err_t RobotWebControlServer::HandleRoot(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kControlPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RobotWebControlServer::HandleStatus(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    return SendJson(request, "200 OK", self->status_handler_());
}

esp_err_t RobotWebControlServer::HandleAction(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (request->content_len <= 0 || request->content_len > 256) {
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Invalid request"})");
    }

    std::array<char, 257> body = {};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), received);
    const cJSON* action =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "action") : nullptr;
    const cJSON* duration =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "duration_ms") : nullptr;
    const cJSON* value =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "value") : nullptr;
    if (!cJSON_IsString(action) || action->valuestring == nullptr) {
        cJSON_Delete(root);
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Missing action"})");
    }

    const int control_value = cJSON_IsNumber(value)
                                  ? value->valueint
                                  : (cJSON_IsNumber(duration) ? duration->valueint : 500);
    std::string message;
    const bool accepted = self->action_handler_(action->valuestring, control_value, message);
    cJSON_Delete(root);

    cJSON* response = cJSON_CreateObject();
    if (response == nullptr) {
        return SendJson(request, "500 Internal Server Error",
                        R"({"ok":false,"message":"Out of memory"})");
    }
    cJSON_AddBoolToObject(response, "ok", accepted);
    cJSON_AddStringToObject(response, "message", message.c_str());
    char* encoded = cJSON_PrintUnformatted(response);
    const std::string response_body = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(response);
    return SendJson(request, accepted ? "200 OK" : "400 Bad Request", response_body);
}

esp_err_t RobotWebControlServer::SendJson(httpd_req_t* request, const char* status,
                                          const std::string& body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
}
