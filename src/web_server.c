#include "web_server.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "mpu6050.h"
#include "orientation_history.h"
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
        "<title>Activity Cube</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;margin:2rem;min-height:calc(100vh - 4rem);"
        "background:#f1f5f9;transition:background-color .4s ease}"
        "body[data-position='1']{background:#ffe4e6}"
        "body[data-position='2']{background:#dcfce7}"
        "body[data-position='3']{background:#fef9c3}"
        "body[data-position='4']{background:#f3e8ff}"
        "body[data-position='5']{background:#dbeafe}"
        "body[data-position='6']{background:#f8fafc}"
        ".status{display:inline-block;padding:.35rem .7rem;border-radius:999px;"
        "background:#e2e8f0;font-weight:600}"
        "iframe{width:100%;height:24rem;border:1px solid #cbd5e1;background:white}"
        "</style></head><body data-position='unknown'>"
        "<h1>Activity Cube</h1><p style='display:flex;gap:1.5em'>"
        "<span>UART counter: <strong id='count'>...</strong></span>"
        "<span>Current time: <strong id='time'>...</strong></span></p>"
        "<h2>MPU6050 orientation</h2>"
        "<p>Cube position: <strong id='position' class='status' aria-live='polite'>...</strong></p>"
        "<p>Roll: <strong id='roll'>...</strong> degrees</p>"
        "<p>Pitch: <strong id='pitch'>...</strong> degrees</p>"
        "<p>Yaw: <strong id='yaw'>...</strong> degrees</p>"
        "<h2>Orientation history</h2>"
        "<p>Position changes are recorded; unclear orientation is shown as such. "
        "Times are device-local after NTP synchronization, otherwise uptime.</p>"
        "<button type='button' onclick=\"document.getElementById('history').src="
        "'/history?refresh='+Date.now()\">Refresh history</button>"
        "<button type='button' onclick='clearHistory()'>Clear history</button>"
        "<span id='history-status' role='status' aria-live='polite'></span>"
        "<iframe id='history' title='Orientation history' src='/history'></iframe>"
        "<script>async function clearHistory(){const status=document.getElementById('history-status');"
        "if(!confirm('Clear the orientation history?'))return;"
        "status.textContent='Clearing...';"
        "try{const response=await fetch('/history',{method:'DELETE'});"
        "if(!response.ok)throw Error(await response.text());"
        "document.getElementById('history').src='/history?refresh='+Date.now();"
        "status.textContent='History cleared.';}"
        "catch(e){status.textContent='Could not clear history: '+e.message;}}"
        "function getPosition(a){const r=a.roll,p=a.pitch;"
        "if(p>=-15&&p<=15&&r>=75&&r<=105)return 1;"
        "if(p>=-105&&p<=-75)return 2;"
        "if(p>=-15&&p<=15&&r>=-15&&r<=15)return 3;"
        "if(p>=75&&p<=105)return 4;"
        "if(p>=-15&&p<=15&&((r>=165&&r<=180)||(r>=-180&&r<=-165)))return 5;"
        "if(p>=-15&&p<=15&&r>=-105&&r<=-75)return 6;"
        "return 0;}"
        "function setPosition(position,label){const page=document.body;"
        "page.dataset.position=position||'unknown';"
        "document.getElementById('position').textContent=label||"
        "(position?'Position '+position:'Unknown');}"
        "async function update(){try{const [c,t,o]=await Promise.all(["
        "fetch('/counter'),fetch('/time'),fetch('/orientation')]);"
        "if(!c.ok||!t.ok||!o.ok)throw Error('Request failed');"
        "document.getElementById('count').textContent=await c.text();"
        "document.getElementById('time').textContent=await t.text();"
        "const angles=await o.json();"
        "document.getElementById('roll').textContent=angles.available?angles.roll.toFixed(2):'Unavailable';"
        "document.getElementById('pitch').textContent=angles.available?angles.pitch.toFixed(2):'Unavailable';"
        "document.getElementById('yaw').textContent=angles.available?angles.yaw.toFixed(2):'Unavailable';"
        "if(angles.available){setPosition(getPosition(angles));}"
        "else{setPosition(0,'Unavailable');}}"
        "catch(e){document.getElementById('count').textContent='Offline';"
        "document.getElementById('time').textContent='Offline';"
        "document.getElementById('roll').textContent='Offline';"
        "document.getElementById('pitch').textContent='Offline';"
        "document.getElementById('yaw').textContent='Offline';"
        "document.getElementById('position').textContent='Offline';"
        "document.body.dataset.position='unknown';}}"
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

