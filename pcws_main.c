// SPDX-License-Identifier: GPL-2.0-only
/* Source reconstruction: see docs/STATUS.md for implemented functionality.
 * Legacy ioctl IDs and model values come directly from the original CTF.
 * No original machine code is linked into this module.
 */
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include "pcws_internal.h"
#include "pcws_uapi.h"

#define PCWS_VERSION "0.3.1"
/* These positive error codes are part of the original ABI. */
#define PCWS_COPY_FROM_ERROR 87L
#define PCWS_UNKNOWN_COMMAND 50L

static bool test_mode;
module_param(test_mode, bool, 0444);
MODULE_PARM_DESC(test_mode, "Allow metadata-only testing without workstation hardware");
static unsigned int model;
module_param(model, uint, 0444);
MODULE_PARM_DESC(model, "Explicit verified board model ID; 0 enables limited DMI detection");
static int drvpcwsapi_debug;
module_param(drvpcwsapi_debug, int, 0600);
MODULE_PARM_DESC(drvpcwsapi_debug, "Enable command diagnostics (legacy parameter)");

static dev_t pcws_devt;
static struct cdev pcws_cdev;
static struct class pcws_class = { .name = "pcws" };
static struct device *pcws_device;

static ssize_t pcwsmodel_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", model);
}
static DEVICE_ATTR_RO(pcwsmodel);

static ssize_t implementation_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "experimental-source-%s\n", PCWS_VERSION);
}
static DEVICE_ATTR_RO(implementation);

static ssize_t backend_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	bool gpio = pcws_cashdrawer_available() || pcws_board_available() || pcws_controls_available();
	bool serial = pcws_serial_available();

	return sysfs_emit(buf, "%s%s%s%s\n",
		pcws_cashdrawer_available() ? "gpio-cashdrawer" :
		gpio ? "gpio" : serial ? "serial" : "metadata-only",
		pcws_board_available() ? "+revision" : "",
		pcws_controls_available() ? "+controls" : "",
		gpio && serial ? "+serial" : "");
}
static DEVICE_ATTR_RO(backend);

static ssize_t power_led_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret = pcws_led_get();
	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", ret);
}
static ssize_t power_led_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int value;
	int ret = kstrtouint(buf, 0, &value);
	if (!ret)
		ret = pcws_led_set(value);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(power_led);
static ssize_t power_led_blink_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret = pcws_led_blink_get();
	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", ret);
}
static ssize_t power_led_blink_ms_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int value;
	int ret = kstrtouint(buf, 0, &value);
	if (!ret)
		ret = pcws_led_blink_set(value);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(power_led_blink_ms);
static ssize_t serial_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int first = pcws_power_get(1), second;
	if (first < 0)
		return first;
	second = pcws_power_get(2);
	if (second == -EINVAL)
		return sysfs_emit(buf, "1=%d\n", first);
	if (second < 0)
		return second;
	return sysfs_emit(buf, "1=%d 2=%d\n", first, second);
}
static ssize_t serial_power_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int port;
	int voltage, ret;
	char extra;
	if (sscanf(buf, "%u %d %c", &port, &voltage, &extra) != 2)
		return -EINVAL;
	ret = pcws_power_set(port, voltage);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(serial_power);
static ssize_t gpio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i, count = pcws_user_gpio_count(), value;
	ssize_t length = 0;
	if (!count)
		return -EOPNOTSUPP;
	for (i = 0; i < count; i++) {
		value = pcws_user_gpio_get(i);
		if (value < 0)
			return value;
		length += sysfs_emit_at(buf, length, "%d=%d\n", i, value);
	}
	return length;
}
static ssize_t gpio_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned int index, value;
	char extra;
	int ret;
	if (sscanf(buf, "%u %u %c", &index, &value, &extra) != 2)
		return -EINVAL;
	ret = pcws_user_gpio_set(index, value);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(gpio);

static struct attribute *pcws_attrs[] = {
	&dev_attr_pcwsmodel.attr,
	&dev_attr_implementation.attr,
	&dev_attr_backend.attr,
	&dev_attr_power_led.attr,
	&dev_attr_power_led_blink_ms.attr,
	&dev_attr_serial_power.attr,
	&dev_attr_gpio.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pcws);

static long pcws_copy_result(void __user *arg, const void *data, size_t size)
{
	/* Preserve the observed legacy copy-to-user failure code. */
	return copy_to_user(arg, data, size) ? -EINVAL : 0;
}

