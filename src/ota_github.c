#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>

#include "errno.h"
#include "lwjson/lwjson.h"
#include "ota.h"
#include "semver.h"

#include "ota_github.h"

#define MAX_HTTP_RECV_BUFFER 256
#define OTA_GUTHUB_MAX_URL_LENGTH 256
#define OTA_GUTHUB_API_URL "https://api.github.com/repos/"

#define CONFIG_EXAMPLE_OTA_RECV_TIMEOUT 7000
// #define CONFIG_LOG_MAXIMUM_LEVEL 4

static ota_github_config_t *g_ota_github_config = NULL;
static ota_github_releases_t *g_ota_github_releases = NULL;

esp_err_t _ota_github_stream_parse(lwjson_stream_parser_t *jsp, char *json_str, int size);
void _ota_github_stream_callback(lwjson_stream_parser_t *jsp, lwjson_stream_type_t type);

static const char *TAG = "OTA_GUTHUB";

esp_err_t ota_github_install_latest(ota_github_config_t *github_config) {
    github_config->latest = true;

    ota_github_releases_t releases = {0};
    esp_err_t err = ota_github_get_releases(github_config, &releases);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Get Releases Error");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "releases->size=%d", releases.size);
    if (releases.size != 1) {
        ESP_LOGW(TAG, "Release id not found %lld \n", github_config->release_id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Download url: %s \n", releases.releases[0].download_url);

    ota_install((uint8_t *)releases.releases[0].download_url);

    return ESP_OK;
}

esp_err_t ota_github_get_releases(ota_github_config_t *ota_github_config, ota_github_releases_t *ota_github_releases) {
    if (g_ota_github_config != NULL || g_ota_github_releases != NULL) {
        ESP_LOGW(TAG, "Ger Releases Already Running");
        return ESP_FAIL;
    }
    g_ota_github_config = ota_github_config;
    g_ota_github_releases = ota_github_releases;

    char url[OTA_GUTHUB_MAX_URL_LENGTH] = {0};
    strcat(url, OTA_GUTHUB_API_URL);
    strcat(url, (char *)ota_github_config->github_user);
    strcat(url, "/");
    strcat(url, (char *)ota_github_config->github_repo);
    strcat(url, "/releases");

    if (ota_github_config->latest) {
        strcat(url, "/latest");
    }

    ESP_LOGI(TAG, "API URL %s", url);
    esp_http_client_config_t http_config = {
        .url = url,
        .use_global_ca_store = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");

    int content_length = 0;

    // Follow redirects
    while (true) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return false;
        }
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
            esp_http_client_cleanup(client);
            return false;
        }
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(
            TAG, "HTTP Stream reader Status = %d, content_length = %lld", status_code,
            esp_http_client_get_content_length(client)
        );
        // has redirects
        if (status_code >= 300 && status_code < 400) {
            esp_http_client_set_redirection(client);
            // clear the response buffer of http_client
            char *buffer = (char *)malloc(content_length);
            esp_http_client_read(client, buffer, content_length);
            free(buffer);
            continue;
        }
        // no more redirects
        break;
    }

    char buffer[MAX_HTTP_RECV_BUFFER + 1] = {0};

    lwjson_stream_parser_t stream_parser;
    lwjson_stream_init(&stream_parser, _ota_github_stream_callback);

    if (ota_github_config->latest) {
        _ota_github_stream_parse(&stream_parser, "[", 1);
    }
    while (1) {
        int len_to_read = content_length < MAX_HTTP_RECV_BUFFER ? (int)content_length : MAX_HTTP_RECV_BUFFER;
        int data_read_len = esp_http_client_read(client, buffer, len_to_read);
        if (data_read_len < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            break;
        } else if (data_read_len > 0) {
            buffer[data_read_len] = 0;
            ESP_LOGD(TAG, "_ota_github_stream_parse %d\n %s", data_read_len, buffer);
            if (_ota_github_stream_parse(&stream_parser, buffer, data_read_len) != ESP_OK) {
                break;
            }
        } else if (data_read_len == 0) {
            /*
             * As esp_http_client_read never returns negative error code, we rely on
             * `errno` to check for underlying transport connectivity closure if any
             */
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
            }
            if (esp_http_client_is_complete_data_received(client) == true) {
                ESP_LOGI(TAG, "Connection closed");
            }
            break;
        }
    }

    if (ota_github_config->latest) {
        _ota_github_stream_parse(&stream_parser, "]", 1);
    }

    lwjson_stream_reset(&stream_parser);
    esp_http_client_cleanup(client);
    g_ota_github_config = NULL;
    g_ota_github_releases = NULL;
    return ESP_OK;
}

