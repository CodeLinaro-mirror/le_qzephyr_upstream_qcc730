#include <zephyr/logging/log.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include "inc/qcom_wifi_mgmt.h"

const struct qcom_wifi_mgmt_ops *const get_qcom_wifi_api(struct net_if *iface);

static int wifi_set_tx_power(uint32_t mgmt_request, struct net_if *iface,
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

static int wifi_get_tx_power(uint32_t mgmt_request, struct net_if *iface,
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

static int wifi_unit_test(uint32_t mgmt_request, struct net_if *iface,
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
