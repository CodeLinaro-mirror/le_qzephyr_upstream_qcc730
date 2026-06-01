# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
zephyr_library_sources_ifdef(CONFIG_WIFI_QCOM qcom_wifi_drv.c)
zephyr_library_sources_ifdef(CONFIG_WIFI_QCOM qcom_wifi_mgmt.c)
zephyr_library_sources_ifdef(CONFIG_WIFI_QCOM_HOSTAP_ELOOP qcom_hostap_eloop.c)
zephyr_library_link_libraries_ifdef(CONFIG_WIFI_QCOM_HOSTAP_ELOOP hostap)
zephyr_library_include_directories_ifdef(CONFIG_WIFI_QCOM_HOSTAP_ELOOP
    ${ZEPHYR_HOSTAP_MODULE_DIR}/src
    ${ZEPHYR_HOSTAP_MODULE_DIR}/src/utils
    ${ZEPHYR_HOSTAP_MODULE_DIR}/src/common
)
zephyr_include_directories(inc)

if(CONFIG_WIFI_QCOM_ENTERPRISE)
    zephyr_library_sources(qcom_wifi_enterprise_glue.c)
    # enterprise_glue.c needs supp_api.h from the hostap module
    zephyr_library_include_directories(
        ${ZEPHYR_HOSTAP_MODULE_DIR}/src
    )
    zephyr_library_link_libraries(hostap)
    # Intercept hs_compl_evt() for diagnostics and AES data-path fallback.
    # --wrap=hs_compl_evt redirects all callers (including within libwifiqcc730.a)
    # to __wrap_hs_compl_evt in qcom_wifi_enterprise_glue.c, which logs the call
    # and force-opens the host AES DPM entry on GTK phase completion.
    zephyr_ld_options(-Wl,--wrap=hs_compl_evt)
endif()

if(CONFIG_FILE_SYSTEM_LITTLEFS AND CONFIG_WIFI_SHELL_RUNTIME_CERTIFICATES)
    zephyr_library_sources(wifi_cert_loader.c)
    # LITTLEFS is an INTERFACE library that provides the lfs.h include path
    # (${ZEPHYR_LITTLEFS_MODULE_DIR}).  Link it so wifi_cert_loader.c can
    # include <zephyr/fs/littlefs.h> which transitively needs lfs.h.
    zephyr_library_link_libraries(LITTLEFS)
endif()
