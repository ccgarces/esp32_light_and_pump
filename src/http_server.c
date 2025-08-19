#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "storage.h"

static const char *TAG = "http_server";

static esp_err_t get_readings_handler(httpd_req_t *req)
{
    char buf[4096];
    int len = storage_export_readings_json(buf, sizeof(buf));
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read storage");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static httpd_uri_t readings_uri = {
    .uri = "/readings",
    .method = HTTP_GET,
    .handler = get_readings_handler,
    .user_ctx = NULL
};

void http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &readings_uri);
        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGW(TAG, "Failed to start HTTP server");
    }
}
