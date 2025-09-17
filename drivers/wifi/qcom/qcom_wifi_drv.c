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
#include <libwifi/wlan_defs.h>

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
    int8_t qwifi_dev_id;
    uint8_t wlan_enabled;
    uint8_t active_device;
    qapi_WLAN_Crypt_Type_e e_cipher;
    uint8_t mac_addr[NET_ETH_ADDR_LEN];
    struct wifi_scan_params cfg_scan;
    scan_result_cb_t scan_cb;
    struct wifi_connect_req_params cfg_connect;
    struct qwifi_bss_status_t bss_status;
    struct net_if *iface;
    const struct device *dev;
    uint8_t frame_buf[NET_ETH_MAX_FRAME_SIZE];
};

struct qwifi_drv_dev_cfg_t {
    int32_t scan_mode;
    int reserved;
};

static struct qwifi_drv_dev_data_t g_wifi_dev_data;
static struct qwifi_drv_dev_cfg_t g_wifi_dev_cfg = {
    .scan_mode = SCAN_MODE_UNBLOCKING,
};

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

static void qwifi_connect_event(struct device *dev, qapi_WLAN_Join_Comp_Evt_t *cxnInfo)
{
    int connect_status = WIFI_STATUS_CONN_SUCCESS;
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct qwifi_bss_status_t *bss = &dev_data->bss_status;
    uint8_t *mac = cxnInfo->bssid;

    LOG_DBG("connect event report:");
    LOG_DBG("ssid: %s", dev_data->cfg_connect.ssid);
    LOG_DBG("mac addr: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    LOG_DBG("security: %d", dev_data->cfg_connect.security);
    LOG_DBG("connection status: %d", cxnInfo->bss_Connection_Status);
    LOG_DBG("status: %d", cxnInfo->evt_hdr.status);
    LOG_DBG("reason code: %d", cxnInfo->reason_code);

    if (cxnInfo->ssid_Length) {
        strlcpy(bss->ssid, cxnInfo->ssid, WIFI_SSID_MAX_LEN);
        bss->ssid_length = cxnInfo->ssid_Length;
    }

    memcpy(bss->bssid, cxnInfo->bssid, NET_ETH_ADDR_LEN);

    if (cxnInfo->evt_hdr.status == QAPI_OK) {
        bss->connected = true;
    } else {
        connect_status = WIFI_STATUS_CONN_FAIL;
        bss->connected = false;
    }

    wifi_mgmt_raise_connect_result_event(dev_data->iface, connect_status);

#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
    if (bss->connected) {
        net_dhcpv4_start(dev_data->iface);
    }
#endif
}

static void qwifi_disconnect_event(struct device *dev, void *private)
{
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct qwifi_bss_status_t *bss = &dev_data->bss_status;

    LOG_DBG("disconnect event report:");
    LOG_DBG("ssid = %s", dev_data->bss_status.ssid);

    bss->connected = false;
    wifi_mgmt_raise_disconnect_result_event(dev_data->iface, WIFI_REASON_DISCONN_SUCCESS);

#if defined(CONFIG_WIFI_QCOM_AUTO_DHCPV4)
    net_dhcpv4_stop(dev_data->iface);
#endif
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
    const uint8_t *psk = NULL;
    uint8_t psk_length = 0;

    LOG_DBG("%s", __FUNCTION__);
    memcpy(&dev_data->cfg_connect, params, sizeof(struct wifi_connect_req_params));

    switch (params->security) {
    case WIFI_SECURITY_TYPE_PSK:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA2_PSK_E;
        break;
    case WIFI_SECURITY_TYPE_WPA_PSK:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA_PSK_E;
        break;
    case WIFI_SECURITY_TYPE_SAE:
        e_wpa_ver = QAPI_WLAN_AUTH_WPA3_SAE_E;
        break;
    case WIFI_SECURITY_TYPE_NONE:
        e_wpa_ver = QAPI_WLAN_AUTH_NONE_E;
        break;
    default:
        LOG_ERR("Authentication method not supported");
        return -EIO;
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
        if ((params->security == WIFI_SECURITY_TYPE_SAE) && (params->sae_password)) {
            psk = params->sae_password;
            psk_length = params->sae_password_length;
        }

        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
                            __QAPI_WLAN_PARAM_GROUP_SECURITY_AUTH_MODE, (void *)&e_wpa_ver,
                            sizeof(qapi_WLAN_Auth_Mode_e), false);
        qapi_WLAN_Set_Param(deviceId, __QAPI_WLAN_PARAM_GROUP_WIRELESS_SECURITY,
                            __QAPI_WLAN_PARAM_GROUP_SECURITY_ENCRYPTION_TYPE, (void *)&dev_data->e_cipher,
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
    memcpy(&dev_data->cfg_scan, params, sizeof(struct wifi_scan_params));

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
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    uint8_t deviceId = dev_data->active_device;
    const int pkt_len = net_pkt_get_len(pkt);

    net_pkt_read(pkt, dev_data->frame_buf, pkt_len);
    qwifi_hal_tx(deviceId, dev_data->frame_buf, pkt_len);

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

static void qwifi_drv_intf_init(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    struct qwifi_drv_dev_data_t *dev_data = dev->data;
    struct ethernet_context *eth_ctx = net_if_l2_data(iface);

    qapi_WLAN_DEV_Mode_e devMode = DEV_MODE_STATION_E;

    dev_data->wlan_enabled = qapi_WLAN_Enable(true);
    qapi_WLAN_Set_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_OPERATION_MODE, &devMode,
                        sizeof(devMode), false);
    dev_data->active_device = NT_DEV_STA_ID;

    eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;
    dev_data->iface = iface;

    uint32_t mac_len = sizeof(dev_data->mac_addr);
    qapi_WLAN_Get_Param(0, __QAPI_WLAN_PARAM_GROUP_WIRELESS, __QAPI_WLAN_PARAM_GROUP_WIRELESS_MAC_ADDRESS,
                        dev_data->mac_addr, &mac_len);

    qwifi_hal_reg_rxcb(iface, qwifi_drv_eth_rx_cb);
    net_if_set_link_addr(iface, dev_data->mac_addr, NET_ETH_ADDR_LEN, NET_LINK_ETHERNET);
    ethernet_init(iface);
    net_if_carrier_off(iface);
    net_eth_carrier_on(iface);
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

    return 0;
}

#ifdef CONFIG_PM_DEVICE

static int device_wlan_pm_action(const struct device *dev, enum pm_device_action pm_action)
{
    //printk("%s\n", __func__);
    int ret = 0;

    switch (pm_action) {
        case PM_DEVICE_ACTION_SUSPEND:
            ret = qapi_WLAN_Suspend();
            //printk("%s ret:%d\r\n",__func__, ret);
            if(ret != QAPI_OK)
                ret = -EFAULT;
            break;
        case PM_DEVICE_ACTION_RESUME:
            qapi_WLAN_Resume();
            break;
        default:
            break;
    }

    return ret;
}

PM_DEVICE_DT_INST_DEFINE(0, device_wlan_pm_action);
#endif

static const struct wifi_mgmt_ops qwifi_drv_mgmt = {
    .scan = qwifi_drv_scan,
    .connect = qwifi_drv_connect,
    .disconnect = qwifi_drv_disconnect,
    .iface_status = qwifi_drv_intf_status,
};

static const struct net_wifi_mgmt_offload qwifi_drv_api = {
    .wifi_iface.iface_api.init = qwifi_drv_intf_init,
    .wifi_iface.send = qwifi_drv_send,
    .wifi_mgmt_api = &qwifi_drv_mgmt,
};

NET_DEVICE_INIT_INSTANCE(qwifi_sta, "qwifi_sta", 0, qwifi_drv_dev_init, PM_DEVICE_DT_INST_GET(0), &g_wifi_dev_data, &g_wifi_dev_cfg,
                         CONFIG_WIFI_INIT_PRIORITY, &qwifi_drv_api, ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2),
                         NET_ETH_MTU);
