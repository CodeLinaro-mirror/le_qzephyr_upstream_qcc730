/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qwifi_drv

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(qwifi_drv, CONFIG_WIFI_LOG_LEVEL);

#include <string.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/device.h>
#include <soc.h>
#include "qapi_lowpower.h"
#ifdef CONFIG_PM_DEVICE
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/pm.h>
#endif

#include "qapi_status.h"
#include <qwifi_api.h>
#include <libwifi.h>
#include <libwifi/wlan_defs.h>
#include "inc/qcom_wifi_mgmt.h"
#include "wlan_drv.h"
#include "wlan_qapi_helper.h"

#ifdef CONFIG_WIFI_NM
#include <zephyr/net/wifi_nm.h>
#endif
#include "wlan_qapi_helper.h"

#define SCAN_MODE_BLOCKING 1
#define SCAN_MODE_UNBLOCKING 2
#define QCOM_DEV_STA_ID 1
#define QCOM_DEV_AP_ID  0
#define QCOM_MAX_DEVICES 2
#define EDGE_BAND_10MHz 10
#define CONFIG_WIFI_SAP_PRIORITY 81

struct qwifi_bss_status_t {
    bool connected;
    uint8_t bssid[NET_ETH_ADDR_LEN];
    int ssid_length;
    char ssid[WIFI_SSID_MAX_LEN + 1];
};

struct qwifi_ap_status_t {
    int status;
    char ssid[WIFI_SSID_MAX_LEN + 1];
};

struct qwifi_drv_dev_data_t {
    uint8_t wlan_enabled;
    uint8_t active_device;
    qapi_WLAN_Crypt_Type_e e_cipher;
    scan_result_cb_t scan_cb;
    struct wifi_connect_req_params cfg_connect;
    struct qwifi_bss_status_t bss_status;
    struct qwifi_ap_status_t ap_status;
    struct net_if *iface;
    const struct device *dev;
    struct qcom_wifi_mgmt_ops qcom_wifi_cmd;
};

struct qwifi_drv_dev_cfg_t {
    int32_t scan_mode;
    int reserved;
};

static struct qwifi_drv_dev_data_t g_wifi_dev_data;
static struct qwifi_drv_dev_cfg_t g_wifi_dev_cfg = {
    .scan_mode = SCAN_MODE_UNBLOCKING,
};

static void wifi_activity_cb(PM_WLAN_ACTIVITY_STATUS activity);
void clear_wifi_busy(void);
#ifdef CONFIG_PM_DEVICE
extern qapi_Status_t qapi_WLAN_Activity_Register_CB(void (*callback)(PM_WLAN_ACTIVITY_STATUS));
#endif

static const uint32_t rate_index_to_kbps[] = {
    /* RateIndex 0-7, 802.11b rates */
    1000,   /* HAL_RT_IDX_11B_LONG_1_MBPS */
    2000,   /* HAL_RT_IDX_11B_LONG_2_MBPS */
    5500,   /* HAL_RT_IDX_11B_LONG_5_5_MBPS */
    11000,  /* HAL_RT_IDX_11B_LONG_11_MBPS */
    1000,   /* HAL_RT_IDX_11B_LONG_1_MBPS_DUP */
    2000,   /* HAL_RT_IDX_11B_SHORT_2_MBPS */
    5500,   /* HAL_RT_IDX_11B_SHORT_5_5_MBPS */
    11000,  /* HAL_RT_IDX_11B_SHORT_11_MBPS */
    
    /* RateIndex 8-15, 802.11a/g rates */
    6000,   /* HAL_RT_IDX_11A_6_MBPS */
    9000,   /* HAL_RT_IDX_11A_9_MBPS */
    12000,  /* HAL_RT_IDX_11A_12_MBPS */
    18000,  /* HAL_RT_IDX_11A_18_MBPS */
    24000,  /* HAL_RT_IDX_11A_24_MBPS */
    36000,  /* HAL_RT_IDX_11A_36_MBPS */
    48000,  /* HAL_RT_IDX_11A_48_MBPS */
    54000,  /* HAL_RT_IDX_11A_54_MBPS */
    
    /* RateIndex 16-23, 802.11n HT20 MCS0-7 (Long GI) */
    6500,   /* HAL_RT_IDX_MCS_1NSS_MM_6_5_MBPS */
    13000,  /* HAL_RT_IDX_MCS_1NSS_MM_13_MBPS */
    19500,  /* HAL_RT_IDX_MCS_1NSS_MM_19_5_MBPS */
    26000,  /* HAL_RT_IDX_MCS_1NSS_MM_26_MBPS */
    39000,  /* HAL_RT_IDX_MCS_1NSS_MM_39_MBPS */
    52000,  /* HAL_RT_IDX_MCS_1NSS_MM_52_MBPS */
    58500,  /* HAL_RT_IDX_MCS_1NSS_MM_58_5_MBPS */
    65000,  /* HAL_RT_IDX_MCS_1NSS_MM_65_MBPS */
    
    /* RateIndex 24-31, 802.11n HT20 MCS0-7 (Short GI) */
    7200,   /* HAL_RT_IDX_MCS_1NSS_MM_SG_7_2_MBPS */
    14400,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_14_4_MBPS */
    21700,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_21_7_MBPS */
    28900,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_28_9_MBPS */
    43300,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_43_3_MBPS */
    57800,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_57_8_MBPS */
    65000,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_65_MBPS */
    72200,  /* HAL_RT_IDX_MCS_1NSS_MM_SG_72_2_MBPS */
};
#define MAX_RATE_INDEX (sizeof(rate_index_to_kbps) / sizeof(rate_index_to_kbps[0]))

static void wifi_activity_cb(PM_WLAN_ACTIVITY_STATUS activity)
{
    const struct device *wifi_dev = device_get_binding("qwifi_sta");
    if (!wifi_dev) {
        LOG_ERR("qwifi_sta not found\r\n");
        return ;
    }
    if (!device_is_ready(wifi_dev)) {
        LOG_ERR("WiFi device not ready\r\n");
        return ;
    }

    //if wifi do not busy in check period, clear it
    if (activity == PM_WLAN_ACTIVITY_IDLE) {
        if(pm_device_is_busy(wifi_dev)) {
            pm_device_busy_clear(wifi_dev);
        }
    } else {
        pm_device_busy_set(wifi_dev);
    }
}

static struct qwifi_drv_dev_data_t g_wifi_dev_data_sap;
static struct qwifi_drv_dev_cfg_t g_wifi_dev_cfg_sap = {
    .scan_mode = SCAN_MODE_UNBLOCKING,
};

static const struct device *g_qwifi_dev_by_id[2] = {NULL, NULL};

const struct qcom_wifi_mgmt_ops *const get_qcom_wifi_api(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct qcom_wifi_mgmt_ops *off_api;

	if (dev == NULL) {
		return NULL;
	}
	struct qwifi_drv_dev_data_t *dev_data = dev->data;
	off_api = &dev_data->qcom_wifi_cmd;

	return off_api ? off_api : NULL;
}

static void qwifi_scan_complete_event(struct device *dev, qapi_WLAN_Scan_Comp_Evt_t *scan_result)
{
    struct wifi_scan_result res;
    int n_bss = scan_result->num_bss_cur;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;

    LOG_DBG("scan result report:");
    LOG_DBG("bss num: %d", n_bss);

    for (int k = 0; k < n_bss; k++) {
        memset(&res, 0, sizeof(struct wifi_scan_result));
        qapi_WLAN_BSS_Scan_Info_t *bss = &scan_result->scan_bss_info[k];

        res.rssi = bss->rssi;
        res.channel = bss->channel;
        res.ssid_length = bss->ssid_Length;
        strlcpy(res.ssid, bss->ssid, WIFI_SSID_MAX_LEN);
        memcpy(res.mac, bss->bssid, WIFI_MAC_ADDR_LEN);
        res.mac_length = WIFI_MAC_ADDR_LEN;
        res.band = bss->band;

        if (bss->security_Enabled) {
            if ((bss->rsn_Cipher & __QAPI_WLAN_CIPHER_TYPE_WEP) || (bss->wpa_Cipher & __QAPI_WLAN_CIPHER_TYPE_WEP)) {
                res.security = WIFI_SECURITY_TYPE_WEP;
            } else if (bss->rsn_Auth & __QAPI_WLAN_SECURITY_AUTH_PSK) {
                res.security = WIFI_SECURITY_TYPE_PSK;
            } else if (bss->rsn_Auth & __QAPI_WLAN_SECURITY_AUTH_SAE) {
                res.security = WIFI_SECURITY_TYPE_SAE;
            } else if (bss->wpa_Auth & __QAPI_WLAN_SECURITY_AUTH_PSK) {
                res.security = WIFI_SECURITY_TYPE_WPA_PSK;
            } else {
                res.security = WIFI_SECURITY_TYPE_UNKNOWN;
            }
        } else {
            res.security = WIFI_SECURITY_TYPE_NONE;
        }

        if (dev_data->scan_cb) {
            dev_data->scan_cb(dev_data->iface, 0, &res);
            k_yield();
        }
    }

    if (dev_data->scan_cb) {
        dev_data->scan_cb(dev_data->iface, 0, NULL);
        dev_data->scan_cb = NULL;
    }
}

static int station_connect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *info)
{
    int connect_status = WIFI_STATUS_CONN_SUCCESS;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct net_if *iface = dev_data->iface;
    struct qwifi_bss_status_t *bss = &dev_data->bss_status;
    uint8_t *mac = info->bssid;

    LOG_DBG("connect event report:");
    LOG_DBG("ssid: %s", dev_data->cfg_connect.ssid);
    LOG_DBG("mac addr: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    LOG_DBG("security: %d", dev_data->cfg_connect.security);
    LOG_DBG("connection status: %d", info->bss_Connection_Status);
    LOG_DBG("status: %d", info->evt_hdr.status);
    LOG_DBG("reason code: %d", info->reason_code);

    if (info->ssid_Length) {
        strlcpy(bss->ssid, info->ssid, WIFI_SSID_MAX_LEN);
        bss->ssid_length = info->ssid_Length;
    }

    memcpy(bss->bssid, info->bssid, NET_ETH_ADDR_LEN);

    if (info->evt_hdr.status == QAPI_OK) {
        connect_status = WIFI_STATUS_CONN_SUCCESS;
        bss->connected = true;
    } else {
        connect_status = WIFI_STATUS_CONN_FAIL;
        bss->connected = false;
    }

    wifi_mgmt_raise_connect_result_event(iface, connect_status);
    if (bss->connected) {
#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
        net_dhcpv4_start(iface);
#endif
    }

    return 0;
}

static int ap_station_connect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *info)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct net_if *iface = dev_data->iface;
    uint8_t *mac_addr = info->bssid;
    bool success = info->evt_hdr.status == QAPI_OK;

    /* filter ap itself connection event. */
    struct net_linkaddr *link_addr = net_if_get_link_addr(iface);
    if (!memcmp(link_addr->addr, mac_addr, link_addr->len)) {
        if (success) {
            dev_data->ap_status.status = WIFI_SAP_IFACE_ENABLED;
            wifi_mgmt_raise_ap_enable_result_event(iface, WIFI_STATUS_AP_SUCCESS);
        } else {
            dev_data->ap_status.status = WIFI_SAP_IFACE_DISABLED;
            wifi_mgmt_raise_ap_enable_result_event(iface, WIFI_STATUS_AP_FAIL);
        }
        return 0;
    }

    if (!success) {
        return -EPERM;
    }

    struct wifi_ap_sta_info sta_info = {0};
    memcpy(&sta_info.mac, mac_addr, sizeof(sta_info.mac));
    sta_info.mac_length = sizeof(sta_info.mac);

    wifi_mgmt_raise_ap_sta_connected_event(iface, &sta_info);

    return 0;
}