/* Parse JSON */
esp_err_t _ota_github_stream_parse(lwjson_stream_parser_t *jsp, char *json_str, int size) {
    lwjsonr_t res;
    int i = 0;
    for (const char *c = json_str; *c != '\0'; ++c, i++) {
        res = lwjson_stream_parse(jsp, *c);
        if (res == lwjsonSTREAMINPROG) {
            // Stream parsing is still in progress
        } else if (res == lwjsonSTREAMWAITFIRSTCHAR) {
            // Waiting first character
        } else if (res == lwjsonSTREAMDONE) {
            // Done
        } else {
            // Error
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

void _ota_github_stream_callback(lwjson_stream_parser_t *jsp, lwjson_stream_type_t type) {
    static ota_github_release_t release = {0};
    static ota_github_release_asset_t asset = {0};

    ESP_LOGV(
        TAG, "%d. Got type %d key '%s' with value '%s'", jsp->stack_pos, type, jsp->stack[jsp->stack_pos - 1].meta.name,
        jsp->data.str.buff
    );

    // [0 {1 "created_at"2: "..", "assets"2: [3 {4 "name"5: "..", "browser_download_url"5: ".."}4 ]3 }1 ]0
    if (jsp->stack_pos >= 3 && jsp->stack[0].type == LWJSON_STREAM_TYPE_ARRAY &&
        jsp->stack[1].type == LWJSON_STREAM_TYPE_OBJECT && jsp->stack[2].type == LWJSON_STREAM_TYPE_KEY) {
        if (jsp->stack_pos == 3) {
            if (type == LWJSON_STREAM_TYPE_NUMBER && strcmp(jsp->stack[2].meta.name, "id") == 0) {
                release.id = atoll((char *)jsp->data.str.buff);
                ESP_LOGD(TAG, "=== release.id=%lld", release.id);
            }
            if (type == LWJSON_STREAM_TYPE_STRING && strcmp(jsp->stack[2].meta.name, "name") == 0) {
                strncpy((char *)release.name, (char *)jsp->data.str.buff, sizeof(release.name));
                ESP_LOGD(TAG, "=== release.name=%s", release.name);
            }
            if (type == LWJSON_STREAM_TYPE_STRING && strcmp(jsp->stack[2].meta.name, "tag_name") == 0) {
                strncpy((char *)release.tag_name, (char *)jsp->data.str.buff, sizeof(release.tag_name));
                ESP_LOGD(TAG, "=== release.tag_name=%s", release.tag_name);
            }
            if (type == LWJSON_STREAM_TYPE_STRING && strcmp(jsp->stack[2].meta.name, "created_at") == 0) {
                strncpy((char *)release.created_at, (char *)jsp->data.str.buff, sizeof(release.created_at));
                ESP_LOGD(TAG, "=== release.created_at=%s", release.created_at);
            }
            if (type == LWJSON_STREAM_TYPE_TRUE && strcmp(jsp->stack[2].meta.name, "prerelease") == 0) {
                release.prerelease = true;
                ESP_LOGD(TAG, "=== release.prerelease=%d", release.prerelease);
            }
        }

        if (jsp->stack_pos >= 6 && strcmp(jsp->stack[2].meta.name, "assets") == 0 &&
            jsp->stack[3].type == LWJSON_STREAM_TYPE_ARRAY && jsp->stack[4].type == LWJSON_STREAM_TYPE_OBJECT &&
            jsp->stack[5].type == LWJSON_STREAM_TYPE_KEY) {
            if (type == LWJSON_STREAM_TYPE_STRING && strcmp(jsp->stack[5].meta.name, "name") == 0) {
                strncpy((char *)asset.name, (char *)jsp->data.str.buff, sizeof(asset.name));
                ESP_LOGD(TAG, "====== asset.name=%s", asset.name);
            }
            if (type == LWJSON_STREAM_TYPE_STRING && strcmp(jsp->stack[5].meta.name, "browser_download_url") == 0) {
                strncpy((char *)asset.url, (char *)jsp->data.str.buff, sizeof(asset.url));
                ESP_LOGD(TAG, "====== asset.url=%s", asset.url);
            }
        }
    }

    if (type == LWJSON_STREAM_TYPE_OBJECT_END && (*jsp).stack_pos == 4) {
        ESP_LOGD(TAG, "Finish object item in assets, %s = %s", asset.name, asset.url);
        if (asset.url[0] != '\0' && strcmp((char *)asset.name, (char *)g_ota_github_config->filename) == 0) {
            strncpy((char *)release.download_url, (char *)asset.url, sizeof(asset.url));
            ESP_LOGD(TAG, "=========== release.download_url=%s", release.download_url);
        }
        asset = (ota_github_release_asset_t){0};
    }

    if (type == LWJSON_STREAM_TYPE_OBJECT_END && (*jsp).stack_pos == 1) {
        ESP_LOGI(TAG, "Finish object item in releases array:");
        ESP_LOGI(TAG, "  id = %lld", release.id);
        ESP_LOGI(TAG, "  name = %s", release.name);
        ESP_LOGI(TAG, "  tag_name = %s", release.tag_name);
        ESP_LOGI(TAG, "  created_at = %s", release.created_at);
        ESP_LOGI(TAG, "  download_url = %s", release.download_url);
        ESP_LOGI(TAG, "  prerelease = %d", release.prerelease);

        if (release.download_url[0] != '\0' && g_ota_github_releases->size < OTA_GITHUB_RELEASES_SIZE) {
            bool prerelease_check = true;
            if (g_ota_github_config->prerelease == true && release.prerelease != true) {
                prerelease_check = false;
            }
            if (g_ota_github_config->prerelease != true && release.prerelease == true) {
                prerelease_check = false;
            }

            bool newer_check = true;
            if (g_ota_github_config->newer) {
                semver_t current_version = {};
                semver_t compare_version = {};
                char *current = (strncmp((char *)g_ota_github_config->current_version, "v", 1) == 0)
                                    ? (char *)(g_ota_github_config->current_version + 1)
                                    : (char *)g_ota_github_config->current_version;
                char *compare = (strncmp((char *)release.tag_name, "v", 1) == 0) ? (char *)(release.tag_name + 1)
                                                                                 : (char *)release.tag_name;
                if (semver_parse(current, &current_version) || semver_parse(compare, &compare_version)) {
                    newer_check = false;
                } else {
                    int resolution = semver_compare(compare_version, current_version);
                    newer_check = resolution > 0;
                }
            }

            bool release_check = true;
            if (g_ota_github_config->release_id && g_ota_github_config->release_id != release.id) {
                release_check = false;
            }

            ESP_LOGI(TAG, "=== newer=%d, prerelease=%d, release=%d", newer_check, prerelease_check, release_check);
            if (newer_check && prerelease_check && release_check && release.id != '\0') {
                memcpy(
                    &g_ota_github_releases->releases[g_ota_github_releases->size], &release,
                    sizeof(struct ota_github_release_t)
                );
                g_ota_github_releases->size++;
            }
            ESP_LOGI(TAG, "=== g_ota_github_releases->size=%d", g_ota_github_releases->size);
        }
        release = (ota_github_release_t){0};
    }
}
