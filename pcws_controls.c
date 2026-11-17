// SPDX-License-Identifier: GPL-2.0-only
/* Explicitly mapped GPIO controls for WS310/WS610. No EEPROM writes. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "pcws_internal.h"
#include "pcws_recovered.h"

#define CONTROL_MAX 23
static char *serial_power_gpio_chips[6];
static unsigned int serial_power_gpio_offsets[6], power_chips_count, power_offsets_count;
module_param_array(serial_power_gpio_chips, charp, &power_chips_count, 0444);
module_param_array(serial_power_gpio_offsets, uint, &power_offsets_count, 0444);
MODULE_PARM_DESC(serial_power_gpio_chips, "Verified serial power GPIO chip labels; WS610: 5/9/12V per port; WS310D+: enable/select9/select12");
MODULE_PARM_DESC(serial_power_gpio_offsets, "Chip-local serial power GPIO offsets, in chip-label order");
static char *power_led_gpio_chip;
static unsigned int power_led_gpio_offset = ~0U;
static bool power_led_active_low;
module_param(power_led_gpio_chip, charp, 0444);
module_param(power_led_gpio_offset, uint, 0444);
module_param(power_led_active_low, bool, 0444);
MODULE_PARM_DESC(power_led_gpio_chip, "Verified power LED GPIO chip label (unconfigured by default)");
MODULE_PARM_DESC(power_led_gpio_offset, "Verified chip-local power LED GPIO offset");
MODULE_PARM_DESC(power_led_active_low, "Invert the configured power LED GPIO");
static char *user_gpio_chips[16];
static unsigned int user_gpio_offsets[16], user_chips_count, user_offsets_count;
static unsigned int user_gpio_output_mask;
module_param_array(user_gpio_chips, charp, &user_chips_count, 0444);
module_param_array(user_gpio_offsets, uint, &user_offsets_count, 0444);
module_param(user_gpio_output_mask, uint, 0444);
MODULE_PARM_DESC(user_gpio_chips, "Up to 16 explicitly mapped user GPIO chip labels");
MODULE_PARM_DESC(user_gpio_offsets, "Chip-local offsets matching user_gpio_chips");
MODULE_PARM_DESC(user_gpio_output_mask, "User GPIO direction bits: 1=output initialized low, 0=input");

static struct gpiod_lookup_table *controls_lookup;
static struct gpio_desc *controls[CONTROL_MAX];
static unsigned int controls_model, power_ports, blink_ms;
static DEFINE_MUTEX(controls_lock);
static void led_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(led_work, led_work_fn);

bool pcws_controls_available(void)
{
	return power_ports || controls[6] || user_chips_count;
}

static int control_set(struct gpio_desc *desc, int value)
{
	return __builtin_choose_expr(
		__builtin_types_compatible_p(typeof(gpiod_set_value_cansleep(desc, value)), void),
		(gpiod_set_value_cansleep(desc, value), 0),
		gpiod_set_value_cansleep(desc, value));
}

static void led_work_fn(struct work_struct *work)
{
	int value, ret;

	mutex_lock(&controls_lock);
	if (blink_ms && controls[6]) {
		value = gpiod_get_value_cansleep(controls[6]);
		ret = value < 0 ? value : control_set(controls[6], !value);
		if (ret)
			blink_ms = 0;
		else
			schedule_delayed_work(&led_work, msecs_to_jiffies(blink_ms));
	}
	mutex_unlock(&controls_lock);
}

int pcws_led_get(void)
{
	return controls[6] ? gpiod_get_value_cansleep(controls[6]) : -EOPNOTSUPP;
}

int pcws_led_set(unsigned int value)
{
	int ret;

	if (!controls[6])
		return -EOPNOTSUPP;
	if (value > 1)
		return -EINVAL;
	mutex_lock(&controls_lock);
	blink_ms = 0;
	mutex_unlock(&controls_lock);
	cancel_delayed_work_sync(&led_work);
	mutex_lock(&controls_lock);
	ret = control_set(controls[6], value);
	mutex_unlock(&controls_lock);
	return ret;
}

int pcws_led_blink_get(void)
{
	return controls[6] ? READ_ONCE(blink_ms) : -EOPNOTSUPP;
}

int pcws_led_blink_set(unsigned int value)
{
	if (!controls[6])
		return -EOPNOTSUPP;
	if (value && (value < 50 || value > 60000))
		return -EINVAL;
	mutex_lock(&controls_lock);
	blink_ms = 0;
	mutex_unlock(&controls_lock);
	cancel_delayed_work_sync(&led_work);
	mutex_lock(&controls_lock);
	blink_ms = value;
	if (value)
		schedule_delayed_work(&led_work, msecs_to_jiffies(value));
	mutex_unlock(&controls_lock);
	return 0;
}

int pcws_user_gpio_count(void)
{
	return user_chips_count;
}

int pcws_user_gpio_get(unsigned int index)
{
	if (index >= user_chips_count || !controls[7 + index])
		return -EINVAL;
	return gpiod_get_value_cansleep(controls[7 + index]);
}

int pcws_user_gpio_set(unsigned int index, unsigned int value)
{
	if (index >= user_chips_count || value > 1 || !controls[7 + index])
		return -EINVAL;
	if (!(user_gpio_output_mask & BIT(index)))
		return -EPERM;
	return control_set(controls[7 + index], value);
}

int pcws_power_get(unsigned int port)
{
	unsigned int base, i;
	int v[3], count = 0, result = 0;
	static const int volts[] = { 5, 9, 12 };

	if (!power_ports)
		return -EOPNOTSUPP;
	if (!port || port > power_ports)
		return -EINVAL;
	base = (port - 1) * 3;
	mutex_lock(&controls_lock);
	for (i = 0; i < 3; i++) {
		v[i] = gpiod_get_value_cansleep(controls[base + i]);
		if (v[i] < 0) {
			result = v[i];
			goto out;
		}
		if (v[i]) {
			count++;
			result = volts[i];
		}
	}
	if (controls_model == PCWS_MODEL_YARROWSBURG)
		result = !v[0] ? 0 : !v[1] && v[2] ? -EIO : v[2] ? 12 : v[1] ? 9 : 5;
	else if (count > 1)
		result = -EIO;
out:
	mutex_unlock(&controls_lock);
	return result;
}

int pcws_power_set(unsigned int port, int voltage)
{
	unsigned int i, base;
	int ret = 0, error;

	if (!power_ports)
		return -EOPNOTSUPP;
	if (!port || port > power_ports ||
	    (voltage != 0 && voltage != 5 && voltage != 9 && voltage != 12))
		return -EINVAL;
	base = (port - 1) * 3;
	mutex_lock(&controls_lock);
	/* Disable before changing selection. Abort on ANY failed disable. */
	for (i = 0; i < (controls_model == PCWS_MODEL_YARROWSBURG ? 1 : 3); i++) {
		error = control_set(controls[base + i], 0);
		if (error && !ret)
			ret = error;
	}
	if (ret)
		goto out;
	msleep(5);
	if (controls_model == PCWS_MODEL_YARROWSBURG) {
		ret = control_set(controls[1], voltage == 9 || voltage == 12);
		if (!ret)
			ret = control_set(controls[2], voltage == 12);
		if (!ret)
			ret = control_set(controls[0], voltage != 0);
	} else if (voltage) {
		i = voltage == 5 ? 0 : voltage == 9 ? 1 : 2;
		ret = control_set(controls[base + i], 1);
	}