static void qwifi_connect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *info)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

    qapi_WLAN_DEV_Mode_e dev_mode = DEV_MODE_STATION_E;
    uint32_t size = sizeof(dev_mode);
    qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &dev_mode, &size);

    if (dev_mode == DEV_MODE_STATION_E) {
        station_connect_event(dev, info);
    } else if (dev_mode == DEV_MODE_AP_E) {
        ap_station_connect_event(dev, info);
    } else {
        LOG_ERR("Unknown dev mode %d", dev_mode);
    }
}

static void station_disconnect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *private)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct qwifi_bss_status_t *bss = &dev_data->bss_status;
    uint8_t dev_id = dev_data->active_device;

    qapi_WLAN_DEV_Mode_e dev_mode = DEV_MODE_STATION_E;
    uint32_t size = sizeof(dev_mode);
    qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &dev_mode, &size);

    LOG_DBG("disconnect event report:");
    LOG_DBG("ssid = %s", dev_data->bss_status.ssid);

    bss->connected = false;
    wifi_mgmt_raise_disconnect_result_event(dev_data->iface, WIFI_REASON_DISCONN_SUCCESS);

    if (dev_mode == DEV_MODE_STATION_E) {
#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
        net_dhcpv4_stop(dev_data->iface);
#endif
    }
}

static void ap_station_disconnect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *info)
{
    struct wifi_ap_sta_info sta_info = {0};
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct net_if *iface = dev_data->iface;
    uint8_t *sta_mac = info->bssid;

    /* filter ap itself connection event. */
    struct net_linkaddr * link_addr = net_if_get_link_addr(iface);
    if (!memcmp(link_addr->addr, sta_mac, link_addr->len)) {
        wifi_mgmt_raise_ap_disable_result_event(iface, WIFI_STATUS_AP_SUCCESS);
        dev_data->ap_status.status = WIFI_SAP_IFACE_DISABLED;
        return ;
    }

    memcpy(&sta_info.mac, sta_mac, sizeof(sta_info.mac));
    sta_info.mac_length = sizeof(sta_info.mac);

    wifi_mgmt_raise_ap_sta_disconnected_event(iface, &sta_info);
}

static void qwifi_disconnect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *info)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

    qapi_WLAN_DEV_Mode_e dev_mode = DEV_MODE_STATION_E;
    uint32_t size = sizeof(dev_mode);
    qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &dev_mode, &size);

    if (dev_id == QCOM_DEV_STA_ID) {
        station_disconnect_event(dev, info);
    } else if (dev_id == QCOM_DEV_AP_ID) {
        ap_station_disconnect_event(dev, info);
    } else {
        LOG_ERR("Unknown dev mode %d", dev_mode);
    }
}

static void qwifi_drv_event_handler(uint8_t dev_id, uint32_t event, void *context, void *private, uint32_t length)
{
    struct device *target_dev = NULL;

    if (dev_id < 2) {
        target_dev = (struct device *)g_qwifi_dev_by_id[dev_id];
    }
    if (target_dev == NULL) {
        target_dev = (struct device *)context;
    }

    switch (event) {
    case QAPI_WLAN_SCAN_COMPLETE_CB_E:
        qwifi_scan_complete_event(target_dev, private);
        break;
    case QAPI_WLAN_CONNECT_CB_E:
        qwifi_connect_event(target_dev, private);
        break;
    case QAPI_WLAN_DISCONNECT_CB_E:
        qwifi_disconnect_event(target_dev, private);
        break;
    case QAPI_WLAN_CHANNEL_SWITCH_CB_E:
        LOG_INF("CSA Done.");
        break;
    default:
        LOG_WRN("%s:%d event: %d, ignored.", __FUNCTION__, __LINE__, event);
        break;
    }

#ifdef CONFIG_PM_DEVICE
    qapi_WLAN_Start_Check_Activity();

#endif

}

static int qwifi_drv_disconnect(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
    qapi_WLAN_Stop_Check_Activity();
#endif

    qapi_WLAN_Disconnect(deviceId);

    return 0;
}

static int qwifi_drv_connect(const struct device *dev, struct wifi_connect_req_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;

    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_Auth_Mode_e e_wpa_ver = QAPI_WLAN_AUTH_NONE_E;
    qapi_WLAN_Crypt_Type_e e_cipher;
    const uint8_t *psk = NULL;
    uint8_t psk_length = 0;
    wlan_qapi_cxt_t *p_cxt = gp_wlan_qapi_cxt;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
    qapi_WLAN_Stop_Check_Activity();
#endif

    LOG_DBG("%s", __FUNCTION__);
    memcpy(&dev_data->cfg_connect, params, sizeof(struct wifi_connect_req_params));

    switch (params->security) {
    case WIFI_SECURITY_TYPE_PSK:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA2_PSK_E;
	e_cipher = QAPI_WLAN_CRYPT_AES_CRYPT_E;
        break;
    case WIFI_SECURITY_TYPE_WPA_PSK:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA_PSK_E;
        break;
    case WIFI_SECURITY_TYPE_SAE:
    case WIFI_SECURITY_TYPE_SAE_H2E:
    case WIFI_SECURITY_TYPE_SAE_AUTO:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA3_SAE_E;
	e_cipher = QAPI_WLAN_CRYPT_AES_CRYPT_E;
        break;
    case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA_WPA2_SAE_MIXED_E;
	e_cipher = QAPI_WLAN_CRYPT_AUTO;
        break;
    case WIFI_SECURITY_TYPE_NONE:
        e_wpa_ver = QAPI_WLAN_AUTH_NONE_E;
        break;
    default:
        LOG_ERR("Authentication method not supported");
        return -EIO;
    }

    if (p_cxt->conc_mode == DEV_MODE_NO_CONC_E) {
        qapi_WLAN_DEV_Mode_e mode = DEV_MODE_STATION_E;
        qapi_Status_t ret = qapi_WLAN_Set_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                                            &mode, sizeof(mode), false);
        if (ret) {
            LOG_ERR("set station mode fail");
            return -EINVAL;
        }
    }

    if (params->ssid_length) {
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
                            (void *)params->ssid, params->ssid_length, false);
        LOG_DBG("ssid=%s", params->ssid);
    }

    if(deviceId == QCOM_DEV_STA_ID) {
	if (params->bssid[0] || params->bssid[1] || params->bssid[2] ||
	    params->bssid[3] || params->bssid[4] || params->bssid[5]) {
		qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
				__QAPI_WLAN_PARAM_GROUP_WIRELESS_BSSID,
				(void *)params->bssid, __QAPI_WLAN_MAC_LEN, false);
	}
    }

    if (params->channel != WIFI_CHANNEL_ANY) {
        uint32_t channel[2] = {0, 0};
        channel[0] = params->channel;
        channel[1] = params->band == WIFI_FREQ_BAND_6_GHZ;
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_CHANNEL,
                            (void *)&channel, sizeof(channel), false);
    }

    if (e_wpa_ver) {
        psk = params->psk;
        psk_length = params->psk_length;
        if (((params->security == WIFI_SECURITY_TYPE_SAE)
		||(params->security == WIFI_SECURITY_TYPE_SAE_H2E)
		||(params->security == WIFI_SECURITY_TYPE_SAE_AUTO)
		||(params->security == WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL))
			&& (params->sae_password)) {
            psk = params->sae_password;
            psk_length = params->sae_password_length;
        }

        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
                            __QAPI_WLAN_PARAM_GROUP_SECURITY_AUTH_MODE, (void *)&e_wpa_ver,
                            sizeof(qapi_WLAN_Auth_Mode_e), false);
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
                            __QAPI_WLAN_PARAM_GROUP_SECURITY_ENCRYPTION_TYPE, (void *)&e_cipher,
                            sizeof(qapi_WLAN_Crypt_Type_e), false);
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
                            __QAPI_WLAN_PARAM_GROUP_SECURITY_PASSPHRASE, (void *)psk, psk_length, false);
    } else {
        wlan_clear_privacy();
    }

    qapi_WLAN_Commit(deviceId);

    return 0;
}

static int qwifi_drv_scan(const struct device *dev, struct wifi_scan_params *params, scan_result_cb_t cb)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;

    qapi_WLAN_Start_Scan_Params_t scan_param = {0};
    qapi_WLAN_DEV_Mode_e opmode;
    uint32_t length = sizeof(qapi_WLAN_DEV_Mode_e);
    qapi_Status_t ret = QAPI_OK;
    uint8_t deviceId = dev_data->active_device;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
    qapi_WLAN_Stop_Check_Activity();
#endif

    LOG_DBG("%s", __FUNCTION__);
    if (params->scan_type != WIFI_SCAN_TYPE_ACTIVE) {
	LOG_WRN("Currently only supports active scanning...");
        return -EINVAL;
    }

    if (params->bands > 0) {
	LOG_WRN("Currently not supports [-b, --bands] option, scanning\
			different bands separately is not supported. It\
			supports scanning all 2.4 G and 5G bands at once.");
	return -EINVAL;
    }

    if (params->dwell_time_active > 0) {
	LOG_WRN("Currently Not support [-a, --dwell_time_active <val_in_ms>] option");
        return -EINVAL;
    }

    if (params->dwell_time_passive > 0) {
	LOG_WRN("Currently Not support [-p, --dwell_time_passive <val_in_ms>] option");
        return -EINVAL;
    }

    if (params->max_bss_cnt > 0) {
	LOG_WRN("Currently not supports [-m, --max_bss <val>] option");
        return -EINVAL;
    }

    for (uint8_t i = 0; i < WIFI_MGMT_SCAN_CHAN_MAX_MANUAL; i++) {
	    if (params->band_chan[i].channel != 0) {
		LOG_WRN("Currently not supports [-c, --chans] option");
		return -EINVAL;
	    }
    }

    dev_data->scan_cb = cb;
    qapi_WLAN_Get_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &opmode, &length);
    if (opmode != DEV_MODE_STATION_E) {
        LOG_WRN("%s current operation mode %d do not support scan, need to set station mode", __FUNCTION__, opmode);
        return -EINVAL;
    }

