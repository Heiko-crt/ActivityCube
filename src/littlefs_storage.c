#include "littlefs_storage.h"

#include "esp_littlefs.h"

esp_err_t littlefs_storage_init(void)
{
    const esp_vfs_littlefs_conf_t config = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    return esp_vfs_littlefs_register(&config);
}