static long pcws_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned int id = cmd & 0xffff, length = cmd >> 16;
	void __user *user = (void __user *)arg;
	static const u8 version_text[] = { '1', '.', '0', '5', 'A' };
	static const u8 version_binary[] = { 1, 0, 5, 0 };
	u8 value;
	struct pcws_power_request power;
	int ret;

	if (drvpcwsapi_debug)
		pr_info_ratelimited("pcws: ioctl id=%#x length=%u\n", id, length);

	switch (id) {
	case LNX_EAPIPCWSTest:
	case LNX_EAPIPCWS32LTest:
	case LNX_EAPIPCWS64Test:
		if (length != 1)
			return -EINVAL;
		value = id == LNX_EAPIPCWSTest;
		return pcws_copy_result(user, &value, 1);
	case LNX_EAPIGetDriverVersion:
		if (length != sizeof(version_text))
			return -EINVAL;
		return pcws_copy_result(user, version_text, sizeof(version_text));
	case LNX_EAPIGetDriverVersionBinary:
		if (length != sizeof(version_binary))
			return -EINVAL;
		return pcws_copy_result(user, version_binary, sizeof(version_binary));
	case LNX_EAPIGetPCWSModel:
	case LNX_EAPIGetPCWSModelEx:
		if (length != 1)
			return -EINVAL;
		/* The old GetPCWSModel always returned 0x16, even on other models. */
		value = id == LNX_EAPIGetPCWSModel ? 0x16 : model;
		return pcws_copy_result(user, &value, 1);
	case LNX_EAPIUedCtrlSetComVolt:
	case LNX_EAPIUedCtrlGetComVolt:
		if (length != sizeof(power))
			return -EINVAL;
		if (copy_from_user(&power, user, sizeof(power)))
			return PCWS_COPY_FROM_ERROR;
		if (!power.port || power.port > 2)
			return -EINVAL;
		if (id == LNX_EAPIUedCtrlSetComVolt) {
			if (power.voltage != 0 && power.voltage != 5 &&
			    power.voltage != 9 && power.voltage != 12)
				return -EINVAL;
			return pcws_power_set(power.port, power.voltage);
		}
		ret = pcws_power_get(power.port);
		if (ret < 0)
			return ret;
		power.voltage = ret;
		return pcws_copy_result(user, &power, sizeof(power));
	case LNX_EAPIGetHWRevision:
		if (length != 1)
			return -EINVAL;
		ret = pcws_board_revision();
		if (ret < 0)
			return ret;
		value = ret;
		return pcws_copy_result(user, &value, 1);
	case LNX_EAPIDebugRequest:
		if (length != 1)
			return -EINVAL;
		if (copy_from_user(&value, user, 1))
			return PCWS_COPY_FROM_ERROR;
		if (value > 1)
			return -EINVAL;
		WRITE_ONCE(drvpcwsapi_debug, value);
		return 0;
	case LNX_EAPIOpenCashDrawer:
	case LNX_EAPIGetCashDrawerStatus:
		if (length != 1)
			return -EINVAL;
		if (copy_from_user(&value, user, 1))
			return PCWS_COPY_FROM_ERROR;
		if (value < 1 || value > 2)
			return -EINVAL;
		if (id == LNX_EAPIOpenCashDrawer)
			return pcws_cashdrawer_open(value - 1);
		ret = pcws_cashdrawer_status(value - 1);
		if (ret < 0)
			return ret;
		value = ret;
		return pcws_copy_result(user, &value, 1);
	default:
		if (id == 0x805 || id == 0x806 || id == 0x81f || id == 0x833 ||
		    (id >= 0x836 && id <= 0x83f) || id == 0x840 || id == 0x842 || id == 0x843 ||
		    id == 0x870 || id == 0x871)
			return pcws_display_ioctl(id, length, user);
		/* Known hardware commands must never silently report success. */
		if ((id >= LNX_EAPIOpenCashDrawer && id <= LNX_EAPISetBuzzer) ||
		    (id >= LNX_EAPIUedCtrlSetPortState &&
		     id <= LNX_EAPIUedCtrlGetCashDrawerVolt))
			return -EOPNOTSUPP;
		return PCWS_UNKNOWN_COMMAND;
	}
}

#ifdef CONFIG_COMPAT
static long pcws_compat_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	/* Implemented payloads contain only bytes, without native pointers. */
	return pcws_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static ssize_t pcws_read(struct file *file, char __user *buf,
			 size_t length, loff_t *position)
{
	return 0; /* Original readdevice is an EOF-only no-op. */
}

static ssize_t pcws_write(struct file *file, const char __user *buf,
			  size_t length, loff_t *position)
{
	return 0; /* Original writedevice consumes no data. */
}

