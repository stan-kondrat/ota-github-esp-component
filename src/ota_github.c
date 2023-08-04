#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

#include "lwjson/lwjson.h"
#include "semver.h"
#include "ota.h"

#include "ota_github.h"

#define MAX_HTTP_RECV_BUFFER 512
#define OTA_GUTHUB_MAX_URL_LENGTH 256
#define OTA_GUTHUB_API_URL "https://api.github.com/repos/"

#define CONFIG_EXAMPLE_OTA_RECV_TIMEOUT 7000

ota_github_config_t ota_github_config = {0};
ota_github_release_t ota_github_release = {0};
ota_github_releases_t ota_github_releases = {0};
ota_github_release_asset_t ota_github_release_asset = {0};

esp_err_t ota_github_stream_parse(lwjson_stream_parser_t *jsp, char * json_str, int size);
void ota_github_stream_callback(lwjson_stream_parser_t *jsp, lwjson_stream_type_t type);

static const char *TAG = "OTA_GUTHUB";

esp_err_t ota_github_install_latest(ota_github_config_t * github_config) {
    github_config->latest = true;

    ota_github_releases_t* releases = ota_github_get_releases(github_config);
    ESP_LOGI(TAG, "releases->size=%d", releases->size);
    if (releases->size != 1) {
        ESP_LOGI(TAG, "Release id not found %lld \n", github_config->release_id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Download url: %s \n", releases->releases[0].download_url);

    ota_install((uint8_t *)releases->releases[0].download_url);
    
    return ESP_OK;
}

ota_github_releases_t* ota_github_get_releases(const ota_github_config_t * github_config) {
    memcpy(&ota_github_config, github_config, sizeof(ota_github_config));

    ota_github_release = (ota_github_release_t){0};
    ota_github_releases = (ota_github_releases_t){0};
    ota_github_release_asset = (ota_github_release_asset_t){0};

    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return &ota_github_releases;
    }

    char url[OTA_GUTHUB_MAX_URL_LENGTH] = {0};
    strcat(url, OTA_GUTHUB_API_URL);
    strcat(url, (char *)ota_github_config.github_user);
    strcat(url, "/");
    strcat(url, (char *)ota_github_config.github_repo);
    strcat(url, "/releases");

    if (ota_github_config.latest) {
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

    ESP_LOGI(TAG, "esp_http_client_init");

    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        return &ota_github_releases;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "content_length = %lld", content_length);

    ESP_LOGI(
        TAG, "HTTP Stream reader Status = %d, content_length = %lld", esp_http_client_get_status_code(client),
        esp_http_client_get_content_length(client)
    );

    static lwjson_stream_parser_t stream_parser;
    lwjson_stream_init(&stream_parser, ota_github_stream_callback);

    if (ota_github_config.latest) {
        ota_github_stream_parse(&stream_parser, "[", 1);
    }
    while (1) {
        int len_to_read = content_length < MAX_HTTP_RECV_BUFFER ? (int)content_length : MAX_HTTP_RECV_BUFFER;
        int data_read_len = esp_http_client_read(client, buffer, len_to_read);
        if (data_read_len < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            break;
        } else if (data_read_len > 0) {
            buffer[data_read_len] = 0;
            if (ota_github_stream_parse(&stream_parser, buffer, data_read_len) != ESP_OK) {
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

    if (ota_github_config.latest) {
        ota_github_stream_parse(&stream_parser, "]", 1);
    }

    lwjson_stream_reset(&stream_parser);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    return &ota_github_releases;
}


/* Parse JSON */
esp_err_t ota_github_stream_parse(lwjson_stream_parser_t *jsp, char * json_str, int size) {
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


void ota_github_stream_callback(lwjson_stream_parser_t *jsp, lwjson_stream_type_t type) {
    // [0 {1 "created_at"2: "..", "assets"2: [3 {4 "name"5: "..", "browser_download_url"5: ".."}4 ]3 }1 ]0
    if (jsp->stack_pos >= 3
        && jsp->stack[0].type == LWJSON_STREAM_TYPE_ARRAY 
        && jsp->stack[1].type == LWJSON_STREAM_TYPE_OBJECT
        && jsp->stack[2].type == LWJSON_STREAM_TYPE_KEY
    ) {
        // ESP_LOGI(TAG, "Got key '%s' with value '%s'\r\n", jsp->stack[2].meta.name, jsp->data.str.buff);
        if (strcmp(jsp->stack[2].meta.name, "id") == 0 && type == LWJSON_STREAM_TYPE_NUMBER) {
            ota_github_release.id = atoll((char *)jsp->data.str.buff);
        }
        if (strcmp(jsp->stack[2].meta.name, "name") == 0) {
            strncpy((char *)ota_github_release.name, (char *)jsp->data.str.buff, sizeof(ota_github_release.name));
        }
        if (strcmp(jsp->stack[2].meta.name, "tag_name") == 0) {
            strncpy(
                (char *)ota_github_release.tag_name, (char *)jsp->data.str.buff,
                sizeof(ota_github_release.tag_name)
            );
        }
        if (strcmp(jsp->stack[2].meta.name, "created_at") == 0) {
            strncpy(
                (char *)ota_github_release.created_at, (char *)jsp->data.str.buff,
                sizeof(ota_github_release.created_at)
            );
        }
        if (strcmp(jsp->stack[2].meta.name, "prerelease") == 0 && type == LWJSON_STREAM_TYPE_TRUE) {
            ota_github_release.prerelease = true;
        }
        if (jsp->stack_pos >= 6
            && strcmp(jsp->stack[2].meta.name, "assets") == 0
            && jsp->stack[3].type == LWJSON_STREAM_TYPE_ARRAY
            && jsp->stack[4].type == LWJSON_STREAM_TYPE_OBJECT 
            && jsp->stack[5].type == LWJSON_STREAM_TYPE_KEY
        ){
            if (strcmp(jsp->stack[5].meta.name, "name") == 0) {
                strncpy(
                    (char *)ota_github_release_asset.name, (char *)jsp->data.str.buff,
                    sizeof(ota_github_release_asset.name)
                );
            }
            if (strcmp(jsp->stack[5].meta.name, "browser_download_url") == 0) {
                strncpy(
                    (char *)ota_github_release_asset.url, (char *)jsp->data.str.buff,
                    sizeof(ota_github_release_asset.url)
                );
            }
        }
    }

    // finish item in assets array
    if (type == LWJSON_STREAM_TYPE_OBJECT_END && (*jsp).stack_pos == 4) {
        // ESP_LOGI(TAG, "4 LWJSON_STREAM_TYPE_OBJECT_END %s / %s ", ota_github_release_asset.name, ota_github_release_asset.url);
        if (ota_github_release_asset.url[0] == '\0'
            && strcmp((char *)ota_github_release_asset.name, (char *)ota_github_config.filename) == 0
        ) {
            strncpy(
                (char *)ota_github_release.download_url, (char *)ota_github_release_asset.url, 
                sizeof(ota_github_release_asset.url)
            );
        }
        ota_github_release_asset = (ota_github_release_asset_t){0};
    }

    // finish item in releases array
    if (type == LWJSON_STREAM_TYPE_OBJECT_END && (*jsp).stack_pos == 1) {
        if (ota_github_releases.size < OTA_GITHUB_releases_SIZE 
            && ota_github_release.download_url[0] != '\0'
        ) {
            bool prerelease_ok = true;
            if (ota_github_config.prerelease == true && ota_github_release.prerelease != true) {
                prerelease_ok = false;
            }
            if (ota_github_config.prerelease != true && ota_github_release.prerelease == true) {
                prerelease_ok = false;
            }

            bool newer_ok = true;
            if (ota_github_config.newer) {
                semver_t current_version = {};
                semver_t compare_version = {};
                char* current = (strncmp((char *)ota_github_config.current_version, "v", 1) == 0)
                    ? (char *)(ota_github_config.current_version + 1) 
                    : (char *)ota_github_config.current_version;     
                char* compare = (strncmp((char *)ota_github_release.tag_name, "v", 1) == 0)
                    ? (char *)(ota_github_release.tag_name + 1) 
                    : (char *)ota_github_release.tag_name;
                if (semver_parse(current, &current_version) || semver_parse(compare, &compare_version)) {
                    newer_ok = false;
                } else {
                    int resolution = semver_compare(compare_version, current_version);
                    newer_ok = resolution > 0;
                }
            }

            bool release_ok = true;
            if (ota_github_config.release_id && ota_github_config.release_id != ota_github_release.id) {
                release_ok = false;
            }

            if (newer_ok && prerelease_ok && release_ok) {
                memcpy(
                    &ota_github_releases.releases[ota_github_releases.size], &ota_github_release,
                    sizeof(ota_github_release)
                );
                ota_github_releases.size++;
            }
        }
        ota_github_release = (ota_github_release_t){0};
    }
}
