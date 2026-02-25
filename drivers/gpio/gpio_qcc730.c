/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_gpio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/gpio/qcom-qcc730-gpio.h>
#include <zephyr/devicetree.h>
#include <soc.h>
#include <zephyr/pm/device.h>

#include <zephyr/drivers/gpio/gpio_utils.h>
#include <nt_gpio_api.h>

#include "nt_gpio_api.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gpio_qcc730, CONFIG_GPIO_LOG_LEVEL);

#define GPIOA_DEV DEVICE_DT_GET(DT_NODELABEL(gpioa))

#ifdef CONFIG_GPIO_QCC730_INTERRUPT
static bool interrupts_enabled;
#endif

struct gpio_qcc730_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	/* added for future development of IRQ */
	sys_slist_t callbacks;
#ifdef CONFIG_PM_DEVICE
	bool gpio_initialized;
#endif
};

struct gpio_qcc730_cfg {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	GPIO_BASE_gpio_Type *regs;
	PMU_BASE_pmu_Type *pmu;
	struct reset_dt_spec reset;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

static int gpio_qcc730_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	PMU_BASE_pmu_Type *pmu = config->pmu;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	if (((flags & GPIO_INPUT) && (flags & GPIO_OUTPUT)) || (flags == GPIO_DISCONNECTED)) {
		/* Pin is always either input or output */
		LOG_ERR("Pin %d: invalid configuration. Pin must be either input or output.", pin);
		return -ENOTSUP;
	}

	if (flags & GPIO_OUTPUT) {
		/* Output is incompatible with pull */
		if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
			LOG_ERR("Pin %d: invalid configuration. Output pin can't enable pull "
				"resistor.",
				pin);
			return -ENOTSUP;
		}

		/* Set output direction */
		regs->GPIO_GPIO_SWPORTA_DDR.reg |= BIT(pin);

		if (flags & GPIO_OUTPUT_INIT_LOW) {
			regs->GPIO_GPIO_SWPORTA_DR.reg &= ~BIT(pin);
		} else if (flags & GPIO_OUTPUT_INIT_HIGH) {
			regs->GPIO_GPIO_SWPORTA_DR.reg |= BIT(pin);
		}

		/* Set drive strength */
		if (flags & QCC730_GPIO_DRIVE_HIGH_E) {
			pmu->PMU_CFG_IOPAD_DS.reg |= BIT(pin);
		} else {
			pmu->PMU_CFG_IOPAD_DS.reg &= ~BIT(pin);
		}
	} else if (flags & GPIO_INPUT) {
		/* Cannot enable pull-up and pull-down at the same time */
		if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) ==
			     (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
			return -ENOTSUP;
		}

		/* Set input direction */
		regs->GPIO_GPIO_SWPORTA_DDR.reg &= ~BIT(pin);


		if (flags & GPIO_PULL_DOWN) {
			pmu->PMU_CFG_IOPAD_PU.reg &= ~BIT(pin);
			pmu->PMU_CFG_IOPAD_PD.reg |= BIT(pin);
		} else if (flags & GPIO_PULL_UP) {
			pmu->PMU_CFG_IOPAD_PD.reg &= ~BIT(pin);
			pmu->PMU_CFG_IOPAD_PU.reg |= BIT(pin);
		} else {
			pmu->PMU_CFG_IOPAD_PU.reg &= ~BIT(pin);
			pmu->PMU_CFG_IOPAD_PD.reg &= ~BIT(pin);
		}
		/* Reset the drive strength bit */
		pmu->PMU_CFG_IOPAD_DS.reg &= ~BIT(pin);
	}

	return 0;
}

static int gpio_qcc730_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	*value = regs->GPIO_GPIO_EXT_PORTA.reg;

	return 0;
}

static int gpio_qcc730_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					   gpio_port_value_t value)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	uint32_t out = regs->GPIO_GPIO_SWPORTA_DR.reg;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	regs->GPIO_GPIO_SWPORTA_DR.reg = (out & ~mask) | (value & mask);

	return 0;
}

static int gpio_qcc730_port_set_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	regs->GPIO_GPIO_SWPORTA_DR.reg |= mask;

	return 0;
}

