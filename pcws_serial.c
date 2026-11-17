// SPDX-License-Identifier: GPL-2.0-only
/* Bounded display request broker. A root-only helper owns the serial TTY. */
#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "pcws_internal.h"
#include "pcws_recovered.h"

#define PCWS_SERIAL_MAGIC 0x53574350U
#define PCWS_SERIAL_MAX 4096
struct pcws_serial_header {
	__le32 magic, sequence;
	__le16 command, length;
	__le32 status;
};
static_assert(sizeof(struct pcws_serial_header) == 16);

static DEFINE_MUTEX(serial_transaction);
static DEFINE_MUTEX(serial_state);
static DECLARE_WAIT_QUEUE_HEAD(serial_requests);
static DECLARE_WAIT_QUEUE_HEAD(serial_replies);
static bool serial_registered, serial_connected, serial_pending, serial_delivered, serial_done;
static u32 serial_sequence;
static u16 serial_command, serial_length;
static int serial_status;
static u8 serial_payload[PCWS_SERIAL_MAX];

static int serial_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&serial_state);
	if (serial_connected)
		ret = -EBUSY;
	else
		serial_connected = true;
	mutex_unlock(&serial_state);
	return ret;
}

static int serial_release(struct inode *inode, struct file *file)
{
	mutex_lock(&serial_state);
	serial_connected = false;
	if (serial_pending) {
		serial_status = -EPIPE;
		serial_done = true;
	}
	mutex_unlock(&serial_state);
	wake_up_interruptible(&serial_replies);
	return 0;
}

static ssize_t serial_read(struct file *file, char __user *buffer, size_t length, loff_t *pos)
{
	struct pcws_serial_header header;
	ssize_t ret;

	for (;;) {
		ret = wait_event_interruptible(serial_requests,
			READ_ONCE(serial_pending) && !READ_ONCE(serial_delivered) && !READ_ONCE(serial_done));
		if (ret)
			return ret;
		mutex_lock(&serial_state);
		if (serial_pending && !serial_delivered && !serial_done)
			break;
		mutex_unlock(&serial_state);
	}
	if (length < sizeof(header) + serial_length) {
		ret = -EMSGSIZE;
		goto out;
	}
	header.magic = cpu_to_le32(PCWS_SERIAL_MAGIC);
	header.sequence = cpu_to_le32(serial_sequence);
	header.command = cpu_to_le16(serial_command);
	header.length = cpu_to_le16(serial_length);
	header.status = 0;
	if (copy_to_user(buffer, &header, sizeof(header)) ||
	    copy_to_user(buffer + sizeof(header), serial_payload, serial_length)) {
		ret = -EFAULT;
		goto out;
	}
	serial_delivered = true;
	ret = sizeof(header) + serial_length;
out:
	mutex_unlock(&serial_state);
	return ret;
}

static ssize_t serial_write(struct file *file, const char __user *buffer,
			    size_t length, loff_t *pos)
{
	struct pcws_serial_header *header;
	s32 status;
	u16 response_length;
	ssize_t ret = -ESTALE;

	if (length < sizeof(*header) || length > sizeof(*header) + PCWS_SERIAL_MAX)
		return -EMSGSIZE;
	header = memdup_user(buffer, length);
	if (IS_ERR(header))
		return PTR_ERR(header);
	status = (s32)le32_to_cpu(header->status);
	response_length = le16_to_cpu(header->length);
	if (le32_to_cpu(header->magic) != PCWS_SERIAL_MAGIC ||
	    response_length != length - sizeof(*header) || status > 0 || status < -4095) {
		ret = -EINVAL;
		goto out;
	}
	mutex_lock(&serial_state);
	if (serial_pending && serial_delivered && !serial_done &&
	    le32_to_cpu(header->sequence) == serial_sequence &&
	    le16_to_cpu(header->command) == serial_command) {
		if (response_length != ((!status && serial_command == 0x871) ? 1 : 0)) {
			ret = -EMSGSIZE;
		} else {
			if (response_length)
				memcpy(serial_payload, header + 1, response_length);
			serial_status = status;
			serial_done = true;
			ret = length;
		}
	}
	mutex_unlock(&serial_state);
	if (ret > 0)
		wake_up_interruptible(&serial_replies);
out:
	kfree(header);
	return ret;
}

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.open = serial_open,
	.release = serial_release,
	.read = serial_read,
	.write = serial_write,
};
static struct miscdevice serial_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pcws-serial",
	.fops = &serial_fops,
	.mode = 0600,
};

