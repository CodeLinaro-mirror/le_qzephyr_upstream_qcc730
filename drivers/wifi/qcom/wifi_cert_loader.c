/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * wifi_cert_loader.c — Load EAP-TLS certificates from LittleFS into the
 * Zephyr TLS credentials store.
 *
 * Usage:
 *   wifi load_certs [-c <ca_path>] [-k <key_path>] [-e <cert_path>] [-p <pass>]
 *   wifi load_certs --list
 *   wifi load_certs --clear
 *
 * sec_tags used (must match wifi_certs.c):
 *   CA cert:     0x1020001  TLS_CREDENTIAL_CA_CERTIFICATE
 *   Client key:  0x1020002  TLS_CREDENTIAL_PRIVATE_KEY
 *   Client cert: 0x1020004  TLS_CREDENTIAL_PUBLIC_CERTIFICATE
 *
 * IMPORTANT: tls_credential_add() stores the caller's pointer directly — it
 * does NOT copy the data.  The buffers (ca_buf, client_key_buf, client_cert_buf)
 * must remain allocated until tls_credential_delete() is called.  They are
 * freed only inside free_cert_bufs(), which is called on clear or re-load.
 *
 * Encrypted private keys: parsed with the user-supplied passphrase via mbedTLS,
 * then re-encoded as unencrypted DER before storage.  Zephyr's TLS layer
 * (sockets_tls.c) calls mbedtls_pk_parse_key() with pwd=NULL, so the stored
 * key must always be unencrypted.
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
#include <zephyr/shell/shell.h>
#include <zephyr/random/random.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>

LOG_MODULE_REGISTER(wifi_cert_loader, LOG_LEVEL_DBG);

/*
 * Compatibility shim: mbedtls_ecp_write_key_ext() was added in mbedTLS 3.1
 * but is absent from libcryptoqcc730.a (which only provides the older
 * mbedtls_ecp_write_key()).  pkwrite.c references it, causing a link error
 * whenever mbedtls_pk_write_key_der() is pulled into the build.
 *
 * This project uses RSA keys for EAP-TLS; the ECC path in pk_write_key_der()
 * is never reached.  The stub satisfies the linker and returns an explicit
 * error if unexpectedly called with an ECC key.
 */
int __attribute__((weak))
mbedtls_ecp_write_key_ext(const mbedtls_ecp_keypair *key,
                           size_t *olen, unsigned char *buf, size_t buflen)
{
    ARG_UNUSED(key);
    ARG_UNUSED(buf);
    ARG_UNUSED(buflen);
    *olen = 0;
    return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
}

/* Maximum passphrase length accepted */
#define KEY_PASS_MAX_LEN 128

/* Temporary DER buffer size for key re-encoding (covers RSA-4096 + ECC) */
#define KEY_DER_TMP_LEN  4096

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
static size_t   ca_buf_len;
static uint8_t *client_key_buf;
static size_t   client_key_buf_len;
static bool     client_key_was_encrypted;
static uint8_t *client_cert_buf;
static size_t   client_cert_buf_len;

/* sec_tags — use the shared enum from wifi_certs.h */
#define WIFI_CERT_TAG_CA         WIFI_CERT_CA_SEC_TAG
#define WIFI_CERT_TAG_CLIENT_KEY WIFI_CERT_CLIENT_KEY_SEC_TAG
#define WIFI_CERT_TAG_CLIENT_CRT WIFI_CERT_CLIENT_SEC_TAG

/* -------------------------------------------------------------------------
 * Private helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief RNG wrapper for mbedTLS pk operations (key blinding etc.)
 */
static int cert_loader_rng(void *ctx, unsigned char *buf, size_t len)
{
    ARG_UNUSED(ctx);
    return sys_csrand_get(buf, len) == 0 ? 0 : -1;
}

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
    ca_buf_len = 0;

    delete_credential_if_exists(WIFI_CERT_TAG_CLIENT_KEY,
                                TLS_CREDENTIAL_PRIVATE_KEY);
    if (client_key_buf) {
        memset(client_key_buf, 0, client_key_buf_len);
    }
    k_free(client_key_buf);
    client_key_buf = NULL;
    client_key_buf_len = 0;
    client_key_was_encrypted = false;

    delete_credential_if_exists(WIFI_CERT_TAG_CLIENT_CRT,
                                TLS_CREDENTIAL_PUBLIC_CERTIFICATE);
    k_free(client_cert_buf);
    client_cert_buf = NULL;
    client_cert_buf_len = 0;
}