out:
	mutex_unlock(&controls_lock);
	return ret;
}

void pcws_controls_exit(void)
{
	unsigned int i;

	mutex_lock(&controls_lock);
	blink_ms = 0;
	mutex_unlock(&controls_lock);
	cancel_delayed_work_sync(&led_work);
	for (i = 0; i < ARRAY_SIZE(controls); i++) {
		if (controls[i]) {
			if (i < 7 || (user_gpio_output_mask & BIT(i - 7)))
				control_set(controls[i], 0);
			gpiod_put(controls[i]);
			controls[i] = NULL;
		}
	}
	if (controls_lookup) {
		gpiod_remove_lookup_table(controls_lookup);
		kfree(controls_lookup);
		controls_lookup = NULL;
	}
	power_ports = 0;
}

int pcws_controls_init(unsigned int model, bool test_mode)
{
	const char *chips[CONTROL_MAX] = { NULL };
	unsigned int offsets[CONTROL_MAX] = { 0 };
	unsigned int i, j, entries = 0;
	int ret;

	if (power_chips_count != power_offsets_count || user_chips_count != user_offsets_count ||
	    user_gpio_output_mask >> user_chips_count)
		return -EINVAL;
	if (!power_chips_count && !power_led_gpio_chip && !user_chips_count)
		return 0;
	if (test_mode || (model != PCWS_MODEL_WOODSTOCK && model != PCWS_MODEL_YARROWSBURG))
		return -EOPNOTSUPP;
	controls_model = model;
	if (power_chips_count) {
		if (power_chips_count != (model == PCWS_MODEL_WOODSTOCK ? 6 : 3))
			return -EINVAL;
		if (model == PCWS_MODEL_YARROWSBURG) {
			ret = pcws_board_revision();
			if (ret < 0)
				return ret;
			if (ret < 'D')
				return -EOPNOTSUPP;
		}
		power_ports = model == PCWS_MODEL_WOODSTOCK ? 2 : 1;
	}
	for (i = 0; i < power_chips_count; i++) {
		chips[i] = serial_power_gpio_chips[i];
		offsets[i] = serial_power_gpio_offsets[i];
	}
	chips[6] = power_led_gpio_chip;
	offsets[6] = power_led_gpio_offset;
	for (i = 0; i < user_chips_count; i++) {
		chips[7 + i] = user_gpio_chips[i];
		offsets[7 + i] = user_gpio_offsets[i];
	}
	for (i = 0; i < CONTROL_MAX; i++) {
		if (!chips[i])
			continue;
		if (!*chips[i] || offsets[i] > U16_MAX)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (chips[j] && !strcmp(chips[i], chips[j]) && offsets[i] == offsets[j])
				return -EINVAL;
		entries++;
	}
	controls_lookup = kzalloc(struct_size(controls_lookup, table, entries + 1), GFP_KERNEL);
	if (!controls_lookup)
		return -ENOMEM;
	entries = 0;
	for (i = 0; i < CONTROL_MAX; i++) {
		struct gpiod_lookup *entry;

		if (!chips[i])
			continue;
		entry = &controls_lookup->table[entries++];
		entry->key = chips[i];
		entry->chip_hwnum = offsets[i];
		entry->con_id = "pcws-controls";
		entry->idx = i;
		entry->flags = i == 6 && power_led_active_low ? GPIO_ACTIVE_LOW : GPIO_ACTIVE_HIGH;
	}
	gpiod_add_lookup_table(controls_lookup);
	for (i = 0; i < CONTROL_MAX; i++) {
		struct gpio_desc *desc;
		bool output = i < 7 || (user_gpio_output_mask & BIT(i - 7));

		if (!chips[i])
			continue;
		desc = gpiod_get_index(NULL, "pcws-controls", i, output ? GPIOD_OUT_LOW : GPIOD_IN);
		if (IS_ERR(desc)) {
			ret = PTR_ERR(desc);
			pcws_controls_exit();
			return ret;
		}
		controls[i] = desc;
	}
	return 0;
}