#if (CONFIG_WIFI_MGMT_SCAN_SSID_FILT_MAX == 1)
    if (params->ssids[0]) {
        strlcpy(scan_param.ssid, params->ssids[0], WIFI_SSID_MAX_LEN);
        scan_param.ssid_Length = strnlen(params->ssids[0], WIFI_SSID_MAX_LEN);
        LOG_INF("scan ssid = %s", params->ssids[0]);
    }
#endif

    if (scan_param.ssid_Length) {
        ret = qapi_WLAN_Start_Scan(deviceId, &scan_param);
    } else {
        ret = qapi_WLAN_Start_Scan(deviceId, NULL);
    }

    if (ret != QAPI_OK) {
        return -EAGAIN;
    }
    return 0;
};

static int qwifi_drv_set_tx_power(const struct device *dev, struct qcom_wifi_set_tx_power_params *params)
{
    int ret = 0;
    qapi_WLAN_Set_Txpower_Params_t set_tx_power_cfg;
    set_tx_power_cfg.txpower = params->txpower;
    set_tx_power_cfg.policy = params->policy;

    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

    ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_TX_POWER_IN_DBM,
                        &set_tx_power_cfg,
                        sizeof(set_tx_power_cfg),
                        false);

    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set tx power for device %d", deviceId);
        return -EIO;
    }

    return 0;
}

static int qwifi_drv_get_tx_power(const struct device *dev, struct qcom_wifi_get_tx_power_params *params)
{
    qapi_WLAN_Get_Power_Evt_t power;
    uint32_t length = sizeof(power);

    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

    if(0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_TX_POWER_IN_DBM,
                        &power,
                        &length)) {
        LOG_ERR("get tx power fail for device %d",deviceId);
        return -EIO;
    }

    LOG_INF("get real_power: %d dbm", power.real_power);
    LOG_INF("get ctl_power: %d dbm", power.ctl_power);
    LOG_INF("get reg_power: %d dbm", power.reg_power);
    LOG_INF("get target_power: %d dbm", power.target_power);

    params->reg_power = power.reg_power;
    params->ctl_power = power.ctl_power;
    params->target_power = power.target_power;
    params->real_power = power.real_power;

    return 0;
}

static int ap_enable(const struct device *dev, struct wifi_connect_req_params *params)
{
    qapi_Status_t ret;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct qwifi_ap_status_t *ap_status = &dev_data->ap_status;
    const uint8_t dev_id = dev_data->active_device;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    qapi_WLAN_DEV_Mode_e mode = DEV_MODE_AP_E;
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
            &mode, sizeof(mode), false);
    if (ret) {
        LOG_ERR("set soft ap mode fail");
        return -EINVAL;
    }

    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
            params->ssid, params->ssid_length, false);
    if (ret) {
        if (params->ssid) {
            LOG_WRN("ssid = %s", params->ssid);
        } else {
            LOG_WRN("ssid =");
        }
        LOG_WRN("ssid length = %d", params->ssid_length);
        return -EINVAL;
    }

    uint32_t channel[2] = {0, 0};
    channel[0] = params->channel;
    channel[1] = params->band == WIFI_FREQ_BAND_6_GHZ;
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_CHANNEL,
            &channel, sizeof(channel), false);
    if (ret) {
        LOG_WRN("channel = %d, %d", channel[0], channel[1]);
        return -EINVAL;
    }

    qapi_WLAN_Auth_Mode_e auth_mode = QAPI_WLAN_AUTH_NONE_E;
    qapi_WLAN_Crypt_Type_e cipher = QAPI_WLAN_CRYPT_NONE_E;
    if (params->security == WIFI_SECURITY_TYPE_NONE) {
        auth_mode = QAPI_WLAN_AUTH_NONE_E;
        cipher = QAPI_WLAN_CRYPT_NONE_E;
    } else if (params->security == WIFI_SECURITY_TYPE_PSK) {
        auth_mode = QAPI_WLAN_AUTH_WPA2_PSK_E;
        cipher = QAPI_WLAN_CRYPT_AES_CRYPT_E;
    } else {
        LOG_WRN("Not Support = %d", params->security);
        return -EINVAL;
    }
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY, __QAPI_WLAN_PARAM_GROUP_SECURITY_AUTH_MODE,
            (void *) &auth_mode, sizeof(auth_mode), false);
    if (ret) {
        LOG_WRN("auth mode = %d, %d ret = %d", channel[0], channel[1], ret);
        return -EINVAL;
    }

    qapi_WLAN_Set_Param(dev_id,
                __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY, __QAPI_WLAN_PARAM_GROUP_SECURITY_ENCRYPTION_TYPE,
                &cipher, sizeof(cipher), false);

    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY, __QAPI_WLAN_PARAM_GROUP_SECURITY_PASSPHRASE,
            params->psk, params->psk_length, false);
    if (ret) {
        if (params->psk) {
            LOG_WRN("psk = %s", params->psk);
        } else {
            LOG_WRN("psk =");
        }
        LOG_WRN("psk length = %d", params->psk_length);
        return -EINVAL;
    }

    uint8_t hidden_flag = !!params->ignore_broadcast_ssid;
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_AP_ENABLE_HIDDEN_MODE,
            &hidden_flag, sizeof(hidden_flag), false);
    if (ret) {
        LOG_WRN("hidden ssid = %d, ret = %d", params->ignore_broadcast_ssid, ret);
        return -EINVAL;
    }

    qapi_WLAN_11n_HT_Config_e htconfig = QAPI_WLAN_11N_HT20_E;
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_11N_HT,
            &htconfig, sizeof(htconfig), false);

    qapi_WLAN_Phy_Mode_e phymode = QAPI_WLAN_11ABGN_HT20_MODE_E;
    ret = qapi_WLAN_Set_Param(dev_id,
            __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_PHY_MODE,
            &phymode, sizeof(phymode), false);

    dev_data->ap_status.status = WIFI_SAP_IFACE_DISABLED;
    ret = qapi_WLAN_Commit(dev_id);
    if (ret) {
        LOG_WRN("ret = %d, commit fail.", ret);
        return -EAGAIN;
    }

    /* save ssid for `wifi ap status` command. */
    strlcpy(ap_status->ssid, params->ssid, sizeof(ap_status->ssid));

    return 0;
}

static int ap_disable(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    qapi_WLAN_Disconnect(dev_id);

    /* swtch to station mode. */
    qapi_WLAN_DEV_Mode_e mode = DEV_MODE_STATION_E;
    qapi_WLAN_Set_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &mode, sizeof(mode), false);

    return 0;
}

static int ap_sta_disconnect(const struct device *dev, const uint8_t *mac)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    qapi_WLAN_AP_Disconnect_Station(dev_id, mac, NET_ETH_ADDR_LEN);

    return 0;
}
static int qwifi_drv_unit_test(const struct device *dev, struct qcom_wifi_unit_test_params *params)
{
    qapi_Status_t ret = QAPI_WLAN_ERROR;
	struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    ret = qapi_WLAN_Unit_Test(dev_id, params, sizeof(struct qcom_wifi_unit_test_params));
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to send unit test command via QAPI");
        return -EIO;
    }
    return 0;
}

/**
 * @brief Enable or disable RTS/CTS protection on the active WLAN device.
 *
 * Controls RTS/CTS protection via qapi_WLAN_Set_Param for the currently active
 * WLAN interface. When enabled, RTS/CTS handshaking can reduce collisions in
 * congested or hidden-node scenarios by requiring a request-to-send and
 * clear-to-send exchange before data transmission.
 *
 * @param rts_cts RTS/CTS control flag:
 *        - 1: enable RTS/CTS protection
 *        - 0: disable RTS/CTS protection
 *
 * @return 0 on success; -1 on failure.
 */
static int qwifi_drv_set_rts_cts(const struct device *dev, struct qcom_wifi_set_rts_cts_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t enable = params->enable;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS,
                        &enable,
                        sizeof(enable),
                        FALSE);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set RTS/CTS (enable=%u) for device %d: %d", enable, deviceId, ret);
        return -EIO;
    }

    return 0;
}

/**
 * @brief Set the RTS control frame transmit rate on the active WLAN device (2.4 GHz).
 *
 * Configures the transmit rate used for RTS/CTS control frames via qapi_WLAN_Set_Param
 * on the currently active interface. Adjusting the RTS rate can influence airtime and
 * robustness of the RTS/CTS protection mechanism, particularly on 2.4 GHz links.
 *
 * Preconditions:
 * - Operates on the active device.
 * - Intended for 2.4 GHz operation (uses __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G).
 *   The accompanying comment suggests using this after a 2G connection.
 *
 * @param rate RTS rate selector (indexed mapping):
 *        - 0: 1 Mbps (802.11b long)
 *        - 1: 6 Mbps (OFDM)
 *        - 2: 12 Mbps (OFDM)
 *   Note: Valid values and their mapping are defined by the underlying firmware/QAPI
 *   and may be limited to the options shown above.
 *
 * @return 0 on success; -1 on failure.
 */
