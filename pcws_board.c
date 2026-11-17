// SPDX-License-Identifier: GPL-2.0-only
/* WS310/WS610 board revision, ported from ws*_GetHWRevision.
 * Inputs are configured in bit significance order, low bit first.
 */
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "pcws_internal.h"
#include "pcws_recovered.h"

static char *revision_gpio_chips[4];
static unsigned int revision_chip_count;
module_param_array(revision_gpio_chips, charp, &revision_chip_count, 0444);
MODULE_PARM_DESC(revision_gpio_chips, "Four verified board-revision GPIO chip labels, bit 0 first");
static unsigned int revision_gpio_offsets[4];
static unsigned int revision_offset_count;
module_param_array(revision_gpio_offsets, uint, &revision_offset_count, 0444);
MODULE_PARM_DESC(revision_gpio_offsets, "Four verified chip-local board-revision offsets, bit 0 first");

static struct gpiod_lookup_table *revision_lookup;
static struct gpio_desc *revision_gpios[4];
static bool revision_ready;

bool pcws_board_available(void)
{
	return revision_ready;
}

void pcws_board_exit(void)
{
	unsigned int i;

	revision_ready = false;
	for (i = 0; i < ARRAY_SIZE(revision_gpios); i++) {
		gpiod_put(revision_gpios[i]);
		revision_gpios[i] = NULL;
	}
	if (revision_lookup) {
		gpiod_remove_lookup_table(revision_lookup);
		kfree(revision_lookup);
		revision_lookup = NULL;
	}
}

int pcws_board_init(unsigned int model, bool test_mode)
{
	unsigned int i, j;
	int ret;

	if (!revision_chip_count && !revision_offset_count)
		return 0;
	if (test_mode || revision_chip_count != 4 || revision_offset_count != 4)
		return -EINVAL;
	if (model != PCWS_MODEL_WOODSTOCK && model != PCWS_MODEL_YARROWSBURG)
		return -EOPNOTSUPP;
	for (i = 0; i < 4; i++) {
		if (!revision_gpio_chips[i] || !*revision_gpio_chips[i] ||
		    revision_gpio_offsets[i] > U16_MAX)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(revision_gpio_chips[i], revision_gpio_chips[j]) &&
			    revision_gpio_offsets[i] == revision_gpio_offsets[j])
				return -EINVAL;
	}
	revision_lookup = kzalloc(struct_size(revision_lookup, table, 5), GFP_KERNEL);
	if (!revision_lookup)
		return -ENOMEM;
	for (i = 0; i < 4; i++) {
		struct gpiod_lookup *entry = &revision_lookup->table[i];

		entry->key = revision_gpio_chips[i];
		entry->chip_hwnum = revision_gpio_offsets[i];
		entry->con_id = "pcws-board-revision";
		entry->idx = i;
		entry->flags = GPIO_ACTIVE_HIGH;
	}
	gpiod_add_lookup_table(revision_lookup);
	for (i = 0; i < 4; i++) {
		struct gpio_desc *desc = gpiod_get_index(NULL, "pcws-board-revision", i, GPIOD_IN);

		if (IS_ERR(desc)) {
			ret = PTR_ERR(desc);
			pr_err("pcws: cannot request revision GPIO %s:%u: %d\n",
			       revision_gpio_chips[i], revision_gpio_offsets[i], ret);
			pcws_board_exit();
			return ret;
		}
		revision_gpios[i] = desc;
	}
	revision_ready = true;
	return 0;
}

int pcws_board_revision(void)
{
	unsigned int i, revision = 0;
	int value;

	if (!revision_ready)
		return -EOPNOTSUPP;
	for (i = 0; i < ARRAY_SIZE(revision_gpios); i++) {
		value = gpiod_get_value_cansleep(revision_gpios[i]);
		if (value < 0)
			return value;
		revision |= !!value << i;
	}
	/* Original sysInfo_GetHWRevision adds ASCII 'A' to the four bits. */
	return 'A' + revision;
}