/**
 * @brief Mount LittleFS at /lfs (lazy, no-op if already mounted).
 */
static int ensure_lfs_mounted(void)
{
    int ret;

    if (cert_fs_mounted) {
        return 0;
    }

    ret = fs_mount(&cert_mount);
    if (ret < 0) {
        LOG_ERR("fs_mount(/lfs) failed: %d — check that certs are flashed "
                "to QSPI offset 0x3000", ret);
        return ret;
    }

    cert_fs_mounted = true;
    LOG_INF("LittleFS mounted at /lfs");
    return 0;
}

/**
 * @brief Read an entire file from the filesystem into a heap buffer.
 *
 * The returned buffer is NUL-terminated.  On success, ownership is
 * transferred to the caller's persistent slot (buf_keep).
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
 * @param len_keep Persistent length slot
 * @return 0 on success, negative errno on failure
 */
static int load_credential(const char *path, sec_tag_t tag,
                           enum tls_credential_type type,
                           uint8_t **buf_keep, size_t *len_keep)
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
    if (len_keep) {
        *len_keep = len;
    }

    LOG_INF("loaded credential: tag=0x%x type=%d from %s (%zu bytes)",
            (unsigned int)tag, (int)type, path, len);
    return 0;
}

/**
 * @brief Load a private key, decrypting it if a passphrase is provided.
 *
 * For unencrypted keys the PEM buffer is stored directly.  For encrypted
 * keys the buffer is decrypted via mbedTLS and re-encoded as unencrypted
 * DER before storage — Zephyr's TLS layer (sockets_tls.c) passes pwd=NULL
 * to mbedtls_pk_parse_key() and cannot handle encrypted keys itself.
 *
 * The passphrase is copied into a local stack buffer and zeroed after use.
 *
 * @param path     Filesystem path to the private key PEM file
 * @param key_pass Passphrase string, or NULL for unencrypted keys
 * @return 0 on success, negative errno on failure
 */
