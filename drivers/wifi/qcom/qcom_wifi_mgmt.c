#include <zephyr/logging/log.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include "inc/qcom_wifi_mgmt.h"

const struct qcom_wifi_mgmt_ops *const get_qcom_wifi_api(struct net_if *iface);

static int wifi_set_tx_power(uint64_t mgmt_request, struct net_if *iface,
				  void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *qcom_wifi_mgmt_api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_tx_power_params *txpower = data;

	if (qcom_wifi_mgmt_api == NULL || qcom_wifi_mgmt_api->set_tx_power == NULL) {
		return -ENOTSUP;
	}

	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}

	if (!data || len != sizeof(*txpower)) {
		return -EINVAL;
	}

	return qcom_wifi_mgmt_api->set_tx_power(dev, txpower);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_TX_POWER, wifi_set_tx_power);

static int wifi_get_tx_power(uint64_t mgmt_request, struct net_if *iface,
				  void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *qcom_wifi_mgmt_api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_tx_power_params *txpower = data;

	if (qcom_wifi_mgmt_api == NULL || qcom_wifi_mgmt_api->get_tx_power == NULL) {
		return -ENOTSUP;
	}

	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}

	if (!data || len != sizeof(*txpower)) {
		return -EINVAL;
	}

	return qcom_wifi_mgmt_api->get_tx_power(dev, txpower);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_TX_POWER, wifi_get_tx_power);

static int wifi_unit_test(uint64_t mgmt_request, struct net_if *iface,
			  void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *qcom_wifi_mgmt_api = get_qcom_wifi_api(iface);
	struct qcom_wifi_unit_test_params *params = data;

	if (qcom_wifi_mgmt_api == NULL || qcom_wifi_mgmt_api->unit_test == NULL) {
		return -ENOTSUP;
	}

	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}

	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return qcom_wifi_mgmt_api->unit_test(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_UNIT_TEST, wifi_unit_test);

