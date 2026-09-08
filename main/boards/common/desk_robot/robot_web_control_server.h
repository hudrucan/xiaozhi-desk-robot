#pragma once

#include <esp_http_server.h>

#include <functional>
#include <string>

class RobotWebControlServer {
public:
    using ActionHandler = std::function<bool(const std::string&, int, std::string&)>;
    using StatusHandler = std::function<std::string()>;

    RobotWebControlServer(ActionHandler action_handler, StatusHandler status_handler);
    ~RobotWebControlServer();

    bool Start(int port = 8080);
    void Stop();

private:
    static esp_err_t HandleRoot(httpd_req_t* request);
    static esp_err_t HandleStatus(httpd_req_t* request);
    static esp_err_t HandleAction(httpd_req_t* request);
    static esp_err_t SendJson(httpd_req_t* request, const char* status, const std::string& body);

    httpd_handle_t server_ = nullptr;
    ActionHandler action_handler_;
    StatusHandler status_handler_;
};
