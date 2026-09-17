#pragma once

#include <esp_http_server.h>

struct cJSON;
class RobotWebStatus;

bool RegisterRobotWebStatusRoutes(httpd_handle_t server, void* context,
                                  esp_err_t (*handler)(httpd_req_t*));
cJSON* CreateRobotWebDomainStatus(RobotWebStatus& status, const char* uri);
