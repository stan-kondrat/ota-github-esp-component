#pragma once

#include <stdbool.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OTA_GITHUB_releases_SIZE
#define OTA_GITHUB_releases_SIZE 10
#endif


typedef struct ota_github_config_t {
    uint8_t github_user[64];
    uint8_t github_repo[64];
    uint8_t filename[64];
    uint8_t current_version[32];
    int64_t release_id;
    bool newer;
    bool latest;
    bool prerelease;
} __attribute__((packed)) ota_github_config_t;

typedef struct ota_github_release_asset_t {
    uint8_t name[64];
    uint8_t url[256];
} __attribute__((packed)) ota_github_release_asset_t;

typedef struct ota_github_release_t {
    int64_t id;
    uint8_t name[64];
    uint8_t tag_name[32];
    uint8_t created_at[32];
    uint8_t download_url[256];
    bool prerelease;
} __attribute__((packed)) ota_github_release_t;

typedef struct ota_github_releases_t {
    ota_github_release_t releases[OTA_GITHUB_releases_SIZE];
    int size;
} __attribute__((packed)) ota_github_releases_t;


ota_github_releases_t* ota_github_get_releases(const ota_github_config_t *);

esp_err_t ota_github_install_latest(ota_github_config_t *);

#ifdef __cplusplus
}
#endif