static int qwifi_drv_set_rts_rate(const struct device *dev, struct qcom_wifi_set_rts_rate_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t rts_rate = params->rts_rate;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G,
                        &rts_rate,
                        sizeof(rts_rate),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set RTS rate (rate=%u) for device %d: %d", rts_rate, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Configure EDCA (WMM) parameters on the active WLAN device.
 *
 * Programs the per-queue Enhanced Distributed Channel Access (EDCA) parameters
 * (AIFSN, CWmin, CWmax, and TXOP limit) via qapi_WLAN_Set_Param for the queue
 * identified by qid, or for all queues when qid == 0xFF.
 *
 * @param qid        Access category/queue identifier:
 *                   - 0..7: apply to the specified hardware/software queue
 *                   - 0xFF: apply to all queues
 * @param aifsn      Arbitration Inter-Frame Space Number. Lower values give higher priority.
 * @param cw_min     Minimum contention window exponent e_min. Effective CWmin = 2^e_min - 1.
 * @param cw_max     Maximum contention window exponent e_max. Effective CWmax = 2^e_max - 1.
 * @param txop_limit Transmit opportunity limit for the queue; duration as defined by the
 *                   firmware/QAPI (commonly in 32 µs units). A value of 0 typically disables
 *                   bursting for the queue.
 *
 * @return 0 on success; -1 on failure.
 *
 * Notes:
 * - Operates on the active device.
 * - Ensure values adhere to firmware/regulatory bounds; invalid values will be rejected
 *   by qapi_WLAN_Set_Param.
 */
static int qwifi_drv_set_edca_param_cfg(const struct device *dev, struct qcom_wifi_set_edca_param_cfg_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_Edca_Params_t edca_param_cfg;

    edca_param_cfg.qid = params->qid;
    edca_param_cfg.aifsn = params->aifsn;
    edca_param_cfg.cw_min = params->cw_min;
    edca_param_cfg.cw_max = params->cw_max;
    edca_param_cfg.txop_limit = params->txop_limit;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_EDCA_PARAM,
                        &edca_param_cfg,
                        sizeof(edca_param_cfg),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set EDCA (qid=%u, aifsn=%u, cw_min=%u, cw_max=%u, txop=%u) for device %d: %d",
                params->qid, params->aifsn, params->cw_min, params->cw_max, params->txop_limit, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Set the upper Packet Error Rate (PER) threshold on the active WLAN device.
 *
 * Configures the PER upper threshold via qapi_WLAN_Set_Param for the currently
 * active interface. This threshold can be used by firmware to trigger internal
 * algorithms (e.g., rate control or diagnostics) when the measured PER exceeds
 * the configured limit.
 *
 * @param value Upper PER threshold value. The valid range is enforced by the
 *              firmware; values must be less than 100 (implementation-specific
 *              units; commonly interpreted as percentage 0..99).
 *
 * @return 0 on success; -1 on failure.
 *
 * Notes:
 * - Operates on the active device.
 * - The function fails if the driver/firmware rejects the provided threshold.
 */
static int qwifi_drv_set_threshold(const struct device *dev, struct qcom_wifi_set_threshold_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t threshold = params->threshold;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_PER_UPPER_THRESHOLD,
                        &threshold,
                        sizeof(threshold),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set PER upper threshold (value=%u) for device %d: %d", threshold, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Configure Block Ack (BA) window timing parameters on the active WLAN device.
 *
 * Programs the BA window parameters (ACK timeout and delay) via qapi_WLAN_Set_Param
 * for the currently active interface. These timing values influence the Block Ack
 * exchange behavior and can affect both reliability and throughput.
 *
 * @param ack_time   ACK timeout in microseconds. Must be less than 4096 µs.
 * @param delay_time Delay value in SM clock cycles (firmware-specific units).
 *                   Typically constrained to less than 64 SM cycles.
 *
 * @return 0 on success; -1 on failure.
 *
 * Notes:
 * - Operates on the active device.
 * - The firmware enforces valid ranges; invalid values are rejected by qapi_WLAN_Set_Param.
 */
static int qwifi_drv_set_ba_win_timing(const struct device *dev, struct qcom_wifi_set_ba_win_timing_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_BA_Window_Params_t ba_win_timing_cfg;

    ba_win_timing_cfg.ack_timeout = params->ack_timeout;
    ba_win_timing_cfg.delay = params->delay;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_BA_WINDOW,
                        &ba_win_timing_cfg,
                        sizeof(ba_win_timing_cfg),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set BA window (ack_timeout=%u, delay=%u) for device %d: %d",
                params->ack_timeout, params->delay, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Set the PHY slot time on the active WLAN device.
 *
 * Configures the slot time used by the MAC backoff algorithm via qapi_WLAN_Set_Param
 * for the currently active interface. Typical values are 9 µs (short slot) and 20 µs
 * (long slot), depending on PHY mode and regulatory/compatibility constraints.
 *
 * @param time Slot time in microseconds (commonly 9 or 20).
 *
 * @return 0 on success; -1 on failure.
 *
 * Notes:
 * - Operates on the active device.
 * - The firmware enforces valid slot times; invalid values are rejected by qapi_WLAN_Set_Param.
 */
static int qwifi_drv_set_slot_time(const struct device *dev, struct qcom_wifi_set_slot_time_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t slot_time = params->slot_time;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_SLOT_TIME,
                        &slot_time,
                        sizeof(slot_time),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set slot time (time=%u) for device %d: %d", slot_time, deviceId, ret);
        return -EIO;
    }

    return 0;
}

/**
 * @brief Configure TX/RX aggregation TID bitmasks on the active WLAN device.
 *
 * Programs aggregation enable masks via qapi_WLAN_Set_Param using
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_ALLOW_TX_RX_AGGR_SET_TID.
 *
 * Internals:
 * - Builds qapi_WLAN_Aggregation_Params_t from the input params:
 *   - agg.tx_TID_Mask = params->tx_tid_mask
 *   - agg.rx_TID_Mask = params->rx_tid_mask
 * - Operates on dev->data->active_device (the currently active device ID).
 * - Calls qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
 *   __QAPI_WLAN_PARAM_GROUP_WIRELESS_ALLOW_TX_RX_AGGR_SET_TID, &agg,
 *   sizeof(agg), FALSE).
 *
 * @param dev Pointer to the driver device instance. Used to access the
 *            active deviceId (dev->data->active_device).
 * @param params Input structure providing two 8-bit bitmasks:
 *            - params->tx_tid_mask: bit i (0..7) enables TX aggregation for TID i.
 *            - params->rx_tid_mask: bit i (0..7) enables RX aggregation for TID i.
 *
 * @return 0 on success; negative error code on failure:
 *         - -EIO if the underlying qapi_WLAN_Set_Param call fails.
 *
 * Notes:
 * - Each mask is limited to 0..0xFF; higher values are rejected earlier by the shell command.
 * - On failure, an error is logged with the device ID and the provided masks.
 */
static int qwifi_drv_set_aggregation(const struct device *dev, struct qcom_wifi_set_aggregation_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_Aggregation_Params_t agg = {0};

    agg.tx_TID_Mask = params->tx_tid_mask;
    agg.rx_TID_Mask = params->rx_tid_mask;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_ALLOW_TX_RX_AGGR_SET_TID,
                        &agg,
                        sizeof(agg),
                        FALSE);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set aggregation TIDs (tx=0x%02x, rx=0x%02x) for device %d: %d",
                agg.tx_TID_Mask, agg.rx_TID_Mask, deviceId, ret);
        return -EIO;
    }
    return 0;
}

static int qwifi_drv_set_amsdu_rx(const struct device *dev, struct qcom_wifi_set_amsdu_rx_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint8_t enable = params->enable;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_AMSDU_RX,
                        &enable,
                        sizeof(enable),
                        FALSE);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set AMSDU RX (enable=%u) for device %d: %d", enable, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Set PHY mode on the active WLAN device.
 *
 * Configures PHY mode via qapi_WLAN_Set_Param for the currently active interface.
 * The value corresponds to qapi_WLAN_Phy_Mode_e as defined by the firmware/QAPI.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Input structure with params->phy_mode (qapi_WLAN_Phy_Mode_e).
 *
 * @return 0 on success; negative error code on failure.
 */
static int qwifi_drv_set_phy_mode(const struct device *dev, struct qcom_wifi_set_phy_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t phy_mode = params->phy_mode;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_PHY_MODE,
                        &phy_mode,
                        sizeof(phy_mode),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set PHY mode (value=%u) for device %d: %d", phy_mode, deviceId, ret);
        return -EIO;
    }
    return 0;
}

/**
 * @brief Get PHY mode on the active WLAN device.
 *
 * Retrieves PHY mode via qapi_WLAN_Get_Param for the currently active interface.
 * The returned value corresponds to qapi_WLAN_Phy_Mode_e.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Output structure with params->phy_mode filled on success.
 *
 * @return 0 on success; negative error code on failure.
 */
static int qwifi_drv_get_phy_mode(const struct device *dev, struct qcom_wifi_get_phy_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t phy_mode = 0;
    uint32_t length = sizeof(phy_mode);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_PHY_MODE,
                        &phy_mode,
                        &length)) {
        LOG_ERR("get PHY mode fail for device %d", deviceId);
        return -EIO;
    }

    params->phy_mode = phy_mode;
    return 0;
}

/**
 * @brief Get Wi-Fi power mode of the active device.
 *
 * Retrieves the current power performance/save mode via qapi_WLAN_Get_Param
 * using __QAPI_WLAN_PARAM_GROUP_WIRELESS_POWER_MODE_PARAMS on the active device.
 *
 * On success, params->power_mode contains:
 *  - 0: Max Perf (no power saving)
 *  - bit0 (1): BMPS enabled (Beacon Mode Power Save)
 *  - bit1 (2): IMPS enabled (Idle Mode Power Save)
 *  - bit2 (4): WUR enabled (Wake-Up Radio)
 *  - bit3 (8): WNM enabled (Wireless Network Management power features)
 *
 * @param dev    Pointer to the driver device instance.
 * @param params Output structure of type qcom_wifi_get_power_mode_params;
 *               on success, params->power_mode is filled with the bitfield above.
 *
 * @return 0 if ok, negative error code if error.
 */
static int qwifi_drv_get_power_mode(const struct device *dev, struct qcom_wifi_get_power_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint8_t power_mode = 0;
    uint32_t length = sizeof(power_mode);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_POWER_MODE_PARAMS,
                        &power_mode,
                        &length)) {
        LOG_ERR("get power mode fail for device %d", deviceId);
        return -EIO;
    }

    params->power_mode = power_mode;
    return 0;
}

/**
 * @brief Get system boot reason bitfield from platform core/PMU.
 *
 * Retrieves the raw boot-reason bitfield via qapi_Core_Obtain_Boot_Reason().
 * The interpretation of the returned flags (e.g., cold/warm boot, DTIM sleep,
 * deep sleep) is platform-specific and typically performed at a higher layer
 * (e.g., shell/UI) using platform-defined masks.
 *
 * @param dev    Pointer to the driver device instance. Used for logging and
 *               access to the active device context when needed.
 * @param params Output structure of type qcom_wifi_get_boot_reason_params;
 *               on success, params->boot_reason is set to the raw 32-bit
 *               bitfield returned by the platform.
 *
 * @return 0 on success; negative error code on failure:
 *         - -EIO if qapi_Core_Obtain_Boot_Reason() fails.
 *
 * Notes:
 * - Known masks for decoding may include:
 *   - PMU_BASE_pmu_PMU_SYSTEM_STATUS_COLD_WARM_BOOT_Msk
 *   - QWLAN_PMU_SYSTEM_STATUS_WARM_BOOT_FROM_SLEEP_MASK
 *   - QWLAN_PMU_SYSTEM_STATUS_WARM_BOOT_FROM_DEEPSLEEP_MASK
 *   These masks are defined in platform headers; this function only surfaces
 *   the raw value without performing interpretation.
 */