static int gpio_qcc730_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	regs->GPIO_GPIO_SWPORTA_DR.reg &= ~mask;

	return 0;
}

static int gpio_qcc730_port_toggle_bits(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	uint32_t out = regs->GPIO_GPIO_SWPORTA_DR.reg;

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	regs->GPIO_GPIO_SWPORTA_DR.reg = out ^ mask;

	return 0;
}

#ifdef CONFIG_GPIO_QCC730_INTERRUPT

int gpio_qcc730_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	uint32_t value = 0;
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

#if defined(CONFIG_PM_DEVICE) && defined(CONFIG_GPIO_QCC730_INTERRUPT)
	struct gpio_qcc730_data *const data = dev->data;

	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	if ((mode == GPIO_INT_MODE_EDGE) && (trig == GPIO_INT_TRIG_BOTH)) {
		return -ENOTSUP;
	}

	/* system clock enable */
	regs->GPIO_GPIO_LS_SYNC.reg = GPIO_BASE_gpio_GPIO_GPIO_LS_SYNC_VALUE_Msk;

	if (GPIO_INT_DISABLE == mode) {
		/* disable the interrupt */
		regs->GPIO_GPIO_INTEN.reg &= ~BIT(pin);
		return 0;
	}

	/* reading  interrupt type register */
	value = regs->GPIO_GPIO_INTTYPE_LEVEL.reg;

	if (GPIO_INT_MODE_LEVEL == mode) {
		value &= (~BIT(pin));
	} else if (GPIO_INT_MODE_EDGE == mode) {
		value |= BIT(pin);
	}

	/* clear/set the interrupt type bit */
	regs->GPIO_GPIO_INTTYPE_LEVEL.reg = value;

	/* read the polarity register */
	value = regs->GPIO_GPIO_INR_POLARITY.reg;

	if (GPIO_INT_TRIG_HIGH == trig) {
		value |= BIT(pin);
	} else if (GPIO_INT_TRIG_LOW == trig) {
		value &= (~BIT(pin));
	} else {
		LOG_ERR("GPIO doesn't support both edge trigger");
		return -ENOTSUP;
	}

	/* clear/set value the value of polarity bit */
	regs->GPIO_GPIO_INR_POLARITY.reg = value;
	/* enable the interrupt for GPIO */
	regs->GPIO_GPIO_INTEN.reg |= BIT(pin);

	return 0;
}

int gpio_qcc730_manage_callback(const struct device *dev, struct gpio_callback *callback, bool set)
{
	struct gpio_qcc730_data *data = dev->data;

#if defined(CONFIG_PM_DEVICE) && defined(CONFIG_GPIO_QCC730_INTERRUPT)
	if (!data->gpio_initialized) {
		LOG_ERR("GPIO is suspended/not initialized");
		return -EBUSY;
	}
#endif

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void gpio_qcc730_isr(const struct device *dev)
{
	struct gpio_qcc730_data *const data = (struct gpio_qcc730_data *)dev->data;
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	uint32_t pins_mask = 0;

	/* Read the interrupt status register in order to determine which pin/pins
	 * triggered it */
	pins_mask = regs->GPIO_GPIO_INTSTATUS.reg;

	/* clear edge type interrupts */
	regs->GPIO_GPIO_PORTA_EOI.reg = pins_mask;

	gpio_fire_callbacks(&data->callbacks, dev, pins_mask);
}
#endif

static int gpio_qcc730_init(const struct device *dev)
{
	int ret = 0;
	const struct gpio_qcc730_cfg *config = dev->config;

	/* GPIO root clock enable */
	if (config->clock_dev) {
		if (!device_is_ready(config->clock_dev)) {
			return -ENODEV;
		}
		ret = clock_control_on(config->clock_dev, config->clock_subsys);
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}
	}

	/* reset the configuration of gpios */
	ret = reset_line_toggle_dt(&config->reset);

	if (ret < 0) {
		LOG_ERR("GPIO reset line toggle failed: %d", ret);
		return -EIO;
	}

#ifdef CONFIG_GPIO_QCC730_INTERRUPT
	/* There is one IRQ line and it is supported only for GPIOA. */
	if (!interrupts_enabled && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpioa))) {
		IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), gpio_qcc730_isr,
			    DEVICE_DT_INST_GET(0), 0);
		irq_enable(DT_INST_IRQN(0));

		interrupts_enabled = true;
	}
