/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * wifi_cert_loader.c — Load EAP-TLS certificates from LittleFS into the
 * Zephyr TLS credentials store.
 *
 * Usage:
 *   wifi load-certs [path]    (default path: /lfs)
 *
 * The LittleFS image on external QSPI flash must contain:
 *   <path>/ca.pem          — CA certificate (PEM)
 *   <path>/client-key.pem  — Client private key (PEM)
 *   <path>/client.pem      — Client certificate (PEM)
 *
 * sec_tags used (must match wifi_certs.c):
 *   CA cert:     0x1020001  TLS_CREDENTIAL_CA_CERTIFICATE
 *   Client key:  0x1020002  TLS_CREDENTIAL_PRIVATE_KEY
 *   Client cert: 0x1020004  TLS_CREDENTIAL_PUBLIC_CERTIFICATE
 *
 * IMPORTANT: tls_credential_add() stores the caller's pointer directly — it
 * does NOT copy the data.  The buffers (ca_buf, client_key_buf, client_cert_buf)
 * must remain allocated until tls_credential_delete() is called.  They are
 * freed only inside free_cert_bufs(), which is called at the start of the next
 * load or explicitly on unload.
 */

#if defined(CONFIG_WIFI_SHELL_RUNTIME_CERTIFICATES) && defined(CONFIG_FILE_SYSTEM_LITTLEFS)

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/wifi_certs.h>

LOG_MODULE_REGISTER(wifi_cert_loader, LOG_LEVEL_DBG);

/* LittleFS partition and mount point */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cert_lfs);

static struct fs_mount_t cert_mount = {
    .type        = FS_LITTLEFS,
    .fs_data     = &cert_lfs,
    .storage_dev = (void *)FIXED_PARTITION_ID(cert_partition),
    .mnt_point   = "/lfs",
    /* Never auto-format: if flash is blank the user hasn't flashed certs */
    .flags       = FS_MOUNT_FLAG_NO_FORMAT,
};

static bool cert_fs_mounted;

/* Persistent credential buffers — must outlive tls_credential_add() */
static uint8_t *ca_buf;
static uint8_t *client_key_buf;
static uint8_t *client_cert_buf;

/* sec_tags from wifi_certs.c */
#define WIFI_CERT_TAG_CA         0x1020001
#define WIFI_CERT_TAG_CLIENT_KEY 0x1020002
#define WIFI_CERT_TAG_CLIENT_CRT 0x1020004

/* -------------------------------------------------------------------------
 * Private helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Delete a TLS credential if it is registered (ignores -ENOENT).
 */
static void delete_credential_if_exists(sec_tag_t tag,
                                        enum tls_credential_type type)
{
    int ret = tls_credential_delete(tag, type);

    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("tls_credential_delete(tag=0x%x, type=%d): %d",
                (unsigned int)tag, (int)type, ret);
    }
}

/**
 * @brief Delete all three WiFi credentials and free their buffers.
 *
 * Safe to call when no credentials have been loaded yet.
 */