static int qwifi_drv_get_boot_reason(const struct device *dev, struct qcom_wifi_get_boot_reason_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_boot_reason_t data = 0;

    if (QAPI_OK != qapi_core_obtain_boot_reason(&data)) {
        LOG_ERR("get boot reason fail for device %d", deviceId);
        return -EIO;
    }

    params->boot_reason = data;
    return 0;
}

/**
 * @brief Get MAC address of the active WLAN device.
 *
 * Retrieves the device MAC address via qapi_WLAN_Get_Param using
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_MAC_ADDRESS on the active device.
 *
 * On success, params->mac is filled with ETH_ALEN (6) bytes of the MAC address.
 *
 * @param dev    Pointer to the driver device instance.
 * @param params Output structure of type qcom_wifi_get_mac_address_params;
 *               on success, params->mac[] contains the device MAC.
 *
 * @return 0 if ok, negative error code if error.
 */
static int qwifi_drv_get_mac_address(const struct device *dev, struct qcom_wifi_get_mac_address_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint8_t mac[__QAPI_WLAN_MAC_LEN] = {0};
    uint32_t length = __QAPI_WLAN_MAC_LEN;

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_MAC_ADDRESS,
                        mac,
                        &length)) {
        LOG_ERR("get MAC address fail for device %d", deviceId);
        return -EIO;
    }

    memcpy(params->mac, mac, __QAPI_WLAN_MAC_LEN);
    return 0;
}

/**
 * @brief Get WLAN concurrency mode of the active device.
 *
 * Retrieves concurrency mode via qapi_WLAN_Get_Param using
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_CONCURRENCY_MODE.
 *
 * On success, params->conc_mode holds the concurrency mode enum
 * (qapi_WLAN_DEV_Mode_e), e.g. DEV_MODE_AP_STA_E for AP+STA concurrency,
 * or a single-mode value when concurrency is disabled.
 *
 * @param dev    Pointer to the driver device instance.
 * @param params Output structure of type qcom_wifi_get_concurrency_mode_params;
 *               on success, params->conc_mode is set to the current mode.
 *
 * @return 0 if ok, negative error code if error.
 */
static int qwifi_drv_get_concurrency_mode(const struct device *dev, struct qcom_wifi_get_concurrency_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_DEV_Mode_e conc_mode = DEV_MODE_STATION_E;
    uint32_t length = sizeof(conc_mode);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_CONCURRENCY_MODE,
                        &conc_mode,
                        &length)) {
        LOG_ERR("get concurrency mode fail for device %d", deviceId);
        return -EIO;
    }

    params->conc_mode = conc_mode;
    return 0;
}

/**
 * @brief Get WLAN operation mode of the active device.
 *
 * Retrieves operation mode via qapi_WLAN_Get_Param using
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE.
 *
 * On success, params->opmode holds the operation mode enum
 * (qapi_WLAN_DEV_Mode_e), typically DEV_MODE_STATION_E for STA
 * or DEV_MODE_AP_E for SoftAP.
 *
 * @param dev    Pointer to the driver device instance.
 * @param params Output structure of type qcom_wifi_get_operation_mode_params;
 *               on success, params->opmode is set to the current operation mode.
 *
 * @return 0 if ok, negative error code if error.
 */
static int qwifi_drv_get_operation_mode(const struct device *dev, struct qcom_wifi_get_operation_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_DEV_Mode_e opmode = DEV_MODE_STATION_E;
    uint32_t length = sizeof(opmode);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &opmode,
                        &length)) {
        LOG_ERR("get operation mode fail for device %d", deviceId);
        return -EIO;
    }

    params->opmode = opmode;
    return 0;
}

/**
 * @brief Set STA beacon-miss (BMISS) threshold.
 *
 * Configures the number of consecutive missed beacons that the STA tolerates
 * before firmware triggers BMISS handling (e.g., roaming or disconnect) via
 * qapi_WLAN_Set_Param using __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG.
 *
 * @param dev Pointer to the driver device instance (provides active deviceId).
 * @param params Input structure:
 *        - params->threshold: BMISS threshold (firmware-defined range; units
 *          are number of missed beacon intervals).
 *
 * @return 0 on success; negative error code on failure.
 *
 * Notes:
 * - Operates on the currently active device (dev->data->active_device).
 * - The firmware validates acceptable threshold values; invalid inputs cause
 *   qapi_WLAN_Set_Param to return an error.
 */
static int qwifi_drv_set_bmiss_threshold(const struct device *dev, struct qcom_wifi_set_bmiss_threshold_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t bmiss = params->threshold;

    qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG,
                        &bmiss,
                        sizeof(bmiss),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set BMISS threshold (value=%u) for device %d: %d", bmiss, deviceId, ret);
        return -EIO;
    }
    return 0;
}

static int qwifi_drv_set_sap_csa(const struct device *dev, struct qcom_wifi_csa_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

    /* not support 6G, default is false */
    qapi_Status_t ret = qapi_WLAN_Sap_Csa(dev_id, params->switch_mode, params->new_channel, false, params->switch_count);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to CSA, ret %d, switch mode %d, switch channel number %d, channel switch count %d",
                ret, params->switch_mode, params->new_channel, params->switch_count);
        return -EINVAL;
    }

    return ret;
}

/**
 * @brief Get RTS/CTS protection enable status.
 *
 * Retrieves the current RTS/CTS protection flag from the active WLAN device
 * via qapi_WLAN_Get_Param using __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS.
 *
 * @param dev Pointer to the device structure for the driver instance.
 * @param params Output structure; on success, params->enable is set to:
 *        - 1: RTS/CTS enabled
 *        - 0: RTS/CTS disabled
 *
 * @return 0 on success, negative error code on failure.
 */
static int qwifi_drv_get_rts_cts(const struct device *dev, struct qcom_wifi_get_rts_cts_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t enable = 0;
    uint32_t length = sizeof(enable);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS,
                        &enable,
                        &length)) {
        LOG_ERR("get RTS/CTS fail for device %d", deviceId);
        return -EIO;
    }

    params->enable = enable;
    return 0;
}

/**
 * @brief Get the RTS control frame transmit rate (2.4 GHz).
 *
 * Reads the RTS rate selector used on 2.4 GHz via qapi_WLAN_Get_Param
 * with __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Output structure; on success, params->rts_rate contains the
 *        firmware-defined rate index (e.g. 0: 1 Mbps, 1: 6 Mbps, 2: 12 Mbps).
 *
 * @return 0 on success, negative error code on failure.
 */
static int qwifi_drv_get_rts_rate(const struct device *dev, struct qcom_wifi_get_rts_rate_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t rts_rate = 0;
    uint32_t length = sizeof(rts_rate);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_RTS_RATE_2G,
                        &rts_rate,
                        &length)) {
        LOG_ERR("get RTS rate fail for device %d", deviceId);
        return -EIO;
    }

    params->rts_rate = rts_rate;
    return 0;
}

/**
 * @brief Get EDCA (WMM) parameters for a queue or all queues.
 *
 * Retrieves the EDCA parameters (AIFSN, CWmin, CWmax, TXOP limit) via
 * qapi_WLAN_Get_Param with __QAPI_WLAN_PARAM_GROUP_WIRELESS_EDCA_PARAM.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Input/Output structure:
 *        - Input: params->qid selects the queue (0..7) or 0xFF for all queues,
 *                 when supported by firmware.
 *        - Output: params->qid, params->aifsn, params->cw_min, params->cw_max,
 *                  params->txop_limit are filled with current configuration.
 *
 * @return 0 on success, negative error code on failure.
 *
 * Notes:
 * - Queue selector semantics depend on firmware support.
 */
static int qwifi_drv_get_edca_param_cfg(const struct device *dev, struct qcom_wifi_get_edca_param_cfg_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_Edca_Params_t edca_param_cfg = {0};
    uint32_t length = sizeof(edca_param_cfg);

    /* Use caller-provided qid as selector when supported by firmware */
    edca_param_cfg.qid = params->qid;

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_EDCA_PARAM,
                        &edca_param_cfg,
                        &length)) {
        LOG_ERR("get EDCA param fail for device %d", deviceId);
        return -EIO;
    }

    params->qid        = edca_param_cfg.qid;
    params->aifsn      = edca_param_cfg.aifsn;
    params->cw_min     = edca_param_cfg.cw_min;
    params->cw_max     = edca_param_cfg.cw_max;
    params->txop_limit = edca_param_cfg.txop_limit;
    return 0;
}

/**
 * @brief Get the upper Packet Error Rate (PER) threshold.
 *
 * Retrieves the configured PER upper threshold via
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_PER_UPPER_THRESHOLD.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Output structure; on success, params->threshold holds the
 *        current PER threshold (firmware-defined units; commonly 0..99).
 *
 * @return 0 on success, negative error code on failure.
 */
static int qwifi_drv_get_threshold(const struct device *dev, struct qcom_wifi_get_threshold_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t threshold = 0;
    uint32_t length = sizeof(threshold);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_PER_UPPER_THRESHOLD,
                        &threshold,
                        &length)) {
        LOG_ERR("get PER upper threshold fail for device %d", deviceId);
        return -EIO;
    }

    params->threshold = threshold;
    return 0;
}

/**
 * @brief Get Block Ack (BA) window timing parameters.
 *
 * Reads the BA window configuration (ACK timeout and delay) via
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_BA_WINDOW.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Output structure; on success:
 *        - params->ack_timeout: ACK timeout in microseconds
 *        - params->delay: delay value in SM clock cycles
 *
 * @return 0 on success, negative error code on failure.
 */
static int qwifi_drv_get_ba_win_timing(const struct device *dev, struct qcom_wifi_get_ba_win_timing_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    qapi_WLAN_BA_Window_Params_t ba_win_timing_cfg = {0};
    uint32_t length = sizeof(ba_win_timing_cfg);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_BA_WINDOW,
                        &ba_win_timing_cfg,
                        &length)) {
        LOG_ERR("get BA window size fail for device %d", deviceId);
        return -EIO;
    }

    params->ack_timeout = ba_win_timing_cfg.ack_timeout;
    params->delay       = ba_win_timing_cfg.delay;
    return 0;
}

/**
 * @brief Get the PHY slot time in microseconds.
 *
 * Retrieves the MAC slot time used for backoff via
 * __QAPI_WLAN_PARAM_GROUP_WIRELESS_SLOT_TIME.
 *
 * @param dev Pointer to the driver device instance.
 * @param params Output structure; on success, params->slot_time is set
 *        to the current slot time (e.g., 9 or 20 µs).
 *
 * @return 0 on success, negative error code on failure.
 */