#endif

#ifdef CONFIG_PM_DEVICE
	struct gpio_qcc730_data *const data = (struct gpio_qcc730_data *)dev->data;
	/* Mark the gpio as initialized */
	data->gpio_initialized = true;
#endif

	return 0;
}

#ifdef CONFIG_PM_DEVICE
#if 0
static int gpio_730_restore(const struct device *dev)
{
	int ret = 0;
	const struct gpio_qcc730_cfg *config = dev->config;

	/* GPIO root clock enable */
	if (config->clock_dev) {
		if (!device_is_ready(config->clock_dev)) {
			return -ENODEV;
		}
		ret = clock_control_on(config->clock_dev, config->clock_subsys);
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}
	}

	/* reset the gpio config 
	TODO: need handle the wfi failure case and call in ram_minimum_code
	*/
    nt_gpio_preset();

	return 0;
}

static int __maybe_unused gpio_qcc730_deinit(const struct device *dev)
{
	int ret = 0;
	const struct gpio_qcc730_cfg *config = dev->config;
	struct gpio_qcc730_data *const data = (struct gpio_qcc730_data *)dev->data;

	/* GPIO root clock disable */
	if (config->clock_dev) {
		ret = clock_control_off(config->clock_dev, config->clock_subsys);
		if (ret < 0) {
			return ret;
		}
	}

	/* reset the configuration of gpios */
	ret = reset_line_toggle_dt(&config->reset);
	if (ret < 0) {
		LOG_ERR("GPIO reset line toggle failed: %d", ret);
		return -EIO;
	}

#if defined(CONFIG_PM_DEVICE) && defined(CONFIG_GPIO_QCC730_INTERRUPT)
	/* There is one IRQ line and it is supported only for GPIOA. */
	if (interrupts_enabled && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpioa))) {
		irq_disable(DT_INST_IRQN(0));
		interrupts_enabled = false;
	}
#endif
	/* Mark the gpio as not initialized */
	data->gpio_initialized = false;

	return 0;
}
#endif

static int gpio_qcc730_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		/*ret = gpio_qcc730_deinit(dev); */
		break;
	case PM_DEVICE_ACTION_RESUME:
		/* ret = gpio_qcc730_init(dev);
		ret = gpio_730_restore(dev);*/

		break;
	default:
		return -ENOTSUP;
	}
	return ret;
}
#endif /* CONFIG_PM_DEVICE */

static DEVICE_API(gpio, gpio_qcc730_api) = {
	.pin_configure = gpio_qcc730_pin_configure,
	.port_get_raw = gpio_qcc730_port_get_raw,
	.port_set_masked_raw = gpio_qcc730_port_set_masked_raw,
	.port_set_bits_raw = gpio_qcc730_port_set_bits_raw,
	.port_clear_bits_raw = gpio_qcc730_port_clear_bits_raw,
	.port_toggle_bits = gpio_qcc730_port_toggle_bits,
#ifdef CONFIG_GPIO_QCC730_INTERRUPT
	.pin_interrupt_configure = gpio_qcc730_pin_interrupt_configure,
	.manage_callback = gpio_qcc730_manage_callback,
#endif
};

#define GPIO_QCC730_DEVICE(n)                                                                      \
	static const struct gpio_qcc730_cfg gpio_qcc730_cfg_##n = {                                \
		.common =                                                                          \
			{                                                                          \
				.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n),               \
			},                                                                         \
		.regs = (GPIO_BASE_gpio_Type *)DT_INST_REG_ADDR(n),                                \
		.pmu = (PMU_BASE_pmu_Type *)DT_REG_ADDR(DT_NODELABEL(pmu)),                        \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, id),                \
	};                                                                                         \
                                                                                                   \
	static struct gpio_qcc730_data gpio_qcc730_data_##n;                                       \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, gpio_qcc730_pm_action);                                        \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_qcc730_init, PM_DEVICE_DT_INST_GET(n),                       \
			      &gpio_qcc730_data_##n,                                               \
			      &gpio_qcc730_cfg_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,       \
			      &gpio_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_QCC730_DEVICE)