int pcws_serial_init(unsigned int model, bool test_mode)
{
	int ret;

	if (test_mode || (model != PCWS_MODEL_WOODSTOCK && model != PCWS_MODEL_YARROWSBURG))
		return 0;
	ret = misc_register(&serial_device);
	if (!ret)
		serial_registered = true;
	return ret;
}

bool pcws_serial_available(void)
{
	return READ_ONCE(serial_registered) && READ_ONCE(serial_connected);
}

void pcws_serial_exit(void)
{
	if (serial_registered) {
		misc_deregister(&serial_device);
		serial_registered = false;
	}
}

static int serial_request(unsigned int command, u8 *payload, unsigned int length)
{
	long waited;
	int ret;

	ret = mutex_lock_interruptible(&serial_transaction);
	if (ret)
		return ret;
	mutex_lock(&serial_state);
	if (!serial_registered || !serial_connected) {
		ret = -EOPNOTSUPP;
		goto out_state;
	}
	serial_sequence++;
	serial_command = command;
	serial_length = length;
	memcpy(serial_payload, payload, length);
	serial_pending = true;
	serial_delivered = false;
	serial_done = false;
	mutex_unlock(&serial_state);
	wake_up_interruptible(&serial_requests);
	waited = wait_event_interruptible_timeout(serial_replies, READ_ONCE(serial_done),
						 msecs_to_jiffies(5000));
	mutex_lock(&serial_state);
	ret = waited < 0 ? (int)waited : !waited ? -ETIMEDOUT : serial_status;
	if (!ret && command == 0x871)
		memcpy(payload, serial_payload, 1);
	serial_pending = false;
out_state:
	mutex_unlock(&serial_state);
	mutex_unlock(&serial_transaction);
	return ret;
}

long pcws_display_ioctl(unsigned int id, unsigned int length, void __user *arg)
{
	u8 *payload;
	int ret;

	switch (id) {
	case LNX_EAPISetDongleSerialPortPower:
		if (length != 2)
			return -EINVAL;
		break;
	case LNX_EAPIGetDongleSerialPortPower:
		if (length != 1)
			return -EINVAL;
		break;
	case LNX_EAPIRearDispFullUpdate:
		if (length != 20 && length != 40)
			return -EINVAL;
		break;
	case LNX_EAPIWriteToRearDisp:
		if (!length || length > 40)
			return -EINVAL;
		break;
	case LNX_EAPIBitBlt2Panel:
		if (length != 2048)
			return -EINVAL;
		break;
	case LNX_EAPISet1PixelWhiteRear:
	case LNX_EAPISet1PixelBlackRear:
		if (length != 2)
			return -EINVAL;
		break;
	case LNX_EAPIRearDisplaySetToWriteRaw:
	case LNX_EAPISetLCDBacklightRear:
	case LNX_EAPISetImageReverseRear:
	case LNX_EAPISetImageInverseRear:
	case LNX_EAPISetDisplayReverseRear:
	case LNX_EAPITurnOnOffRearLCD:
	case LNX_EAPISetDisplayAllPointOnRear:
		if (length != 1)
			return -EINVAL;
		break;
	case LNX_EAPIClearRearDisplay:
	case LNX_EAPIDisplayClearRearLCD:
	case LNX_EAPILCDContrastUpRear:
	case LNX_EAPILCDContrastDownRear:
		if (length)
			return -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}
	payload = kzalloc(length ? length : 1, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	if (length && copy_from_user(payload, arg, length)) {
		ret = 87; /* Legacy input-copy failure. */
		goto out;
	}
	if ((id == LNX_EAPISet1PixelWhiteRear || id == LNX_EAPISet1PixelBlackRear) &&
	    (payload[0] >= 240 || payload[1] >= 64)) {
		ret = -EINVAL;
		goto out;
	}
	if (id == 0x870 && (payload[0] > 1 ||
	    (payload[1] != 0 && payload[1] != 5 && payload[1] != 9 && payload[1] != 12))) {
		ret = -EINVAL;
		goto out;
	}
	if (length == 1 && payload[0] > (id == LNX_EAPISetLCDBacklightRear ? 4 : 1)) {
		ret = -EINVAL;
		goto out;
	}
	ret = serial_request(id, payload, length);
	if (!ret && id == 0x871 && copy_to_user(arg, payload, 1))
		ret = -EINVAL;
out:
	kfree(payload);
	return ret;
}