static int qwifi_drv_get_slot_time(const struct device *dev, struct qcom_wifi_get_slot_time_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t slot_time = 0;
    uint32_t length = sizeof(slot_time);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_SLOT_TIME,
                        &slot_time,
                        &length)) {
        LOG_ERR("get slot time fail for device %d", deviceId);
        return -EIO;
    }

    params->slot_time = slot_time;
    return 0;
}

/**
 * @brief Get STA beacon-miss (BMISS) threshold.
 *
 * Reads the current BMISS threshold configured in firmware via
 * qapi_WLAN_Get_Param using __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG.
 *
 * @param dev Pointer to the driver device instance (provides active deviceId).
 * @param params Output structure:
 *        - params->threshold: BMISS threshold (number of missed beacons) on success.
 *
 * @return 0 on success; negative error code on failure.
 */
static int qwifi_drv_get_bmiss_threshold(const struct device *dev, struct qcom_wifi_get_bmiss_threshold_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    uint32_t bmiss_threshold = 0;
    uint32_t length = sizeof(bmiss_threshold);

    if (0 != qapi_WLAN_Get_Param(deviceId,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_BMISS_CONFIG,
                        &bmiss_threshold,
                        &length)) {
        LOG_ERR("get BMISS threshold fail for device %d", deviceId);
        return -EIO;
    }

    params->threshold = bmiss_threshold;
    return 0;
}

int32_t set_op_mode(char *opmode, char *hidden_ssid)
{
	int32_t ret = -1;
    uint8_t hidden_flag = 0;
    qapi_WLAN_DEV_Mode_e devMode;
    const uint8_t dev_id = 0;

    if (!opmode || !hidden_ssid) {
        LOG_ERR("Invalid NULL parameters");
        return -EINVAL;
    }

    if(!strcmp(opmode,"ap")) {
        devMode = DEV_MODE_AP_E;
        if(strcmp(hidden_ssid,"hidden") == 0) {
            hidden_flag = 1;
        }
        else if(strcmp(hidden_ssid,"0") == 0 || strlen(hidden_ssid) == 0) {
            hidden_flag = 0;
        }
        else {
            LOG_ERR("Invalid hidden_ssid value: %s", hidden_ssid);
            return -EINVAL;
        }
    }
	else if(!strcmp(opmode,"station")) {
		devMode = DEV_MODE_STATION_E;
	}
	#ifdef NT_FN_CONCURRENCY
	else if(!strcmp(opmode,"ap_sta")) {
		devMode = DEV_MODE_AP_STA_E;
	}
	#endif
	else {
		LOG_INF("unknown mode %s\n",opmode);
		return -EINVAL;
	}

	ret = qapi_WLAN_Set_Param(dev_id,
							__QAPI_WLAN_PARAM_GROUP_WIRELESS,
							__QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
							&devMode,
							sizeof(devMode),
							FALSE);

	if(ret != QAPI_OK) {
		info_printf("set mode %s fail\n", opmode);
		return -EINVAL;
	}
	
	if(devMode == DEV_MODE_AP_E) {
		ret = qapi_WLAN_Set_Param(dev_id, 
								__QAPI_WLAN_PARAM_GROUP_WIRELESS,
								__QAPI_WLAN_PARAM_GROUP_WIRELESS_AP_ENABLE_HIDDEN_MODE,
								&hidden_flag,
								sizeof(hidden_flag),
								FALSE);
		if(ret != 0) {
			LOG_INF("Not able to set hidden mode for AP \r\n");
			return -EINVAL;
		}
	}
	return ret;
}

int32_t qwifi_drv_set_active_deviceid(const struct device *dev, uint16_t *deviceId)
{
    uint16_t active_device_id = *deviceId;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;

    dev_data->active_device = *deviceId;

    qapi_Status_t ret = qapi_WLAN_Set_Param(0,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_DEVICE_ID,
                        &active_device_id,
                        sizeof(active_device_id),
                        false);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set device id for device");
        return -EIO;
    }
    return 0;
}

static int qwifi_drv_set_op_mode(const struct device *dev, struct qcom_wifi_set_op_mode_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    char *opmode = params->opmode;
    char *hidden_ssid = params->hidden_ssid;
    int ret = 0;

    ret = set_op_mode(opmode, hidden_ssid);

    if (ret < 0) {
        LOG_ERR("Failed to set operation mode for device %d, ret=%d", deviceId, ret);
        return ret;
    }

    return 0;
}

static int qwifi_drv_intf_status(const struct device *dev, struct wifi_iface_status *status)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    struct qwifi_bss_status_t *bss_status = &dev_data->bss_status;
    struct qwifi_ap_status_t *ap_status = &dev_data->ap_status;

    qapi_WLAN_DEV_Mode_e dev_mode = DEV_MODE_STATION_E;
    uint32_t size = sizeof(dev_mode);
    qapi_WLAN_Set_Rate_Params_t rate_cfg = {0};
#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif
    qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                        __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                        &dev_mode, &size);
    if (dev_mode == DEV_MODE_STATION_E) {
        status->state = bss_status->connected ? WIFI_STATE_COMPLETED : WIFI_STATE_DISCONNECTED;
        status->ssid_len = bss_status->ssid_length;
        memcpy(status->bssid, bss_status->bssid, sizeof(status->bssid));
        strlcpy(status->ssid, bss_status->ssid, sizeof(status->ssid));
    } else if (dev_mode == DEV_MODE_AP_E) {
        status->state = ap_status->status;
        status->iface_mode = WIFI_MODE_AP;
        struct net_linkaddr *link_addr = net_if_get_link_addr(dev_data->iface);
        memcpy(status->bssid, link_addr->addr, sizeof(status->bssid));
        strlcpy(status->ssid, ap_status->ssid, sizeof(status->ssid));
    } else {
        LOG_ERR("%s:%d unknown mode %d.", __func__, __LINE__, dev_mode);
        return -EINVAL;
    }

    qapi_WLAN_Status_t wifi_status = {0};
    uint32_t length = sizeof(wifi_status);
    qapi_Status_t ret = qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                            __QAPI_WLAN_PARAM_GROUP_WIRELESS_WIFI_STATUS,
                                            &wifi_status, &length);
    if (ret != QAPI_OK) {
        LOG_ERR("%s:%d fail to get wifi status, ret %d", __func__, __LINE__, ret);
        return ret;
    }

    /** link mode */
    switch (wifi_status.link_mode) {
    case MODE_11B:
        status->link_mode = WIFI_1;
        break;
    case MODE_11A_ONLY:
        status->link_mode = WIFI_2;
        break;
    case MODE_11G:
        status->link_mode = WIFI_3;
        break;
    case MODE_11A_HT20:
    case MODE_11NG_HT20:
    case MODE_11ABGN_HT20:
        status->link_mode = WIFI_4;
        break;
    default:
        status->link_mode = WIFI_LINK_MODE_UNKNOWN;
        break;
    }

    /* security */
    switch (wifi_status.auth_mode) {
    case QAPI_WLAN_AUTH_WPA3_SAE_E:
        status->security = WIFI_SECURITY_TYPE_SAE;
        break;
    case QAPI_WLAN_AUTH_WPA2_PSK_E:
        status->security = WIFI_SECURITY_TYPE_PSK;
        break;
    case QAPI_WLAN_AUTH_WPA_PSK_E:
        status->security = WIFI_SECURITY_TYPE_WPA_PSK;
        break;
    case QAPI_WLAN_AUTH_NONE_E:
        status->security = WIFI_SECURITY_TYPE_NONE;
        break;
    default:
        status->security = WIFI_SECURITY_TYPE_UNKNOWN;
        break;
    }

    status->rssi = wifi_status.rssi;
    status->dtim_period = wifi_status.dtim_period;
    status->beacon_interval = wifi_status.beacon_interval;
    status->band = wifi_status.band;
    status->channel = wifi_status.channel;

    rate_cfg.rate_staid = dev_id;
    ret = qapi_WLAN_Get_Rate(&rate_cfg);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to get rate (staid=%u): %d", rate_cfg.rate_staid, ret);
        return ret;
    }

    if (rate_cfg.rate_p_rate >= MAX_RATE_INDEX) {
        LOG_ERR("Invalid rate index: %d", rate_cfg.rate_p_rate);
        return -EINVAL;
    }

    status->current_phy_tx_rate = rate_index_to_kbps[rate_cfg.rate_p_rate] / 1000.0;

    return 0;
}

static int qwifi_drv_send(const struct device *dev, struct net_pkt *pkt)
{
    qapi_Status_t ret;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    const size_t pkt_len = net_pkt_get_len(pkt);

    void *pkt_buf = nt_dpm_allocate_buffer_ext(pkt_len);
    if (!pkt_buf) {
        return -ENOMEM;
    }

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    size_t n = net_buf_linearize(pkt_buf, pkt_len, pkt->buffer, 0, pkt_len);
    if (n != pkt_len) {
        nt_dpm_free_buffer_ext(pkt_buf);
        LOG_ERR("%s:%d copy fail.", __func__, __LINE__);
        return -EFAULT;
    }

    ret = qwifi_hal_tx(deviceId, pkt_buf, pkt_len);
    if (ret != NT_OK) {
        nt_dpm_free_buffer_ext(pkt_buf);
        return -EAGAIN;
    }

    return 0;
}

qapi_Status_t qwifi_drv_eth_rx_cb(void *drv_intf_data, void *bufp, uint16_t len, void *hal_data)
{
    struct net_pkt *pkt;
    struct net_if *iface = (struct net_if *)drv_intf_data;

    ARG_UNUSED(hal_data);
    const struct device *dev = net_if_get_device(iface);
    
#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    pkt = net_pkt_rx_alloc_with_buffer(iface, len, AF_UNSPEC, 0, K_MSEC(100));
    if (!pkt) {
        return QAPI_ERR_NO_MEMORY;
    }

    net_pkt_write(pkt, bufp, len);
    net_recv_data(iface, pkt);

    return 0;
}

static void link_change_handler(void *drv_iface, uint32_t event, uint8_t* mac_addr)
{
    struct net_if *iface = (struct net_if *)drv_iface;

    switch (event) {
    case Q_LINKCHANGE_ADD:
        net_if_set_link_addr(iface, mac_addr, NET_ETH_ADDR_LEN, NET_LINK_ETHERNET);
        net_eth_carrier_on(iface);
        break;
    case Q_LINKCHANGE_REMOVE:
        net_eth_carrier_off(iface);
        break;
    default:
        LOG_WRN("%s:%d event: %d, ignored.", __FUNCTION__, __LINE__, event);
        break;
    }
}