static int load_private_key(const char *path, const char *key_pass)
{
    uint8_t *file_buf = NULL;
    size_t file_len = 0;
    uint8_t *store_buf = NULL;
    size_t store_len = 0;
    size_t cred_len = 0;
    mbedtls_pk_context pk;
    char pass_copy[KEY_PASS_MAX_LEN];
    size_t pass_len = 0;
    bool encrypted = false;
    int ret;

    ret = read_file_to_buf(path, &file_buf, &file_len);
    if (ret < 0) {
        return ret;
    }

    /* Copy passphrase to local buffer so we can zero it after use */
    if (key_pass && key_pass[0] != '\0') {
        pass_len = strnlen(key_pass, KEY_PASS_MAX_LEN - 1);
        memcpy(pass_copy, key_pass, pass_len);
        pass_copy[pass_len] = '\0';
    } else {
        pass_len = 0;
        pass_copy[0] = '\0';
    }

    mbedtls_pk_init(&pk);

    /* First attempt: parse without passphrase */
    ret = mbedtls_pk_parse_key(&pk, file_buf, file_len + 1,
                                NULL, 0, cert_loader_rng, NULL);
    if (ret != 0) {
        if (pass_len == 0) {
            LOG_ERR("pk_parse_key(%s) failed: -0x%04x "
                    "(encrypted key? provide --key-pass)", path, -ret);
            mbedtls_pk_free(&pk);
            k_free(file_buf);
            return -EACCES;
        }

        /* Second attempt: parse with passphrase */
        ret = mbedtls_pk_parse_key(&pk, file_buf, file_len + 1,
                                    (const uint8_t *)pass_copy, pass_len,
                                    cert_loader_rng, NULL);
        memset(pass_copy, 0, sizeof(pass_copy)); /* zero passphrase copy */

        if (ret != 0) {
            LOG_ERR("pk_parse_key(%s) with passphrase failed: -0x%04x "
                    "(wrong passphrase?)", path, -ret);
            mbedtls_pk_free(&pk);
            k_free(file_buf);
            return -EACCES;
        }

        encrypted = true;
    } else {
        memset(pass_copy, 0, sizeof(pass_copy));
    }

    k_free(file_buf);
    file_buf = NULL;

    if (encrypted) {
        /*
         * Re-encode as unencrypted DER.
         * mbedtls_pk_write_key_der() writes to the END of the buffer and
         * returns the number of bytes written (positive) or an error (negative).
         */
        uint8_t *der_tmp = k_malloc(KEY_DER_TMP_LEN);

        if (!der_tmp) {
            mbedtls_pk_free(&pk);
            return -ENOMEM;
        }

        int der_len = mbedtls_pk_write_key_der(&pk, der_tmp, KEY_DER_TMP_LEN);

        mbedtls_pk_free(&pk);

        if (der_len <= 0) {
            LOG_ERR("pk_write_key_der(%s) failed: -0x%04x", path, -der_len);
            memset(der_tmp, 0, KEY_DER_TMP_LEN);
            k_free(der_tmp);
            return -EIO;
        }

        store_buf = k_malloc((size_t)der_len);
        if (!store_buf) {
            memset(der_tmp, 0, KEY_DER_TMP_LEN);
            k_free(der_tmp);
            return -ENOMEM;
        }

        /* DER data sits at the tail of der_tmp */
        memcpy(store_buf, der_tmp + KEY_DER_TMP_LEN - der_len,
               (size_t)der_len);
        memset(der_tmp, 0, KEY_DER_TMP_LEN);
        k_free(der_tmp);

        store_len = (size_t)der_len;
        cred_len  = store_len; /* DER: binary, no NUL */
    } else {
        mbedtls_pk_free(&pk);
        /*
         * Key was unencrypted — re-read from file rather than keeping
         * the pk context parsed copy (avoids a second DER encode for the
         * common case and reuses the same code path).
         */
        ret = read_file_to_buf(path, &store_buf, &store_len);
        if (ret < 0) {
            return ret;
        }
        cred_len = store_len + 1; /* PEM: include NUL terminator */
    }

    /* Register in the TLS credentials store */
    delete_credential_if_exists(WIFI_CERT_TAG_CLIENT_KEY,
                                TLS_CREDENTIAL_PRIVATE_KEY);
    if (client_key_buf) {
        memset(client_key_buf, 0, client_key_buf_len);
        k_free(client_key_buf);
        client_key_buf = NULL;
    }

    ret = tls_credential_add(WIFI_CERT_TAG_CLIENT_KEY,
                             TLS_CREDENTIAL_PRIVATE_KEY,
                             store_buf, cred_len);
    if (ret < 0) {
        LOG_ERR("tls_credential_add(key) failed: %d", ret);
        memset(store_buf, 0, store_len);
        k_free(store_buf);
        return ret;
    }

    client_key_buf = store_buf;
    client_key_buf_len = store_len;
    client_key_was_encrypted = encrypted;

    LOG_INF("loaded private key from %s (%zu bytes%s)",
            path, store_len, encrypted ? ", decrypted at load" : "");
    return 0;
}

/**
 * @brief Print subject CN and expiry for a PEM certificate buffer.
 */
static void print_cert_details(const struct shell *sh, const uint8_t *buf,
                               size_t len)
{
    mbedtls_x509_crt crt;
    char info[128];

    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, buf, len + 1) != 0) {
        shell_print(sh, "    (unable to parse certificate)");
        mbedtls_x509_crt_free(&crt);
        return;
    }

    if (mbedtls_x509_dn_gets(info, sizeof(info), &crt.subject) > 0) {
        shell_print(sh, "    Subject : %s", info);
    }
    shell_print(sh, "    Expires : %04d-%02d-%02d",
                crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);

    mbedtls_x509_crt_free(&crt);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Load EAP-TLS certificates from specified file paths.
 *
 * Each path parameter is optional; pass NULL to skip that credential.
 * LittleFS is mounted lazily on the first call.  Previously loaded
 * credentials for the same tag are replaced; others are left intact.
 *
 * For encrypted private keys, supply the passphrase in @p key_pass.
 * The key is decrypted and stored as unencrypted DER internally.
 *
 * @param ca_path   Path to CA certificate PEM file, or NULL
 * @param key_path  Path to client private key PEM file, or NULL
 * @param cert_path Path to client certificate PEM file, or NULL
 * @param key_pass  Private key passphrase, or NULL for unencrypted keys
 * @return 0 on success, negative errno on failure
 */