static esp_err_t orientation_handler(httpd_req_t *request)
{
    mpu6050_orientation_t orientation;
    const esp_err_t err = mpu6050_get_orientation(&orientation);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        return httpd_resp_send(request, "{\"available\":false}", HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to read MPU6050 orientation");
    }

    char response[128];
    const int length = snprintf(response, sizeof(response),
                                "{\"available\":true,\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
                                orientation.roll, orientation.pitch, orientation.yaw);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to format MPU6050 orientation");
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, response, length);
}

static const char *orientation_name(int position)
{
    static const char *const names[] = {
        "Unclear", "Position 1", "Position 2", "Position 3",
        "Position 4", "Position 5", "Position 6",
    };
    return position >= 0 && position <= 6 ? names[position] : NULL;
}

static esp_err_t history_send_chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t history_handler(httpd_req_t *request)
{
    FILE *file = fopen(ORIENTATION_HISTORY_FILE, "r");
    if (file == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not open orientation history");
    }

    esp_err_t err = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    err = history_send_chunk(
        request, "<!doctype html><html><head><meta charset='utf-8'>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<style>body{font-family:system-ui,sans-serif;margin:.5rem}"
                 "table{border-collapse:collapse;width:100%}"
                 "th,td{text-align:left;padding:.5rem;border-bottom:1px solid #ddd}"
                 "thead{position:sticky;top:0;background:#f1f5f9}</style>"
                 "</head><body><table><thead><tr><th>Timestamp</th>"
                 "<th>Orientation</th></tr></thead><tbody>");
    char line[96];
    while (err == ESP_OK && fgets(line, sizeof(line), file) != NULL) {
        char timestamp[64];
        int position;
        int consumed = 0;
        if (sscanf(line, "%63[^,],%d%n", timestamp, &position, &consumed) != 2 ||
            orientation_name(position) == NULL) {
            continue;
        }

        const char *tail = line + consumed;
        if (*tail == '\r') {
            ++tail;
        }
        if (*tail == '\n') {
            ++tail;
        }
        if (*tail != '\0') {
            continue;
        }

        char escaped_timestamp[256];
        size_t output_length = 0;
        for (const char *source = timestamp; *source != '\0'; ++source) {
            const char *replacement = NULL;
            switch (*source) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default:
                if (output_length + 1 >= sizeof(escaped_timestamp)) {
                    err = ESP_FAIL;
                } else {
                    escaped_timestamp[output_length++] = *source;
                }
                continue;
            }
            const size_t replacement_length = strlen(replacement);
            if (output_length + replacement_length >= sizeof(escaped_timestamp)) {
                err = ESP_FAIL;
                break;
            }
            memcpy(&escaped_timestamp[output_length], replacement, replacement_length);
            output_length += replacement_length;
        }
        if (err != ESP_OK) {
            break;
        }
        escaped_timestamp[output_length] = '\0';

        char row[384];
        const int length = snprintf(row, sizeof(row),
                                    "<tr><td>%s</td><td>%s</td></tr>",
                                    escaped_timestamp, orientation_name(position));
        if (length < 0 || (size_t)length >= sizeof(row)) {
            err = ESP_FAIL;
            break;
        }
        err = httpd_resp_send_chunk(request, row, length);
    }

    if (ferror(file) && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        return err;
    }

    return history_send_chunk(request, "</tbody></table></body></html>") == ESP_OK
               ? httpd_resp_send_chunk(request, NULL, 0)
               : ESP_FAIL;
}

static esp_err_t history_clear_handler(httpd_req_t *request)
{
    const esp_err_t err = orientation_history_clear();
    if (err != ESP_OK) {
        ESP_LOGE("web_server", "Failed to clear orientation history: %s",
                 esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to clear orientation history");
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, "History cleared",
                           HTTPD_RESP_USE_STRLEN);
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
    config.max_uri_handlers = 10;
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

    const httpd_uri_t history_clear_uri = {
        .uri = "/history",
        .method = HTTP_DELETE,
        .handler = history_clear_handler,
    };
    err = httpd_register_uri_handler(server, &history_clear_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return err;
    }

    const httpd_uri_t history_uri = {
        .uri = "/history",
        .method = HTTP_GET,
        .handler = history_handler,
    };
    err = httpd_register_uri_handler(server, &history_uri);
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

    const httpd_uri_t orientation_uri = {
        .uri = "/orientation",
        .method = HTTP_GET,
        .handler = orientation_handler,
    };
    err = httpd_register_uri_handler(server, &orientation_uri);
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