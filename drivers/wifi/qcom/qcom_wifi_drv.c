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
#ifdef CONFIG_PM_DEVICE
#include <zephyr/pm/device.h>
#endif

#include <qwifi_api.h>
#include <libwifi.h>
#include <libwifi/wlan_defs.h>
#include "inc/qcom_wifi_mgmt.h"

#define SCAN_MODE_BLOCKING 1
#define SCAN_MODE_UNBLOCKING 2
#define NT_DEV_STA_ID 1

struct qwifi_bss_status_t {
    bool connected;
    uint8_t bssid[NET_ETH_ADDR_LEN];
    int ssid_length;
    char ssid[WIFI_SSID_MAX_LEN + 1];
};

struct qwifi_drv_dev_data_t {
    uint8_t wlan_enabled;
    uint8_t active_device;
    qapi_WLAN_Crypt_Type_e e_cipher;
    scan_result_cb_t scan_cb;
    struct wifi_connect_req_params cfg_connect;
    struct qwifi_bss_status_t bss_status;
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

const struct qcom_wifi_mgmt_ops *const get_qcom_wifi_api(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct qcom_wifi_mgmt_ops *off_api;

	if (dev == NULL) {
		return NULL;
	}
	struct qwifi_drv_dev_data_t *dev_data = dev->data;
	off_api = &dev_data->qcom_wifi_cmd;
#ifdef CONFIG_WIFI_NM
	struct wifi_nm_instance *nm = wifi_nm_get_instance_iface(iface);

	if (nm) {
		return nm->ops;
	}
#endif /* CONFIG_WIFI_NM */
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
    struct wifi_ap_sta_info sta_info = {0};
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct net_if *iface = dev_data->iface;
    uint8_t *sta_mac = info->bssid;

    if (info->evt_hdr.status != QAPI_OK) {
        return -EPERM;
    }

    /* filter ap itself connection event. */
    struct net_linkaddr * link_addr = net_if_get_link_addr(iface);
    if (!memcmp(link_addr->addr, sta_mac, link_addr->len)) {
        return 0;
    }

    memcpy(&sta_info.mac, sta_mac, sizeof(sta_info.mac));
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

    if (dev_mode == DEV_MODE_STATION_E) {
        station_disconnect_event(dev, info);
    } else if (dev_mode == DEV_MODE_AP_E) {
        ap_station_disconnect_event(dev, info);
    } else {
        LOG_ERR("Unknown dev mode %d", dev_mode);
    }
}

static void qwifi_drv_event_handler(uint8_t dev_id, uint32_t event, void *context, void *private, uint32_t length)
{
    switch (event) {
    case QAPI_WLAN_SCAN_COMPLETE_CB_E:
        qwifi_scan_complete_event(context, private);
        break;
    case QAPI_WLAN_CONNECT_CB_E:
        qwifi_connect_event(context, private);
        break;
    case QAPI_WLAN_DISCONNECT_CB_E:
        qwifi_disconnect_event(context, private);
        break;
    default:
        LOG_WRN("%s:%d event: %d, ignored.", __FUNCTION__, __LINE__, event);
        break;
    }
}

static int qwifi_drv_disconnect(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;

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

    qapi_WLAN_DEV_Mode_e mode = DEV_MODE_STATION_E;
    qapi_Status_t ret = qapi_WLAN_Set_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE,
                                            &mode, sizeof(mode), false);
    if (ret) {
        LOG_ERR("set station mode fail");
        return -EINVAL;
    }

    if (params->ssid_length) {
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SSID,
                            (void *)params->ssid, params->ssid_length, false);
        LOG_DBG("ssid=%s", params->ssid);
    }

    if (!params->bssid[0]) {
        qapi_WLAN_Set_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_BSSID,
                            (void *)params->bssid, __QAPI_WLAN_MAC_LEN, false);
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

    LOG_DBG("%s", __FUNCTION__);
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
        LOG_INF("scan ssid=%s", params->ssids[0]);
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
    const uint8_t dev_id = 0;

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

    ret = qapi_WLAN_Commit(dev_id);
    if (ret) {
        LOG_WRN("ret = %d, commit fail.", ret);
        return -EAGAIN;
    }

    return 0;
}

static int ap_disable(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;

    qapi_WLAN_Disconnect(dev_id);

    return 0;
}

static int ap_sta_disconnect(const struct device *dev, const uint8_t *mac)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    qapi_WLAN_AP_Disconnect_Station(dev_id, mac, NET_ETH_ADDR_LEN);
    return 0;
}