static int wifi_set_rts_cts(uint64_t mgmt_request, struct net_if *iface,
			    void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_rts_cts_params *params = data;

	if (api == NULL || api->set_rts_cts == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_rts_cts(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RTS_CTS, wifi_set_rts_cts);

static int wifi_set_rts_rate(uint64_t mgmt_request, struct net_if *iface,
			     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_rts_rate_params *params = data;

	if (api == NULL || api->set_rts_rate == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_rts_rate(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RTS_RATE, wifi_set_rts_rate);

static int wifi_set_edca_param_cfg(uint64_t mgmt_request, struct net_if *iface,
				   void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_edca_param_cfg_params *params = data;

	if (api == NULL || api->set_edca_param_cfg == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_edca_param_cfg(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG, wifi_set_edca_param_cfg);

static int wifi_set_threshold(uint64_t mgmt_request, struct net_if *iface,
			      void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_threshold_params *params = data;

	if (api == NULL || api->set_threshold == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_threshold(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_THRESHOLD, wifi_set_threshold);

static int wifi_set_ba_win_timing(uint64_t mgmt_request, struct net_if *iface,
				void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_ba_win_timing_params *params = data;

	if (api == NULL || api->set_ba_win_timing == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_ba_win_timing(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING, wifi_set_ba_win_timing);

static int wifi_set_slot_time(uint64_t mgmt_request, struct net_if *iface,
			      void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_slot_time_params *params = data;

	if (api == NULL || api->set_slot_time == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_slot_time(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME, wifi_set_slot_time);

static int wifi_set_aggregation(uint64_t mgmt_request, struct net_if *iface,
				void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_aggregation_params *params = data;

	if (api == NULL || api->set_aggregation == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_aggregation(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_AGGREGATION, wifi_set_aggregation);

static int wifi_set_amsdu_rx(uint64_t mgmt_request, struct net_if *iface,
			     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_amsdu_rx_params *params = data;

	if (api == NULL || api->set_amsdu_rx == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_amsdu_rx(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_AMSDU_RX, wifi_set_amsdu_rx);

static int wifi_get_rts_cts(uint64_t mgmt_request, struct net_if *iface,
			    void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_rts_cts_params *params = data;

	if (api == NULL || api->get_rts_cts == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_rts_cts(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RTS_CTS, wifi_get_rts_cts);

static int wifi_get_rts_rate(uint64_t mgmt_request, struct net_if *iface,
			     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_rts_rate_params *params = data;

	if (api == NULL || api->get_rts_rate == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_rts_rate(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RTS_RATE, wifi_get_rts_rate);

static int wifi_get_edca_param_cfg(uint64_t mgmt_request, struct net_if *iface,
				   void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_edca_param_cfg_params *params = data;

	if (api == NULL || api->get_edca_param_cfg == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_edca_param_cfg(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG, wifi_get_edca_param_cfg);

static int wifi_get_threshold(uint64_t mgmt_request, struct net_if *iface,
			      void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_threshold_params *params = data;

	if (api == NULL || api->get_threshold == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_threshold(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_THRESHOLD, wifi_get_threshold);

static int wifi_get_ba_win_timing(uint64_t mgmt_request, struct net_if *iface,
				void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_ba_win_timing_params *params = data;

	if (api == NULL || api->get_ba_win_timing == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_ba_win_timing(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING, wifi_get_ba_win_timing);

static int wifi_get_slot_time(uint64_t mgmt_request, struct net_if *iface,
			      void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_slot_time_params *params = data;

	if (api == NULL || api->get_slot_time == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_slot_time(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME, wifi_get_slot_time);

static int wifi_set_bmiss_threshold(uint64_t mgmt_request, struct net_if *iface,
				    void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_bmiss_threshold_params *params = data;

	if (api == NULL || api->set_bmiss_threshold == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_bmiss_threshold(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD, wifi_set_bmiss_threshold);

static int wifi_get_bmiss_threshold(uint64_t mgmt_request, struct net_if *iface,
				    void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_bmiss_threshold_params *params = data;

	if (api == NULL || api->get_bmiss_threshold == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_bmiss_threshold(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD, wifi_get_bmiss_threshold);

static int wifi_set_phy_mode(uint64_t mgmt_request, struct net_if *iface,
			     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_phy_mode_params *params = data;

	if (api == NULL || api->set_phy_mode == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_phy_mode(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_PHY_MODE, wifi_set_phy_mode);

static int wifi_get_phy_mode(uint64_t mgmt_request, struct net_if *iface,
			     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_phy_mode_params *params = data;

	if (api == NULL || api->get_phy_mode == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_phy_mode(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, wifi_get_phy_mode);

static int wifi_get_boot_reason(uint64_t mgmt_request, struct net_if *iface,
			       void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_boot_reason_params *params = data;

	if (api == NULL || api->get_boot_reason == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_boot_reason(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_BOOT_REASON, wifi_get_boot_reason);

static int wifi_get_power_mode(uint64_t mgmt_request, struct net_if *iface,
			       void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_power_mode_params *params = data;

	if (api == NULL || api->get_power_mode == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_power_mode(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_POWER_MODE, wifi_get_power_mode);

static int wifi_get_mac_address(uint64_t mgmt_request, struct net_if *iface,
				void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_mac_address_params *params = data;

	if (api == NULL || api->get_mac_address == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_mac_address(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS, wifi_get_mac_address);

static int wifi_get_concurrency_mode(uint64_t mgmt_request, struct net_if *iface,
				     void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_concurrency_mode_params *params = data;

	if (api == NULL || api->get_concurrency_mode == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_concurrency_mode(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_CONCURRENCY_MODE, wifi_get_concurrency_mode);

static int wifi_get_operation_mode(uint64_t mgmt_request, struct net_if *iface,
				   void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_get_operation_mode_params *params = data;

	if (api == NULL || api->get_operation_mode == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_operation_mode(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_OPERATION_MODE, wifi_get_operation_mode);

static int wifi_set_rate(uint64_t mgmt_request, struct net_if *iface,
			 void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_rate_params *params = data;

	if (api == NULL || api->set_rate == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->set_rate(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_SET_RATE, wifi_set_rate);

static int wifi_get_rate(uint64_t mgmt_request, struct net_if *iface,
			 void *data, size_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct qcom_wifi_mgmt_ops *api = get_qcom_wifi_api(iface);
	struct qcom_wifi_set_rate_params *params = data;

	if (api == NULL || api->get_rate == NULL) {
		return -ENOTSUP;
	}
	if (!net_if_is_admin_up(iface)) {
		return -ENETDOWN;
	}
	if (!data || len != sizeof(*params)) {
		return -EINVAL;
	}

	return api->get_rate(dev, params);
}
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_WIFI_QCOM_GET_RATE, wifi_get_rate);