static void free_cert_bufs(void)
{
    delete_credential_if_exists(WIFI_CERT_TAG_CA,
                                TLS_CREDENTIAL_CA_CERTIFICATE);
    k_free(ca_buf);
    ca_buf = NULL;

    delete_credential_if_exists(WIFI_CERT_TAG_CLIENT_KEY,
                                TLS_CREDENTIAL_PRIVATE_KEY);
    k_free(client_key_buf);
    client_key_buf = NULL;

    delete_credential_if_exists(WIFI_CERT_TAG_CLIENT_CRT,
                                TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
    k_free(client_cert_buf);
    client_cert_buf = NULL;
}

/**
 * @brief Read an entire file from the filesystem into a heap buffer.
 *
 * The returned buffer is NUL-terminated.  Caller must free with k_free()
 * on error; on success, ownership is transferred to the caller's persistent
 * slot (buf_keep).
 *
 * @param path    Filesystem path (e.g. "/lfs/ca.pem")
 * @param buf_out Output: pointer to allocated buffer
 * @param len_out Output: number of bytes read (excluding NUL)
 * @return 0 on success, negative errno on failure
 */
static int read_file_to_buf(const char *path, uint8_t **buf_out,
                            size_t *len_out)
{
    struct fs_file_t file;
    struct fs_dirent entry;
    uint8_t *buf;
    ssize_t bytes_read;
    int ret;

    ret = fs_stat(path, &entry);
    if (ret < 0) {
        LOG_ERR("fs_stat(%s) failed: %d", path, ret);
        return ret;
    }

    if (entry.type != FS_DIR_ENTRY_FILE) {
        LOG_ERR("%s is not a regular file", path);
        return -EISDIR;
    }

    if (entry.size == 0) {
        LOG_ERR("%s is empty", path);
        return -ENODATA;
    }

    buf = k_malloc(entry.size + 1);
    if (!buf) {
        LOG_ERR("k_malloc(%zu) failed for %s", entry.size + 1, path);
        return -ENOMEM;
    }

    fs_file_t_init(&file);
    ret = fs_open(&file, path, FS_O_READ);
    if (ret < 0) {
        LOG_ERR("fs_open(%s) failed: %d", path, ret);
        k_free(buf);
        return ret;
    }

    bytes_read = fs_read(&file, buf, entry.size);
    fs_close(&file);

    if (bytes_read < 0) {
        LOG_ERR("fs_read(%s) failed: %zd", path, bytes_read);
        k_free(buf);
        return (int)bytes_read;
    }

    if ((size_t)bytes_read != entry.size) {
        LOG_WRN("fs_read(%s): expected %zu bytes, got %zd",
                path, entry.size, bytes_read);
    }

    buf[bytes_read] = '\0'; /* NUL-terminate for PEM parsing */
    *buf_out = buf;
    *len_out = (size_t)bytes_read;

    LOG_DBG("read %zd bytes from %s", bytes_read, path);
    return 0;
}

/**
 * @brief Read a PEM file and register it as a TLS credential.
 *
 * On success, the buffer is stored in *buf_keep and must not be freed until
 * tls_credential_delete() has been called.
 *
 * @param path     Filesystem path
 * @param tag      sec_tag_t to register under
 * @param type     Credential type
 * @param buf_keep Persistent buffer slot (freed + replaced on re-load)
 * @return 0 on success, negative errno on failure
 */
static int load_credential(const char *path, sec_tag_t tag,
                           enum tls_credential_type type,
                           uint8_t **buf_keep)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    int ret;

    ret = read_file_to_buf(path, &buf, &len);
    if (ret < 0) {
        return ret;
    }

    delete_credential_if_exists(tag, type);
    if (buf_keep && *buf_keep) {
        k_free(*buf_keep);
        *buf_keep = NULL;
    }

    /*
     * Pass len+1 so mbedtls_x509_crt_parse() sees the NUL terminator —
     * crt_is_pem() checks buf[buflen-1] == '\0'.
     */
    ret = tls_credential_add(tag, type, buf, len + 1);
    if (ret < 0) {
        LOG_ERR("tls_credential_add(tag=0x%x, type=%d) failed: %d",
                (unsigned int)tag, (int)type, ret);
        k_free(buf);
        return ret;
    }

    if (buf_keep) {
        *buf_keep = buf;
    }

    LOG_INF("loaded credential: tag=0x%x type=%d from %s (%zu bytes)",
            (unsigned int)tag, (int)type, path, len);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Load EAP-TLS certificates from a LittleFS directory.
 *
 * Mounts LittleFS (lazily, on first call) then reads ca.pem,
 * client-key.pem, and client.pem from @p dir, registering them in the
 * TLS credentials store.
 *
 * @param dir LittleFS directory (e.g. "/lfs")
 * @return 0 on success, negative errno on failure
 */
int wifi_load_certs_from_fs(const char *dir)
{
    char path[128];
    int ret;

    if (!dir || strlen(dir) == 0) {
        return -EINVAL;
    }

    /* Lazy-mount LittleFS on first call */
    if (!cert_fs_mounted) {
        ret = fs_mount(&cert_mount);
        if (ret < 0) {
            LOG_ERR("fs_mount(/lfs) failed: %d — check that certs are flashed "
                    "to QSPI offset 0x3000", ret);
            return ret;
        }
        cert_fs_mounted = true;
        LOG_INF("LittleFS mounted at /lfs");
    }

    /* Release any previously loaded certs */
    free_cert_bufs();

    /* CA certificate */
    snprintf(path, sizeof(path), "%s/ca.pem", dir);
    ret = load_credential(path, WIFI_CERT_TAG_CA,
                          TLS_CREDENTIAL_CA_CERTIFICATE, &ca_buf);
    if (ret < 0) {
        free_cert_bufs();
        return ret;
    }

    /* Client private key */
    snprintf(path, sizeof(path), "%s/client-key.pem", dir);
    ret = load_credential(path, WIFI_CERT_TAG_CLIENT_KEY,
                          TLS_CREDENTIAL_PRIVATE_KEY, &client_key_buf);
    if (ret < 0) {
        free_cert_bufs();
        return ret;
    }

    /* Client certificate */
    snprintf(path, sizeof(path), "%s/client.pem", dir);
    ret = load_credential(path, WIFI_CERT_TAG_CLIENT_CRT,
                          TLS_CREDENTIAL_PUBLIC_CERTIFICATE, &client_cert_buf);
    if (ret < 0) {
        free_cert_bufs();
        return ret;
    }

    LOG_INF("All three credentials loaded from %s", dir);
    return 0;
}

#endif /* CONFIG_WIFI_SHELL_RUNTIME_CERTIFICATES && CONFIG_FILE_SYSTEM_LITTLEFS */
