#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_MITIGATION_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x81e6b37f, "dmi_get_system_info" },
	{ 0xbb7a1999, "gpiod_get_value_cansleep" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x26dfbd38, "misc_deregister" },
	{ 0x718c4554, "gpiod_put" },
	{ 0xf6480e98, "param_ops_uint" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x7f02188f, "__msecs_to_jiffies" },
	{ 0x9291cd3b, "memdup_user" },
	{ 0x92540fbf, "finish_wait" },
	{ 0x9ac69741, "gpiod_set_value_cansleep" },
	{ 0xb7f1eb2f, "gpiod_get_index" },
	{ 0x640716ea, "param_array_ops" },
	{ 0x69acdf38, "memcpy" },
	{ 0x3b6c41ea, "kstrtouint" },
	{ 0x37a0cba, "kfree" },
	{ 0x8c26d495, "prepare_to_wait_event" },
	{ 0xe2964344, "__wake_up" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xe783e261, "sysfs_emit" },
	{ 0x122c3a7e, "_printk" },
	{ 0x1d24c881, "___ratelimit" },
	{ 0x1000e51, "schedule" },
	{ 0x8ddd8aad, "schedule_timeout" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x3ae3bf84, "queue_delayed_work_on" },
	{ 0x7682ba4e, "__copy_overflow" },
	{ 0xc1115412, "cdev_add" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0x1e6d26a8, "strstr" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0xd140da38, "class_unregister" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0x89940875, "mutex_lock_interruptible" },
	{ 0xcb1fb1c, "misc_register" },
	{ 0x19f465bb, "param_ops_charp" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x835b4d4c, "device_create_with_groups" },
	{ 0x38aa1397, "gpiod_add_lookup_table" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0xf39d6569, "cancel_delayed_work_sync" },
	{ 0x823ae8b8, "param_ops_bool" },
	{ 0x4b4577bb, "device_destroy" },
	{ 0xb0e6a56, "delayed_work_timer_fn" },
	{ 0xff81487d, "gpiod_remove_lookup_table" },
	{ 0x544be8df, "kmalloc_trace" },
	{ 0x3de92247, "param_ops_int" },
	{ 0x99640e74, "class_register" },
	{ 0x99f2d00a, "sysfs_emit_at" },
	{ 0xf9a482f9, "msleep" },
	{ 0x5aaf13e, "cdev_init" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xe2c17b5d, "__SCT__might_resched" },
	{ 0x4344f508, "kmalloc_caches" },
	{ 0xa7aab139, "cdev_del" },
	{ 0x2d3385d3, "system_wq" },
	{ 0x3075a628, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "7CE8E0245AB68F10096ABC3");
MODULE_INFO(rhelversion, "9.8");