static void qwifi_drv_intf_init(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct ethernet_context *eth_ctx = net_if_l2_data(iface);
    ethernet_init(iface);
    qwifi_hal_reg_rxcb(iface, qwifi_drv_eth_rx_cb, link_change_handler);

    dev_data->wlan_enabled = qapi_WLAN_Enable(true);
    qapi_WLAN_DEV_Mode_e devMode = DEV_MODE_STATION_E;
    qapi_WLAN_Set_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE, &devMode,
                        sizeof(devMode), false);
    dev_data->active_device = QCOM_DEV_STA_ID;

    eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;
    dev_data->iface = iface;

#ifdef CONFIG_PM_DEVICE
    qapi_WLAN_Activity_Register_CB(wifi_activity_cb);
    qapi_WLAN_Start_Check_Activity();

    pm_device_busy_set(dev);
#endif

#ifdef CONFIG_WIFI_NM
    wifi_nm_register_mgd_type_iface(wifi_nm_get_instance("wifi_sta"),
		    WIFI_TYPE_STA, iface);
#endif
    g_qwifi_dev_by_id[dev_data->active_device] = dev;
    LOG_DBG("%s, active_device=%d", __FUNCTION__, dev_data->active_device);
}

static void qwifi_drv_ap_intf_init(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct ethernet_context *eth_ctx = net_if_l2_data(iface);

    ethernet_init(iface);
    qwifi_hal_reg_rxcb(iface, qwifi_drv_eth_rx_cb, link_change_handler);

    dev_data->wlan_enabled = 1;
    dev_data->active_device = QCOM_DEV_AP_ID;

    net_if_carrier_off(iface);

    eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;
    dev_data->iface = iface;

#ifdef CONFIG_WIFI_NM
    wifi_nm_register_mgd_type_iface(wifi_nm_get_instance("wifi_sap"),
		    WIFI_TYPE_SAP, iface);
#endif

    g_qwifi_dev_by_id[dev_data->active_device] = dev;
    LOG_DBG("%s, active_device=%d", __FUNCTION__, dev_data->active_device);
}

static int qwifi_drv_set_rate(const struct device *dev, struct qcom_wifi_set_rate_params *params)
{
    qapi_WLAN_Set_Rate_Params_t cfg = {0};

    cfg.ra_ON = params->ra_ON;
    cfg.rate_staid = params->rate_staid;
    cfg.rate_p_rate = params->rate_p_rate;
    cfg.rate_s_rate = params->rate_s_rate;
    cfg.rate_t_rate = params->rate_t_rate;

    qapi_Status_t ret = qapi_WLAN_Set_Rate(&cfg);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to set rate (staid=%u, p=%u, s=%u, t=%u): %d",
                cfg.rate_staid, cfg.rate_p_rate, cfg.rate_s_rate, cfg.rate_t_rate, ret);
        return -EIO;
    }
    return 0;
}

static int qwifi_drv_get_rate(const struct device *dev, struct qcom_wifi_set_rate_params *params)
{
    qapi_WLAN_Set_Rate_Params_t cfg = {0};

    cfg.rate_staid = params->rate_staid;

    qapi_Status_t ret = qapi_WLAN_Get_Rate(&cfg);
    if (ret != QAPI_OK) {
        LOG_ERR("Failed to get rate (staid=%u): %d", cfg.rate_staid, ret);
        return -EIO;
    }

    params->ra_ON = cfg.ra_ON;
    params->rate_p_rate = cfg.rate_p_rate;
    params->rate_s_rate = cfg.rate_s_rate;
    params->rate_t_rate = cfg.rate_t_rate;

    return 0;
}



static int get_config(const struct device *dev, enum ethernet_config_type type,
                      struct ethernet_config *config)
{
    int ret = 0;

    switch (type) {
    case ETHERNET_CONFIG_TYPE_EXTRA_TX_PKT_HEADROOM:
        config->extra_tx_pkt_headroom = 0;
        break;
    default:
        return -EINVAL;
    }

    return ret;
}

#ifdef CONFIG_PM_DEVICE
extern uint64_t bmps_duration;
extern struct k_timer bmps_timer;
static int device_wlan_pm_action(const struct device *dev, enum pm_device_action pm_action)
{

    int ret = 0;

    switch (pm_action) {
        case PM_DEVICE_ACTION_SUSPEND:
            ret = qapi_WLAN_Suspend();
            if(ret != QAPI_OK)
            {
                pm_device_busy_set(dev);
                LOG_ERR("%s: qapi_WLAN_Suspend return:%d", __FUNCTION__, ret);
                ret = -ret;
            }
            break;
        case PM_DEVICE_ACTION_RESUME:
            qapi_WLAN_Resume();
            pm_device_busy_set(dev);
            break;
        default:
            break;
    }

    return ret;
}

PM_DEVICE_DT_INST_DEFINE(0, device_wlan_pm_action);
#endif

static int qwifi_drv_channel(const struct device *dev, struct wifi_channel_info *channel_info)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

    if (!channel_info) {
        return -EINVAL;
    }

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    if (channel_info->oper == WIFI_MGMT_SET) {
        uint32_t channel[2] = {0, 0};
        channel[0] = channel_info->channel;
        channel[1] = 0;

        qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS_CHANNEL,
                                               (void *)&channel, sizeof(channel), false);
        if (ret != QAPI_OK) {
            LOG_ERR("%s:%d Set channel %u failed: %d", __func__, __LINE__, channel_info->channel, ret);
            return -EAGAIN;
        }

        return 0;
    } else if (channel_info->oper == WIFI_MGMT_GET) {
        qapi_WLAN_Status_t wifi_status = {0};
        uint32_t length = sizeof(wifi_status);
        qapi_Status_t ret = qapi_WLAN_Get_Param(deviceId,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS_WIFI_STATUS,
                                               &wifi_status, &length);
        if (ret != QAPI_OK) {
            LOG_ERR("%s:%d Get wifi status failed: %d", __func__, __LINE__, ret);
            return -EAGAIN;
        }

        channel_info->channel = wifi_status.channel;
        return 0;
    }

    return -ENOTSUP;
}

static int qwifi_drv_reg_domain(const struct device *dev, struct wifi_reg_domain *regd)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

    if (!regd) {
        return -EINVAL;
    }

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    if (regd->oper == WIFI_MGMT_SET) {
        uint8_t country_code[3] = {0};
        country_code[0] = regd->country_code[0];
        country_code[1] = regd->country_code[1];
        country_code[2] = 0;

        qapi_Status_t ret = qapi_WLAN_Set_Param(deviceId,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS_COUNTRY_CODE,
                                               (void *)country_code, sizeof(country_code), false);

        if (ret != QAPI_OK) {
            LOG_ERR("%s:%d Set country code %c%c failed: %d", __func__, __LINE__,
                    regd->country_code[0], regd->country_code[1], ret);
            return -EAGAIN;
        }

        return 0;
    } else if (regd->oper == WIFI_MGMT_GET) {
        uint8_t country_code[4] = {0};
        uint32_t length = 4;
        qapi_Status_t ret = qapi_WLAN_Get_Param(deviceId,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                               __QAPI_WLAN_PARAM_GROUP_WIRELESS_COUNTRY_CODE,
                                               country_code, &length);
        bool country_code_valid = false;

        if (ret == QAPI_OK && country_code[0] != 0 && country_code[1] != 0) {
            regd->country_code[0] = country_code[0];
            regd->country_code[1] = country_code[1];
            country_code_valid = true;
            LOG_DBG("%s:%d Country code: %c%c", __func__, __LINE__, country_code[0], country_code[1]);
        } else {
            regd->country_code[0] = 'W';
            regd->country_code[1] = 'W';
            LOG_DBG("%s:%d Failed to get country code (ret=%d), using default: WW", __func__, __LINE__, ret);
        }

        qapi_WLAN_Reg_Evt_t reg_evt = {0};
        ret = qapi_WLAN_Get_Regulatory_Info(&reg_evt);
        if (ret != QAPI_OK) {
            LOG_ERR("%s:%d qapi_WLAN_Get_Regulatory_Info failed: %d", __func__, __LINE__, ret);
            regd->num_channels = 0;
            return 0;
        }

        if (!regd->chan_info) {
            LOG_ERR("%s:%d chan_info buffer not provided by caller", __func__, __LINE__);
            return -EINVAL;
        }

        int idx = 0;
        const int max_out = MAX_REG_CHAN_NUM;
        int total_rules = reg_evt.num_2g_reg_rules + reg_evt.num_5g_reg_rules;

        LOG_DBG("%s:%d Regulatory domain: %c%c, rules: %d (2G:%d + 5G:%d)", __func__, __LINE__,
                reg_evt.alpha[0], reg_evt.alpha[1], total_rules,
                reg_evt.num_2g_reg_rules, reg_evt.num_5g_reg_rules);

        for (int r = 0; r < total_rules && idx < max_out; r++) {
            uint16_t start_freq = reg_evt.reg_rules[r].start_freq;
            uint16_t end_freq = reg_evt.reg_rules[r].end_freq;
            uint8_t reg_power = reg_evt.reg_rules[r].reg_power;
            uint16_t flags = reg_evt.reg_rules[r].flag_info;

            LOG_DBG("%s:%d Rule[%d]: %u-%u MHz, power=%u dBm, flags=0x%04x", __func__, __LINE__,
                r, start_freq, end_freq, reg_power, flags);

            uint16_t step = (start_freq >= 5000) ? 20 : 5;
            for (uint16_t freq = start_freq + EDGE_BAND_10MHz; freq <= end_freq - EDGE_BAND_10MHz && idx < max_out; freq += step) {
                if ((step == 5 && (freq < 2412 || (freq > 2484 && freq < 5000))) ||
                    (step == 20 && (freq < 5180 || freq > 5895))) {
                    continue;
                }
                if (step == 5 && freq > 2472 && freq != 2484)
                    continue;

                regd->chan_info[idx].center_frequency = freq;
                regd->chan_info[idx].max_power = reg_power;
                regd->chan_info[idx].supported = 1;
                regd->chan_info[idx].passive_only = (flags & 0x02) ? 1 : 0;
                regd->chan_info[idx].dfs = (step == 20 && (flags & 0x10)) ? 1 : 0;
                idx++;
            }
        }

        regd->num_channels = idx;
        LOG_DBG("%s:%d Generated %d channels from %d regulatory rules", __func__, __LINE__, idx, total_rules);
        return 0;
    }
    return -ENOTSUP;
}

