#include "robot_web_status_routes.h"

#include "robot_web_status.h"

#include <cJSON.h>

#include <array>
#include <cstring>

bool RegisterRobotWebStatusRoutes(httpd_handle_t server, void* context,
                                  esp_err_t (*handler)(httpd_req_t*)) {
    const std::array<const char*, 8> uris = {
        "/api/status/core",    "/api/status/motors", "/api/status/sensors",
        "/api/status/battery", "/api/status/display", "/api/status/camera",
        "/api/status/audio",   "/api/status/system",
    };
    for (const char* uri : uris) {
        const httpd_uri_t route = {
            .uri = uri,
            .method = HTTP_GET,
            .handler = handler,
            .user_ctx = context,
        };
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            return false;
        }
    }
    return true;
}

cJSON* CreateRobotWebDomainStatus(RobotWebStatus& status, const char* uri) {
    if (std::strcmp(uri, "/api/status/core") == 0) {
        return status.CreateCore();
    }
    if (std::strcmp(uri, "/api/status/motors") == 0) {
        return status.CreateMotors();
    }
    if (std::strcmp(uri, "/api/status/sensors") == 0) {
        return status.CreateSensors();
    }
    if (std::strcmp(uri, "/api/status/battery") == 0) {
        return status.CreateBattery();
    }
    if (std::strcmp(uri, "/api/status/display") == 0) {
        return status.CreateDisplay();
    }
    if (std::strcmp(uri, "/api/status/camera") == 0) {
        return status.CreateCamera();
    }
    if (std::strcmp(uri, "/api/status/audio") == 0) {
        return status.CreateAudio();
    }
    if (std::strcmp(uri, "/api/status/system") == 0) {
        return status.CreateSystem();
    }
    return nullptr;
}
