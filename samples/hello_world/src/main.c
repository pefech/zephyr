/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/i2c.h>

#define I2C_NODE 	DT_NODELABEL(i2c2)
#if !DT_NODE_HAS_STATUS_OKAY(I2C_NODE)
#error "Unsupported board: i2c2 devicetree alias is not defined"
#endif

static const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);

#define SW0_NODE	DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios,
							      {0});
static struct gpio_callback button_cb_data;

K_SEM_DEFINE(button_sem, 0, 1);

const uint8_t color_red 	= 	0b00000001;
const uint8_t color_green 	= 	0b00000100;
const uint8_t color_blue 	= 	0b00010000;
const uint8_t led_register 	= 	0x05;


const static uint8_t color_list[] =
{
	color_red,
	color_green,
	color_blue,
	(color_red | color_green),
	(color_red | color_blue),
	(color_green | color_blue),
	(color_red | color_green | color_blue)
};

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	printk("Button pressed at %" PRIu32 "\n", k_cycle_get_32());

	k_sem_give(&button_sem);
}

int main(void)
{
	int ret;

	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(i2c_dev)) {
		printk("Error: I2C device %s is not ready\n", i2c_dev->name);
		return 0;
	}

	if (!gpio_is_ready_dt(&button)) {
		printk("Error: button device %s is not ready\n",
		       button.port->name);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, button.port->name, button.pin);
		return 0;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	while (1) {
		printk("Waiting for button press\n");
		k_sem_take(&button_sem, K_FOREVER);
		printk("Got semaphore\n");

		for (int i = 0; i < 7; i++)
		{
			i2c_reg_write_byte(i2c_dev, 0x62, led_register, color_list[i]);
			k_sleep(K_MSEC(500));
		}

		/* Turn off all LEDs */
		i2c_reg_write_byte(i2c_dev, 0x62, led_register, 0x00);
	}

	return 0;
}