static const struct file_operations pcws_fops = {
	.owner = THIS_MODULE,
	.read = pcws_read,
	.write = pcws_write,
	.unlocked_ioctl = pcws_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = pcws_compat_ioctl,
#endif
};

static bool pcws_valid_model(unsigned int value)
{
	return value == PCWS_MODEL_WS5A ||
		(value >= PCWS_MODEL_WOODSTOCK && value <= PCWS_MODEL_CRISFIELD);
}

static unsigned int pcws_detect_model(void)
{
	/* Exact strings from checkProdName/checkProdFamily, using DMI directly.
	 * Do not reproduce the original fixed physical-address MMIO probes.
	 */
	const char *family = dmi_get_system_info(DMI_PRODUCT_FAMILY);
	const char *product = dmi_get_system_info(DMI_PRODUCT_NAME);
	const char *vendor = dmi_get_system_info(DMI_SYS_VENDOR);

	/* Exact WS610 identity observed on the physical tom-micros workstation.
	 * Detection does not request GPIOs or open serial devices.
	 */
	if (vendor && family && product && !strcmp(vendor, "Micros Systems Inc.") &&
	    !strcmp(family, "BayTrail System") && !strcmp(product, "Workstation6"))
		return PCWS_MODEL_WOODSTOCK;
	if (!family || !product || !strstr(family, "Apollolake"))
		return PCWS_MODEL_NONE;
	if (!strcmp(product, "Micros Workstation 3"))
		return PCWS_MODEL_YARROWSBURG;
	if (!strcmp(product, "Micros Express Station 4"))
		return PCWS_MODEL_CRISFIELD;
	return PCWS_MODEL_NONE;
}

static int __init pcws_init(void)
{
	int ret;

	if (model && !pcws_valid_model(model))
		return -EINVAL;
	if (test_mode && model)
		return -EINVAL; /* A VM must never pretend to be real workstation hardware. */
	if (!model && !test_mode)
		model = pcws_detect_model();
	if (!model && !test_mode) {
		pr_info("pcws: no supported DMI match; see source documentation\n");
		return -ENODEV;
	}
	ret = pcws_board_init(model, test_mode);
	if (ret)
		return ret;
	ret = pcws_cashdrawer_init(model, test_mode);
	if (ret)
		goto err_board;
	ret = pcws_controls_init(model, test_mode);
	if (ret)
		goto err_backend;
	ret = pcws_serial_init(model, test_mode);
	if (ret)
		goto err_controls;
	ret = alloc_chrdev_region(&pcws_devt, 100, 1, "pcws");
	if (ret)
		goto err_serial;
	cdev_init(&pcws_cdev, &pcws_fops);
	pcws_cdev.owner = THIS_MODULE;
	ret = cdev_add(&pcws_cdev, pcws_devt, 1);
	if (ret)
		goto err_region;
	/* Static class registration avoids class_create() signature changes
	 * and vendor backports that do not match LINUX_VERSION_CODE.
	 */
	ret = class_register(&pcws_class);
	if (ret)
		goto err_cdev;
	pcws_device = device_create_with_groups(&pcws_class, NULL, pcws_devt,
					      NULL, pcws_groups, "pcws");
	if (IS_ERR(pcws_device)) {
		ret = PTR_ERR(pcws_device);
		goto err_class;
	}
	pr_info("pcws: experimental source %s, model=%u, backend=%s\n",
		PCWS_VERSION, model, (pcws_cashdrawer_available() || pcws_board_available() || pcws_controls_available()) ? "gpio" : "metadata-only");
	return 0;

err_class:
	class_unregister(&pcws_class);
err_cdev:
	cdev_del(&pcws_cdev);
err_region:
	unregister_chrdev_region(pcws_devt, 1);
err_serial:
	pcws_serial_exit();
err_controls:
	pcws_controls_exit();
err_backend:
	pcws_cashdrawer_exit();
err_board:
	pcws_board_exit();
	return ret;
}

static void __exit pcws_exit(void)
{
	device_destroy(&pcws_class, pcws_devt);
	class_unregister(&pcws_class);
	cdev_del(&pcws_cdev);
	unregister_chrdev_region(pcws_devt, 1);
	pcws_serial_exit();
	pcws_controls_exit();
	pcws_cashdrawer_exit();
	pcws_board_exit();
}

module_init(pcws_init);
module_exit(pcws_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("PCWS source reconstruction contributors");
MODULE_DESCRIPTION("Experimental PCWS source reconstruction; partial hardware support");
MODULE_VERSION(PCWS_VERSION);