static int qwifi_drv_intf_status(const struct device *dev, struct wifi_iface_status *status)
{
    uint32_t length = 0;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t dev_id = dev_data->active_device;
    struct qwifi_bss_status_t *bss_status = &dev_data->bss_status;

    status->state = bss_status->connected ? WIFI_STATE_COMPLETED : WIFI_STATE_DISCONNECTED;
    status->ssid_len = bss_status->ssid_length;
    memcpy(status->bssid, bss_status->bssid, sizeof(status->bssid));
    strlcpy(status->ssid, bss_status->ssid, sizeof(status->ssid));
    status->ssid[WIFI_SSID_MAX_LEN] = '\0';

    qapi_WLAN_Status_t wifi_status = {0};
    length = sizeof(wifi_status);
    qapi_Status_t ret = qapi_WLAN_Get_Param(dev_id, __QAPI_WLAN_PARAM_GROUP_WIRELESS,
                                            __QAPI_WLAN_PARAM_GROUP_WIRELESS_WIFI_STATUS,
                                            &wifi_status, &length);
    if (ret != QAPI_OK) {
        return ret;
    }

    /** link mode */
    switch (wifi_status.link_mode) {
    case MODE_11B:
        status->link_mode = WIFI_1;
        break;
    case MODE_11A_ONLY:
    case MODE_11A_HT20:
        status->link_mode = WIFI_2;
        break;
    case MODE_11G:
        status->link_mode = WIFI_3;
        break;
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
    case QAPI_WLAN_AUTH_WPA2_PSK_E:
        status->security = WIFI_SECURITY_TYPE_PSK;
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
    dev_data->active_device = NT_DEV_STA_ID;

    eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;
    dev_data->iface = iface;

#ifdef CONFIG_PM_DEVICE
    pm_device_busy_set(dev);
#endif
    LOG_DBG("%s", __FUNCTION__);
}

static int qwifi_drv_dev_init(const struct device *dev)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;

    dev_data->dev = dev;
    qwifi_init();
    qapi_WLAN_Set_Callback(qwifi_drv_event_handler, (void *)dev);
    dev_data->e_cipher = QAPI_WLAN_CRYPT_AES_CRYPT_E;

    struct qcom_wifi_mgmt_ops qwifi_ops = {
        .set_tx_power = qwifi_drv_set_tx_power,
        .get_tx_power = qwifi_drv_get_tx_power,
    };
    dev_data->qcom_wifi_cmd = qwifi_ops;

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
    LOG_INF("%s", __FUNCTION__);
    int ret = 0;

    switch (pm_action) {
        case PM_DEVICE_ACTION_SUSPEND:
            ret = qapi_WLAN_Suspend();
            if(ret != QAPI_OK)
            {
                LOG_ERR("%s: qapi_WLAN_Suspend return:%d", __FUNCTION__, ret);
                ret = -ret;
            }
            break;
        case PM_DEVICE_ACTION_RESUME:
            qapi_WLAN_Resume();
            if (k_timer_remaining_get(&bmps_timer) > 0) {
                if(bmps_duration== 0)
                {
                    k_timer_stop(&bmps_timer);
                    pm_device_busy_set(dev);
                    LOG_INF("%s: bmps_duration is 0, exit bmps.", __FUNCTION__);
                }
            }
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
            for (uint16_t freq = start_freq; freq <= end_freq && idx < max_out; freq += step) {
                if ((step == 5 && (freq < 2412 || (freq > 2484 && freq < 5000))) ||
                    (step == 20 && (freq < 5180 || freq > 5825))) {
                    continue;
                }
                if (step == 5 && freq > 2472 && freq != 2484)
                    continue;
                if (freq == 2484 && !(start_freq <= 2484 && end_freq >= 2484))
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
};

static const struct net_wifi_mgmt_offload qwifi_drv_api = {
    .wifi_iface.iface_api.init = qwifi_drv_intf_init,
    .wifi_iface.send = qwifi_drv_send,
    .wifi_iface.get_config = get_config,
    .wifi_mgmt_api = &qwifi_drv_mgmt,
};

NET_DEVICE_INIT_INSTANCE(qwifi_sta, "qwifi_sta", 0, qwifi_drv_dev_init, PM_DEVICE_DT_INST_GET(0), &g_wifi_dev_data, &g_wifi_dev_cfg,
                         CONFIG_WIFI_INIT_PRIORITY, &qwifi_drv_api, ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2),
                         NET_ETH_MTU);
