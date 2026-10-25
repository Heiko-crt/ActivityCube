#include "web_server.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "time_service.h"
#include "uart_counter.h"

#define FILES_URI_PREFIX "/files/"
#define FILE_NAME_MAX_LENGTH 64
#define FILE_PATH_MAX_LENGTH 96
#define FILE_UPLOAD_MAX_SIZE (265U * 1024U)
#define FILE_IO_BUFFER_SIZE 1024

static httpd_handle_t server;

static bool get_file_paths(const char *uri, char *path, size_t path_size,
                           char *temporary_path, size_t temporary_path_size)
{
    const size_t prefix_length = sizeof(FILES_URI_PREFIX) - 1;
    if (strncmp(uri, FILES_URI_PREFIX, prefix_length) != 0) {
        return false;
    }

    const char *name = uri + prefix_length;
    const size_t name_length = strnlen(name, FILE_NAME_MAX_LENGTH + 1);
    if (name_length == 0 || name_length > FILE_NAME_MAX_LENGTH ||
        name[0] == '.' || strstr(name, "..") != NULL) {
        return false;
    }

    for (size_t i = 0; i < name_length; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }

    const int path_length = snprintf(path, path_size, "/littlefs/%s", name);
    const int temporary_path_length =
        snprintf(temporary_path, temporary_path_size, "/littlefs/.upload-%s.tmp", name);
    return path_length > 0 && (size_t)path_length < path_size &&
           temporary_path_length > 0 &&
           (size_t)temporary_path_length < temporary_path_size;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Activity Cube</title></head><body>"
        "<h1>Activity Cube</h1><p style='display:flex;gap:1.5em'>"
        "<span>UART counter: <strong id='count'>...</strong></span>"
        "<span>Current time: <strong id='time'>...</strong></span></p>"
        "<script>async function update(){try{const [c,t]=await Promise.all(["
        "fetch('/counter'),fetch('/time')]);"
        "document.getElementById('count').textContent=await c.text();"
        "document.getElementById('time').textContent=await t.text();}"
        "catch(e){document.getElementById('count').textContent='Offline';"
        "document.getElementById('time').textContent='Offline';}}"
        "update();setInterval(update,1000);</script></body></html>";

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t counter_handler(httpd_req_t *request)
{
    char response[16];
    const int length = snprintf(response, sizeof(response), "%lu",
                                (unsigned long)uart_counter_get());

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, response, length);
}

static esp_err_t time_handler(httpd_req_t *request)
{
    char response[32];

    if (!time_is_synchronized) {
        snprintf(response, sizeof(response), "Waiting for NTP");
    } else {
        struct tm local_time;
        const time_t now = current_time;
        if (localtime_r(&now, &local_time) == NULL ||
            strftime(response, sizeof(response), "%Y-%m-%d %H:%M:%S",
                     &local_time) == 0) {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to format current time");
        }
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t file_list_handler(httpd_req_t *request)
{
    DIR *directory = opendir("/littlefs");
    if (directory == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not open LittleFS directory");
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    struct dirent *entry;
    esp_err_t err = ESP_OK;
    int read_error = 0;
    while (true) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            read_error = errno;
            break;
        }

        if (entry->d_name[0] == '.') {
            continue;
        }

        char line[FILE_NAME_MAX_LENGTH + 2];
        const int length = snprintf(line, sizeof(line), "%s\n", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            err = ESP_FAIL;
            break;
        }
        err = httpd_resp_send_chunk(request, line, length);
        if (err != ESP_OK) {
            break;
        }
    }
    if (closedir(directory) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (read_error != 0) {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t file_read_handler(httpd_req_t *request)
{
    char path[FILE_PATH_MAX_LENGTH];
    char temporary_path[FILE_PATH_MAX_LENGTH + 16];
    if (!get_file_paths(request->uri, path, sizeof(path), temporary_path,
                        sizeof(temporary_path))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid file name");
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "File not found");
    }

    httpd_resp_set_type(request, "application/octet-stream");
    char buffer[FILE_IO_BUFFER_SIZE];
    size_t bytes_read;
    esp_err_t err = ESP_OK;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        err = httpd_resp_send_chunk(request, buffer, bytes_read);
        if (err != ESP_OK) {
            fclose(file);
            return err;
        }
    }

    if (ferror(file)) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t file_write_handler(httpd_req_t *request)
{
    char path[FILE_PATH_MAX_LENGTH];
    char temporary_path[FILE_PATH_MAX_LENGTH + 16];
    if (!get_file_paths(request->uri, path, sizeof(path), temporary_path,
                        sizeof(temporary_path))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Invalid file name");
    }
    if (request->content_len > FILE_UPLOAD_MAX_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "File exceeds the 265 KiB upload limit");
    }

    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not open temporary file");
    }

    char buffer[FILE_IO_BUFFER_SIZE];
    size_t remaining = request->content_len;
    while (remaining > 0) {
        const size_t chunk_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const int received = httpd_req_recv(request, buffer, chunk_size);
        if (received <= 0 ||
            fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
            fclose(file);
            remove(temporary_path);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to receive or write file");
        }
        remaining -= (size_t)received;
    }

    if (fclose(file) != 0) {
        remove(temporary_path);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to finish writing file");
    }
    if (rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to save file");
    }

    return httpd_resp_send(request, "File saved", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    err = httpd_register_uri_handler(server, &index_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t counter_uri = {
        .uri = "/counter",
        .method = HTTP_GET,
        .handler = counter_handler,
    };
    err = httpd_register_uri_handler(server, &counter_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t time_uri = {
        .uri = "/time",
        .method = HTTP_GET,
        .handler = time_handler,
    };
    err = httpd_register_uri_handler(server, &time_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t file_list_uri = {
        .uri = "/files",
        .method = HTTP_GET,
        .handler = file_list_handler,
    };
    err = httpd_register_uri_handler(server, &file_list_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t file_read_uri = {
        .uri = "/files/*",
        .method = HTTP_GET,
        .handler = file_read_handler,
    };
    err = httpd_register_uri_handler(server, &file_read_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t file_write_uri = {
        .uri = "/files/*",
        .method = HTTP_PUT,
        .handler = file_write_handler,
    };
    err = httpd_register_uri_handler(server, &file_write_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    return ESP_OK;
}