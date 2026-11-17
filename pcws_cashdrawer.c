// SPDX-License-Identifier: GPL-2.0-only
/* Descriptor-based port of the four original GPIO cash-drawer backends.
 * Chip labels and chip-local line offsets must be verified on the workstation.
 * Original firmware pad numbers are NOT Linux gpiochip offsets.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "pcws_internal.h"
#include "pcws_recovered.h"

static bool enable_cashdrawer;
module_param(enable_cashdrawer, bool, 0444);
MODULE_PARM_DESC(enable_cashdrawer, "Request four explicitly mapped cash-drawer GPIO lines");
static char *gpio_chips[4];
static unsigned int gpio_chip_count;
module_param_array(gpio_chips, charp, &gpio_chip_count, 0444);
MODULE_PARM_DESC(gpio_chips, "GPIO chip labels in order: open A, open B, sense A, sense B");
static unsigned int gpio_offsets[4];
static unsigned int gpio_offset_count;
module_param_array(gpio_offsets, uint, &gpio_offset_count, 0444);
MODULE_PARM_DESC(gpio_offsets, "Four verified chip-local line offsets, in gpio_chips order");

static struct gpiod_lookup_table *pcws_gpio_lookup;
static struct gpio_desc *pcws_gpios[4];
static DEFINE_MUTEX(pcws_cashdrawer_mutex);
static bool pcws_gpio_ready;

/* New kernels return GPIO write errors; older/vendor kernels return void.
 * Select by the actual declaration rather than a kernel version number.
 */
static int pcws_gpio_set(struct gpio_desc *desc, int value)
{
	return __builtin_choose_expr(
		__builtin_types_compatible_p(typeof(gpiod_set_value_cansleep(desc, value)), void),
		(gpiod_set_value_cansleep(desc, value), 0),
		gpiod_set_value_cansleep(desc, value));
}

bool pcws_cashdrawer_available(void)
{
	return pcws_gpio_ready;
}

void pcws_cashdrawer_exit(void)
{
	unsigned int i;

	pcws_gpio_ready = false;
	for (i = 0; i < ARRAY_SIZE(pcws_gpios); i++) {
		if (pcws_gpios[i]) {
			if (i < 2)
				gpiod_set_value_cansleep(pcws_gpios[i], 0);
			gpiod_put(pcws_gpios[i]);
			pcws_gpios[i] = NULL;
		}
	}
	if (pcws_gpio_lookup) {
		gpiod_remove_lookup_table(pcws_gpio_lookup);
		kfree(pcws_gpio_lookup);
		pcws_gpio_lookup = NULL;
	}
}

int pcws_cashdrawer_init(unsigned int model, bool test_mode)
{
	unsigned int i, j;
	bool active_low;
	int ret;

	if (!enable_cashdrawer)
		return gpio_chip_count || gpio_offset_count ? -EINVAL : 0;
	if (test_mode || gpio_chip_count != 4 || gpio_offset_count != 4)
		return -EINVAL;
	switch (model) {
	case PCWS_MODEL_WOODSTOCK:
	case PCWS_MODEL_ESSEX:
		active_low = true; /* Pulse low, status asserted high. */
		break;
	case PCWS_MODEL_YARROWSBURG:
	case PCWS_MODEL_BIGPOOL:
		active_low = false; /* Pulse high, status asserted low. */
		break;
	default:
		return -EOPNOTSUPP;
	}
	for (i = 0; i < 4; i++) {
		if (!gpio_chips[i] || !*gpio_chips[i] || gpio_offsets[i] > U16_MAX)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(gpio_chips[i], gpio_chips[j]) &&
			    gpio_offsets[i] == gpio_offsets[j])
				return -EINVAL;
	}
	pcws_gpio_lookup = kzalloc(struct_size(pcws_gpio_lookup, table, 5), GFP_KERNEL);
	if (!pcws_gpio_lookup)
		return -ENOMEM;
	for (i = 0; i < 4; i++) {
		struct gpiod_lookup *entry = &pcws_gpio_lookup->table[i];

		entry->key = gpio_chips[i];
		entry->chip_hwnum = gpio_offsets[i];
		entry->con_id = "pcws-cashdrawer";
		entry->idx = i;
		entry->flags = (i < 2 ? active_low : !active_low) ?
			GPIO_ACTIVE_LOW : GPIO_ACTIVE_HIGH;
	}
	gpiod_add_lookup_table(pcws_gpio_lookup);
	for (i = 0; i < 4; i++) {
		struct gpio_desc *desc = gpiod_get_index(NULL, "pcws-cashdrawer", i,
						      i < 2 ? GPIOD_OUT_LOW : GPIOD_IN);

		if (IS_ERR(desc)) {
			ret = PTR_ERR(desc);
			pr_err("pcws: cannot request GPIO %s:%u: %d\n",
			       gpio_chips[i], gpio_offsets[i], ret);
			pcws_cashdrawer_exit();
			return ret;
		}
		pcws_gpios[i] = desc;
	}
	pcws_gpio_ready = true;
	return 0;
}

int pcws_cashdrawer_open(unsigned int drawer)
{
	int ret, idle_ret;

	if (drawer > 1)
		return -EINVAL;
	if (!pcws_gpio_ready)
		return -EOPNOTSUPP;
	ret = mutex_lock_interruptible(&pcws_cashdrawer_mutex);
	if (ret)
		return ret;
	ret = pcws_gpio_set(pcws_gpios[drawer], 1);
	/* Original pulse is 50ms. Always attempt idle, even on a write error. */
	if (!ret)
		msleep(50);
	idle_ret = pcws_gpio_set(pcws_gpios[drawer], 0);
	mutex_unlock(&pcws_cashdrawer_mutex);
	return ret ? ret : idle_ret;
}

int pcws_cashdrawer_status(unsigned int drawer)
{
	if (drawer > 1)
		return -EINVAL;
	if (!pcws_gpio_ready)
		return -EOPNOTSUPP;
	/* The original wrappers put CDA in bit 1 and CDB in bit 0, then
	 * select bit (drawer != 0). Preserve that counterintuitive ABI:
	 * request 1 reads sense B, request 2 reads sense A.
	 */
	return gpiod_get_value_cansleep(pcws_gpios[3 - drawer]);
}