int wifi_load_certs_from_fs(const char *ca_path, const char *key_path,
                             const char *cert_path, const char *key_pass)
{
    int ret;

    if (!ca_path && !key_path && !cert_path) {
        return -EINVAL;
    }

    ret = ensure_lfs_mounted();
    if (ret < 0) {
        return ret;
    }

    if (ca_path) {
        ret = load_credential(ca_path, WIFI_CERT_TAG_CA,
                              TLS_CREDENTIAL_CA_CERTIFICATE,
                              &ca_buf, &ca_buf_len);
        if (ret < 0) {
            return ret;
        }
    }

    if (key_path) {
        ret = load_private_key(key_path, key_pass);
        if (ret < 0) {
            return ret;
        }
    }

    if (cert_path) {
        ret = load_credential(cert_path, WIFI_CERT_TAG_CLIENT_CRT,
                              TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
                              &client_cert_buf, &client_cert_buf_len);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief Unload all certificates from the TLS credentials store.
 */
void wifi_clear_certs(void)
{
    free_cert_bufs();
    LOG_INF("All certificates cleared");
}

/**
 * @brief Show loaded credential status and LittleFS file listing.
 *
 * @param sh Shell instance for output
 */
void wifi_list_certs(const struct shell *sh)
{
    struct fs_dir_t dir;
    struct fs_dirent entry;
    bool any_files = false;
    int ret;

    /* --- Loaded credentials --- */
    shell_print(sh, "Loaded credentials:");

    if (ca_buf) {
        shell_print(sh, "  CA cert     (tag=0x%07x): YES (%zu bytes)",
                    WIFI_CERT_TAG_CA, ca_buf_len);
        print_cert_details(sh, ca_buf, ca_buf_len);
    } else {
        shell_print(sh, "  CA cert     (tag=0x%07x): NO", WIFI_CERT_TAG_CA);
    }

    if (client_key_buf) {
        shell_print(sh, "  Client key  (tag=0x%07x): YES (%zu bytes)%s",
                    WIFI_CERT_TAG_CLIENT_KEY, client_key_buf_len,
                    client_key_was_encrypted ? "  [originally encrypted]" : "");
    } else {
        shell_print(sh, "  Client key  (tag=0x%07x): NO",
                    WIFI_CERT_TAG_CLIENT_KEY);
    }

    if (client_cert_buf) {
        shell_print(sh, "  Client cert (tag=0x%07x): YES (%zu bytes)",
                    WIFI_CERT_TAG_CLIENT_CRT, client_cert_buf_len);
        print_cert_details(sh, client_cert_buf, client_cert_buf_len);
    } else {
        shell_print(sh, "  Client cert (tag=0x%07x): NO",
                    WIFI_CERT_TAG_CLIENT_CRT);
    }

    shell_print(sh, "");

    /* --- LittleFS file listing --- */
    ret = ensure_lfs_mounted();
    if (ret < 0) {
        shell_print(sh, "Files in /lfs: (not mounted, err %d)", ret);
        return;
    }

    shell_print(sh, "Files in /lfs:");
    fs_dir_t_init(&dir);

    if (fs_opendir(&dir, "/lfs") < 0) {
        shell_print(sh, "  (cannot open directory)");
        return;
    }

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type == FS_DIR_ENTRY_FILE) {
            shell_print(sh, "  %-32s %6zu bytes", entry.name, entry.size);
            any_files = true;
        }
    }

    fs_closedir(&dir);

    if (!any_files) {
        shell_print(sh, "  (empty)");
    }
}

#endif /* CONFIG_WIFI_SHELL_RUNTIME_CERTIFICATES && CONFIG_FILE_SYSTEM_LITTLEFS */
