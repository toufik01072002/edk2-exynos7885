// SPDX-License-Identifier: GPL-2.0
/*
 * A20 Virtual Boot handoff mailbox producer.
 *
 * The mailbox is a fixed, no-map reserved-memory window. This driver is the
 * only Linux writer; userspace submits one complete ABI record through
 * /dev/a20-handoff. The driver computes CRC32 and publishes magic last.
 */
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define A20_MAILBOX_PHYS       0x90200000ULL
#define A20_MAILBOX_SIZE       0x1000
#define A20_RECORD_MAGIC       0x31464F484F323041ULL
#define A20_RECORD_VERSION     1
#define A20_RECORD_HEADER_SIZE 56

struct a20_handoff_record {
	u64 magic;
	u32 version;
	u32 header_size;
	u32 record_size;
	u32 flags;
	u64 dtb_address;
	u64 dtb_size;
	u64 cmdline_address;
	u32 cmdline_size;
	u32 crc32;
	u8 reserved[A20_MAILBOX_SIZE - A20_RECORD_HEADER_SIZE];
};

struct a20_mailbox {
	void __iomem *base;
	struct miscdevice misc;
	struct mutex lock;
};

static ssize_t a20_mailbox_write(struct file *file,
				 const char __user *buffer,
				 size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct a20_mailbox *mailbox =
		container_of(misc, struct a20_mailbox, misc);
	struct a20_handoff_record *record;
	u64 magic = A20_RECORD_MAGIC;
	u32 expected_crc;
	ssize_t ret = count;

	if (count != A20_MAILBOX_SIZE || *ppos != 0)
		return -EINVAL;

	record = kzalloc(sizeof(*record), GFP_KERNEL);
	if (!record)
		return -ENOMEM;

	if (copy_from_user(record, buffer, sizeof(*record))) {
		ret = -EFAULT;
		goto out;
	}

	if (record->magic != A20_RECORD_MAGIC ||
	    record->version != A20_RECORD_VERSION ||
	    record->header_size != A20_RECORD_HEADER_SIZE ||
	    record->record_size < A20_RECORD_HEADER_SIZE ||
	    record->record_size > A20_MAILBOX_SIZE ||
	    record->crc32 != 0) {
		ret = -EINVAL;
		goto out;
	}

	record->crc32 = 0;
	expected_crc = crc32_le(~0U, (u8 *)record, record->record_size) ^ ~0U;
	record->crc32 = expected_crc;

	mutex_lock(&mailbox->lock);
	/* Invalidate first, then publish a complete CRC-protected record. */
	memset_io(mailbox->base, 0, sizeof(magic));
	wmb();
	magic = record->magic;
	record->magic = 0;
	memcpy_toio(mailbox->base, record, sizeof(*record));
	wmb();
	memcpy_toio(mailbox->base, &magic, sizeof(magic));
	wmb();
	mutex_unlock(&mailbox->lock);

out:
	kfree(record);
	return ret;
}

static const struct file_operations a20_mailbox_fops = {
	.owner = THIS_MODULE,
	.write = a20_mailbox_write,
	.llseek = no_llseek,
};

static int a20_mailbox_probe(struct platform_device *pdev)
{
	struct a20_mailbox *mailbox;
	struct device_node *region;
	struct resource res;
	int ret;

	region = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!region)
		return -ENODEV;

	ret = of_address_to_resource(region, 0, &res);
	of_node_put(region);
	if (ret)
		return ret;

	if (res.start != A20_MAILBOX_PHYS ||
	    resource_size(&res) != A20_MAILBOX_SIZE)
		return -EINVAL;

	mailbox = devm_kzalloc(&pdev->dev, sizeof(*mailbox), GFP_KERNEL);
	if (!mailbox)
		return -ENOMEM;

	mailbox->base = ioremap(res.start, resource_size(&res));
	if (!mailbox->base)
		return -ENOMEM;

	mutex_init(&mailbox->lock);
	mailbox->misc.minor = MISC_DYNAMIC_MINOR;
	mailbox->misc.name = "a20-handoff";
	mailbox->misc.fops = &a20_mailbox_fops;
	mailbox->misc.parent = &pdev->dev;

	ret = misc_register(&mailbox->misc);
	if (ret) {
		iounmap(mailbox->base);
		return ret;
	}

	platform_set_drvdata(pdev, mailbox);
	dev_info(&pdev->dev, "mailbox writer ready at 0x%llx (%u bytes)\n",
		 (unsigned long long)res.start, A20_MAILBOX_SIZE);
	return 0;
}

static int a20_mailbox_remove(struct platform_device *pdev)
{
	struct a20_mailbox *mailbox = platform_get_drvdata(pdev);

	misc_deregister(&mailbox->misc);
	iounmap(mailbox->base);
	return 0;
}

static const struct of_device_id a20_mailbox_of_match[] = {
	{ .compatible = "samsung,a20-handoff-producer" },
	{ }
};
MODULE_DEVICE_TABLE(of, a20_mailbox_of_match);

static struct platform_driver a20_mailbox_driver = {
	.probe = a20_mailbox_probe,
	.remove = a20_mailbox_remove,
	.driver = {
		.name = "a20-handoff-mailbox",
		.of_match_table = a20_mailbox_of_match,
	},
};
module_platform_driver(a20_mailbox_driver);

MODULE_DESCRIPTION("Samsung A20 Virtual Boot handoff mailbox producer");
MODULE_AUTHOR("7K / OpenAI collaboration");
MODULE_LICENSE("GPL");
