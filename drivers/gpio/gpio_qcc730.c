/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_qcc730_gpio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dt-bindings/gpio/qcom-qcc730-gpio.h>
#include <soc.h>

#include <zephyr/drivers/gpio/gpio_utils.h>

#define GPIOA_DEV DEVICE_DT_GET(DT_NODELABEL(gpioa))

#ifdef CONFIG_GPIO_QCC730_INTERRUPT
static bool interrupts_enabled;
#endif

struct gpio_qcc730_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	/* added for future development of IRQ */
	sys_slist_t callbacks;
};

struct gpio_qcc730_cfg {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	GPIO_BASE_gpio_Type *regs;
	PMU_BASE_pmu_Type *pmu;
};

static int gpio_qcc730_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	PMU_BASE_pmu_Type *pmu = config->pmu;

	if (((flags & GPIO_INPUT) && (flags & GPIO_OUTPUT)) || (flags == GPIO_DISCONNECTED)) {
		/* Pin is always either input or output */
		return -ENOTSUP;
	}

	if (flags & GPIO_OUTPUT) {
		/* Output is incompatible with pull */
		if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
			return -ENOTSUP;
		}

		/* Set output direction */
		regs->GPIO_GPIO_SWPORTA_DDR.reg |= BIT(pin);

		if (flags & GPIO_OUTPUT_INIT_LOW) {
			regs->GPIO_GPIO_SWPORTA_DR.reg &= ~BIT(pin);
		} else if (flags & GPIO_OUTPUT_INIT_HIGH) {
			regs->GPIO_GPIO_SWPORTA_DR.reg |= BIT(pin);
		}

	} else if (flags & GPIO_INPUT) {
		/* Set input direction */
		regs->GPIO_GPIO_SWPORTA_DDR.reg &= ~BIT(pin);

		/* Cannot enable pull-up and pull-down at the same time */
		if ((flags & GPIO_PULL_UP & GPIO_PULL_DOWN) != 0) {
			return -ENOTSUP;
		}

		uint32_t pull_up = pmu->PMU_CFG_IOPAD_PU.reg;
		uint32_t pull_down = pmu->PMU_CFG_IOPAD_PD.reg;
		pull_up &= ~BIT(pin);
		pull_down &= ~BIT(pin);

		if (flags & GPIO_PULL_DOWN) {
			pull_down |= BIT(pin);
		} else if (flags & GPIO_PULL_UP) {
			pull_up |= BIT(pin);
		}
		pmu->PMU_CFG_IOPAD_PU.reg = pull_up;
		pmu->PMU_CFG_IOPAD_PD.reg = pull_down;
	}

	if (flags & QCC730_GPIO_DRIVE_HIGH_E) {
		pmu->PMU_CFG_IOPAD_DS.reg |= BIT(pin);
	} else {
		pmu->PMU_CFG_IOPAD_DS.reg &= ~BIT(pin);
	}
	return 0;
}

static int gpio_qcc730_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

	*value = regs->GPIO_GPIO_EXT_PORTA.reg;

	return 0;
}

static int gpio_qcc730_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					   gpio_port_value_t value)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	uint32_t out = regs->GPIO_GPIO_SWPORTA_DR.reg;

	regs->GPIO_GPIO_SWPORTA_DR.reg = (out & ~mask) | (value & mask);

	return 0;
}

static int gpio_qcc730_port_set_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

	regs->GPIO_GPIO_SWPORTA_DR.reg |= mask;

	return 0;
}

static int gpio_qcc730_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;

	regs->GPIO_GPIO_SWPORTA_DR.reg &= ~mask;

	return 0;
}

static int gpio_qcc730_port_toggle_bits(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_qcc730_cfg *config = dev->config;
	GPIO_BASE_gpio_Type *regs = config->regs;
	uint32_t out = regs->GPIO_GPIO_SWPORTA_DR.reg;

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

	if ((mode == GPIO_INT_MODE_EDGE) && (trig == GPIO_INT_TRIG_BOTH)) {
		return -ENOTSUP;
	}

	/* Configure the porta pins */

	if (GPIOA_DEV == dev) {
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
			return -ENOTSUP;
		}

		/* clear/set value the value of polarity bit */
		regs->GPIO_GPIO_INR_POLARITY.reg = value;
		/* enable the interrupt for GPIO */
		regs->GPIO_GPIO_INTEN.reg |= BIT(pin);
	} else {
		return -ENOTSUP;
	}

	return 0;
}

int gpio_qcc730_manage_callback(const struct device *dev, struct gpio_callback *callback, bool set)
{
	struct gpio_qcc730_data *data = dev->data;

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
	const struct gpio_qcc730_cfg *config = dev->config;
	PMU_BASE_pmu_Type *pmu = config->pmu;

	/* GPIO root clock enable */
	pmu->PMU_ROOT_CLK_ENABLE.bit.GPIO_ROOT_CLK_ENABLE = 1;

	/* reset the configuration of gpios */
	pmu->PMU_SOFT_RESET.bit.GPIO_SOFT_RESET = 1;
	pmu->PMU_SOFT_RESET.bit.GPIO_SOFT_RESET = 0;

#ifdef CONFIG_GPIO_QCC730_INTERRUPT
	/* There is one IRQ line and it is supported only for GPIOA. */
	if (!interrupts_enabled && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpioa))) {
		IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), gpio_qcc730_isr,
			    DEVICE_DT_INST_GET(0), 0);
		irq_enable(DT_INST_IRQN(0));

		interrupts_enabled = true;
	}
#endif
	return 0;
}

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
	};                                                                                         \
                                                                                                   \
	static struct gpio_qcc730_data gpio_qcc730_data_##n;                                       \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_qcc730_init, NULL, &gpio_qcc730_data_##n,                    \
			      &gpio_qcc730_cfg_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,       \
			      &gpio_qcc730_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_QCC730_DEVICE)