static int qwifi_ps_drv_set_power_optimization_enable_in_bmps(const struct device *dev, struct qcom_wifi_pm_power_optimization_params *param)
{
    int err = 0;
    uint8_t enable = param->enable ? 1 : 0;

    qapi_Status_t ret = qapi_bmps_power_optimization_enable(enable);
    if(ret != QAPI_OK) {
        LOG_ERR("fail to set bmps power optimization, ret = %d", ret);
        err = -EINVAL;
    }

    return 0;
}

static int qwifi_ps_drv_set_compress_qos_null_enable_in_bmps(const struct device *dev, struct qcom_wifi_pm_compress_qos_null_params *param)
{
    int err = 0;
    uint8_t enable = param->enable ? 1 : 0;

    qapi_Status_t ret = qapi_bmps_compress_qos_null_enable(enable);
    if(ret != QAPI_OK) {
        LOG_ERR("fail to set compress qos null, ret = %d", ret);
        ret = -EINVAL;
    }

    return err;
}

static int qwifi_ps_drv_set_rx_filter_in_bmps(const struct device *dev, struct qcom_wifi_pm_rx_filter_params *param)
{
    int err = 0;
    uint8_t enable = param->enable;

    qapi_bmps_rx_filter_enable(enable);

    if (enable) {
        qapi_bmps_bcmc_rx_filter_cb_register(param->bmps_rx_filter_cb, NULL);
    }

    return err;
}

static int qwifi_ps_drv_set_bmps_enable(const struct device *dev, struct qcom_wifi_pm_bmps_params *param)
{
    int err = 0;

    qapi_Status_t ret = qapi_bmps_cfg(param->enable, 0);
    if (ret) {
        LOG_ERR("fail to enable bmps. ret %d.", ret);
        err = -EINVAL;
    }

    return err;
}

static int qwifi_ps_drv_ignore_bc_mc_in_bmps(const struct device *dev, struct qcom_wifi_pm_ignore_bc_mc_params *param)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

    qapi_WLAN_ignore_bcmc_in_bmps(dev_id, param->enable);

    return 0;
}

static int wifi_ps_timeout(uint32_t timeout_ms)
{
    int err = 0;

    LOG_INF("Set bmps idle_timeout to %d ms", timeout_ms);
    qapi_Status_t ret = qapi_bmps_cfg(2, timeout_ms);
    if (ret) {
        LOG_ERR("idle timeout set fail. ret %d, timeout = %d", ret, timeout_ms);
        err = -EINVAL;
    }

    return err;
}

static int qwifi_power_save(const struct device *dev, struct wifi_ps_params *params)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    int ret = 0;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    switch (params->type) {
    case WIFI_PS_PARAM_TIMEOUT:
        ret = wifi_ps_timeout(params->timeout_ms);
        break;
    case WIFI_PS_PARAM_LISTEN_INTERVAL:
        if ((params->listen_interval < WIFI_LISTEN_INTERVAL_MIN) ||
            (params->listen_interval > WIFI_LISTEN_INTERVAL_MAX)) {
            params->fail_reason = WIFI_PS_PARAM_LISTEN_INTERVAL_RANGE_INVALID;
            ret = -EINVAL;
            break;

        }
        qapi_WLAN_Listen_Interval_Params_t listen_interval = {0};
        listen_interval.time = params->listen_interval;
        qapi_Status_t err = qapi_WLAN_Set_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                                __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_LISTEN_INTERVAL_IN_TU,
                                                &listen_interval, sizeof(listen_interval), FALSE);
        if (err != QAPI_OK) {
            LOG_ERR("fail to set listen interval err = %d", err);
            ret = -EINVAL;
        }
        break;
    case WIFI_PS_PARAM_WAKEUP_MODE:
    case WIFI_PS_PARAM_MODE:
    case WIFI_PS_PARAM_STATE:
    case WIFI_PS_PARAM_EXIT_STRATEGY:
    default:
        params->fail_reason = WIFI_PS_PARAM_FAIL_OPERATION_NOT_SUPPORTED;
        ret = -ENOTSUP;
        break;
    }

    return ret;
}

int qwifi_get_power_save(const struct device *dev, struct wifi_ps_config *config)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    uint16_t listen_interval;
    uint32_t length = sizeof(listen_interval);

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif

    qapi_WLAN_Get_Param(dev_id,
                         __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                         __QAPI_WLAN_PARAM_GROUP_WIRELESS_STA_LISTEN_INTERVAL_IN_TU,
                         &listen_interval,
                         &length);

    config->ps_params.listen_interval = listen_interval;
    config->ps_params.exit_strategy = WIFI_PS_EXIT_EVERY_TIM;
    config->ps_params.mode = WIFI_PS_MODE_LEGACY;

    return 0;
}

static int qwifi_drv_dev_init(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    if (strcmp(dev->name, "qwifi_sta") == 0) {
        g_qwifi_dev_by_id[QCOM_DEV_STA_ID] = dev;
        qwifi_init();
        qapi_WLAN_Set_Callback(qwifi_drv_event_handler, (void *)dev);
    }
    else if (strcmp(dev->name, "qwifi_sap") == 0) {
        g_qwifi_dev_by_id[QCOM_DEV_AP_ID] = dev;
    }
    dev_data->dev = dev;

    dev_data->e_cipher = QAPI_WLAN_CRYPT_AES_CRYPT_E;

    struct qcom_wifi_mgmt_ops qwifi_ops = {
        .set_tx_power = qwifi_drv_set_tx_power,
        .get_tx_power = qwifi_drv_get_tx_power,
        .unit_test = qwifi_drv_unit_test,
        .set_rts_cts        = qwifi_drv_set_rts_cts,
        .set_rts_rate       = qwifi_drv_set_rts_rate,
        .set_edca_param_cfg = qwifi_drv_set_edca_param_cfg,
        .set_threshold      = qwifi_drv_set_threshold,
        .set_ba_win_timing    = qwifi_drv_set_ba_win_timing,
        .set_slot_time      = qwifi_drv_set_slot_time,
        .set_phy_mode       = qwifi_drv_set_phy_mode,
        .set_aggregation    = qwifi_drv_set_aggregation,
        .set_amsdu_rx       = qwifi_drv_set_amsdu_rx,
        .set_rate           = qwifi_drv_set_rate,
        .set_sap_csa        = qwifi_drv_set_sap_csa,

        .get_rts_cts        = qwifi_drv_get_rts_cts,
        .get_rts_rate       = qwifi_drv_get_rts_rate,
        .get_edca_param_cfg = qwifi_drv_get_edca_param_cfg,
        .get_threshold      = qwifi_drv_get_threshold,
        .get_ba_win_timing    = qwifi_drv_get_ba_win_timing,
        .get_slot_time      = qwifi_drv_get_slot_time,
        .get_phy_mode       = qwifi_drv_get_phy_mode,
        .get_power_mode     = qwifi_drv_get_power_mode,
        .get_boot_reason    = qwifi_drv_get_boot_reason,
        .get_mac_address    = qwifi_drv_get_mac_address,
        .get_concurrency_mode = qwifi_drv_get_concurrency_mode,
        .get_operation_mode = qwifi_drv_get_operation_mode,
        .get_rate           = qwifi_drv_get_rate,
        .set_bmiss_threshold      = qwifi_drv_set_bmiss_threshold,
        .get_bmiss_threshold      = qwifi_drv_get_bmiss_threshold,
        .set_op_mode = qwifi_drv_set_op_mode,
        .set_device_id = qwifi_drv_set_active_deviceid,
        .set_bmps_enable = qwifi_ps_drv_set_bmps_enable,
        .set_ignore_bc_mc_in_bmps = qwifi_ps_drv_ignore_bc_mc_in_bmps,
        .set_power_optimization_enable_in_bmps = qwifi_ps_drv_set_power_optimization_enable_in_bmps,
        .set_compress_qos_null_enable_in_bmps = qwifi_ps_drv_set_compress_qos_null_enable_in_bmps,
        .set_rx_filter_in_bmps = qwifi_ps_drv_set_rx_filter_in_bmps
    };
    dev_data->qcom_wifi_cmd = qwifi_ops;

    return 0;
}

static const struct wifi_mgmt_ops qwifi_drv_mgmt = {
    .scan = qwifi_drv_scan,
    .connect = qwifi_drv_connect,
    .disconnect = qwifi_drv_disconnect,
    .iface_status = qwifi_drv_intf_status,
    .channel = qwifi_drv_channel,
    .reg_domain = qwifi_drv_reg_domain,
    .ap_enable = ap_enable,
    .ap_disable = ap_disable,
    .ap_sta_disconnect = ap_sta_disconnect,
    .set_power_save = qwifi_power_save,
    .get_power_save_config = qwifi_get_power_save,
};

static const struct net_wifi_mgmt_offload qwifi_drv_api = {
    .wifi_iface.iface_api.init = qwifi_drv_intf_init,
    .wifi_iface.send = qwifi_drv_send,
    .wifi_iface.get_config = get_config,
    .wifi_mgmt_api = &qwifi_drv_mgmt,
};

#ifdef CONFIG_WIFI_NM
DEFINE_WIFI_NM_INSTANCE(wifi_sta, &qwifi_drv_mgmt);
#endif

NET_DEVICE_INIT_INSTANCE(qwifi_sta, "qwifi_sta", 0, qwifi_drv_dev_init, PM_DEVICE_DT_INST_GET(0), &g_wifi_dev_data, &g_wifi_dev_cfg,
                         CONFIG_WIFI_INIT_PRIORITY, &qwifi_drv_api, ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2),
                         NET_ETH_MTU);

static const struct wifi_mgmt_ops qwifi_ap_mgmt = {
    .ap_enable = ap_enable,
    .ap_disable = ap_disable,
    .ap_sta_disconnect = ap_sta_disconnect,
    .iface_status = qwifi_drv_intf_status,
    .channel = qwifi_drv_channel,
    .reg_domain = qwifi_drv_reg_domain,
};

static const struct net_wifi_mgmt_offload qwifi_ap_api = {
    .wifi_iface.iface_api.init = qwifi_drv_ap_intf_init,
    .wifi_iface.send = qwifi_drv_send,
    .wifi_iface.get_config = get_config,
    .wifi_mgmt_api = &qwifi_ap_mgmt,
};

#ifdef CONFIG_WIFI_NM
DEFINE_WIFI_NM_INSTANCE(wifi_sap, &qwifi_ap_mgmt);
#endif

NET_DEVICE_INIT_INSTANCE(qwifi_uap, "qwifi_sap", 1, qwifi_drv_dev_init, NULL, &g_wifi_dev_data_sap, &g_wifi_dev_cfg_sap,
                         CONFIG_WIFI_SAP_PRIORITY, &qwifi_ap_api, ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2),
                         NET_ETH_MTU);
