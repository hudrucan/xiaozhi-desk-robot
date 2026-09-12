#pragma once

#include <esp_http_server.h>

#include <functional>
#include <string>

class RobotWebControlServer {
public:
    using ActionHandler =
        std::function<bool(const std::string&, int, const std::string&, std::string&)>;
    using StatusHandler = std::function<std::string()>;
    using SnapshotSender = std::function<bool(const uint8_t*, size_t)>;
    using SnapshotHandler = std::function<bool(const SnapshotSender&)>;

    RobotWebControlServer(ActionHandler action_handler, StatusHandler status_handler,
                          SnapshotHandler snapshot_handler = {});
    ~RobotWebControlServer();

    // Start buffering logs before Wi-Fi and the HTTP server are available.
    // Calling this more than once is safe.
    static void BeginLogCapture();

    bool Start(int port = 8080);
    void Stop();

private:
    static esp_err_t HandleRoot(httpd_req_t* request);
    static esp_err_t HandleStatus(httpd_req_t* request);
    static esp_err_t HandleLogs(httpd_req_t* request);
    static esp_err_t HandleAction(httpd_req_t* request);
    static esp_err_t HandleSnapshot(httpd_req_t* request);
    static esp_err_t SendJson(httpd_req_t* request, const char* status, const std::string& body);

    httpd_handle_t server_ = nullptr;
    ActionHandler action_handler_;
    StatusHandler status_handler_;
    SnapshotHandler snapshot_handler_;
};
