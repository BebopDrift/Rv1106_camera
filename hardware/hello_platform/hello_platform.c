// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* 用户读取 message 属性文件时，由内核调用 */
static ssize_t message_show(struct device *dev,
                            struct device_attribute *attr,
                            char *buf)
{
    const char *message;
    int ret;

    ret = of_property_read_string(
        dev->of_node,
        "qu,message",
        &message
    );

    if (ret)
        return ret;

    dev_info(dev, "message_show called\n");

    return sysfs_emit(buf, "%s\n", message);
}

/* 定义名为 message 的只读设备属性 */
static DEVICE_ATTR_RO(message);

struct hello_event {
    __le64 seq;
    __le64 ts_ns;
    __le32 value;
    __le32 reserved;
};

struct hello_data {
    struct gpio_desc *input;
    struct gpio_desc *output;
    int irq;
    u64 event_seq;

    DECLARE_KFIFO(events, struct hello_event, 32);
    spinlock_t events_lock;
    wait_queue_head_t events_wait;
    bool disconnected;

    struct miscdevice event_device;
    struct kref refs;
};

static void hello_data_free(struct kref *refs)
{
    struct hello_data *data = container_of(refs, struct hello_data, refs);

    kfree(data);
}

static void hello_data_put(void *arg)
{
    struct hello_data *data = arg;

    kref_put(&data->refs, hello_data_free);
}

static int hello_events_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct hello_data *data =
        container_of(misc, struct hello_data, event_device);

    kref_get(&data->refs);
    file->private_data = data;

    return stream_open(inode, file);
}

static int hello_events_release(struct inode *inode, struct file *file)
{
    hello_data_put(file->private_data);
    return 0;
}

static ssize_t hello_events_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *pos)
{
    struct hello_data *data = file->private_data;
    struct hello_event event;
    int ret;

    if (count == 0)
        return 0;
    if (count < sizeof(event))
        return -EINVAL;

    spin_lock_irq(&data->events_lock);
    if (kfifo_is_empty(&data->events) && !data->disconnected) {
        if (file->f_flags & O_NONBLOCK) {
            spin_unlock_irq(&data->events_lock);
            return -EAGAIN;
        }

        ret = wait_event_interruptible_lock_irq(
            data->events_wait,
            !kfifo_is_empty(&data->events) || data->disconnected,
            data->events_lock);
        if (ret) {
            spin_unlock_irq(&data->events_lock);
            return ret;
        }
    }

    ret = kfifo_get(&data->events, &event);
    spin_unlock_irq(&data->events_lock);

    if (!ret)
        return 0;
    if (copy_to_user(buf, &event, sizeof(event)))
        return -EFAULT;

    return sizeof(event);
}

static __poll_t hello_events_poll(struct file *file, poll_table *wait)
{
    struct hello_data *data = file->private_data;
    unsigned long flags;
    __poll_t mask = 0;

    poll_wait(file, &data->events_wait, wait);

    spin_lock_irqsave(&data->events_lock, flags);
    if (!kfifo_is_empty(&data->events))
        mask |= EPOLLIN | EPOLLRDNORM;
    if (data->disconnected)
        mask |= EPOLLHUP;
    spin_unlock_irqrestore(&data->events_lock, flags);

    return mask;
}

static const struct file_operations hello_event_fops = {
    .owner   = THIS_MODULE,
    .open    = hello_events_open,
    .release = hello_events_release,
    .read    = hello_events_read,
    .poll    = hello_events_poll,
    .llseek  = no_llseek,
};

static irqreturn_t hello_irq_thread(int irq, void *dev_id)
{
    struct device *dev = dev_id;
    struct hello_data *data = dev_get_drvdata(dev);
    u64 ts_ns = ktime_get_ns();
    u64 seq;
    int value;
    struct hello_event event;
    bool queued;

    value = gpiod_get_value_cansleep(data->input);
    if (value < 0) {
        dev_err(dev, "IRQ: failed to read input: %d\n", value);
        return IRQ_HANDLED;
    }

    seq = ++data->event_seq;
		event.seq = cpu_to_le64(seq);
		event.ts_ns = cpu_to_le64(ts_ns);
		event.value = cpu_to_le32(value);
		event.reserved = 0;

		spin_lock_irq(&data->events_lock);
		queued = kfifo_put(&data->events, event);
		spin_unlock_irq(&data->events_lock);

		if (queued)
				wake_up_interruptible(&data->events_wait);
		else
				dev_warn_ratelimited(dev, "GPIO event queue full\n");

    dev_info(dev, "GPIO interrupt: input=%d seq=%llu ts_ns=%llu\n",
             value, (unsigned long long)seq,
             (unsigned long long)ts_ns);

    return IRQ_HANDLED;
}

static ssize_t level_show(
	struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct hello_data *data = dev_get_drvdata(dev);
	int value;

	value = gpiod_get_value_cansleep(data->input);
	if(value < 0)
		return value;

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t level_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct hello_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 10, &value);
	if(ret)
		return ret;
	if(value > 1)
		return -EINVAL;

	gpiod_set_value_cansleep(data->output, value);

	return count;
}

static DEVICE_ATTR_RW(level);

/* 内核发现匹配的设备后，调用这个函数 */
static int hello_probe(struct platform_device *pdev)
{
		struct device *dev = &pdev->dev;
		struct hello_data *data;

    const char *message;
    int ret;

    dev_info(&pdev->dev, "probe entered\n");

    ret = of_property_read_string(
        pdev->dev.of_node,
        "qu,message",
        &message
    );

    if (ret) {
        dev_err(&pdev->dev,
                "failed to read qu,message: %d/n", ret);
        return ret;
    }

		dev_info(&pdev->dev, "message: %s\n", message);

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data)
				return -ENOMEM;

		kref_init(&data->refs);
		ret = devm_add_action_or_reset(dev, hello_data_put, data);
		if (ret)
				return ret;

		INIT_KFIFO(data->events);
		spin_lock_init(&data->events_lock);
		init_waitqueue_head(&data->events_wait);

		data->input = devm_gpiod_get(dev, "input", GPIOD_IN);
		if(IS_ERR(data->input))
		return dev_err_probe(dev, PTR_ERR(data->input), "failed to get input GPIO\n");

		data->output = devm_gpiod_get(dev, "output", GPIOD_OUT_LOW);
		if (IS_ERR(data->output))
			return dev_err_probe(dev, PTR_ERR(data->output),
					     "failed to get output GPIO\n");

		platform_set_drvdata(pdev, data);

		data->irq = gpiod_to_irq(data->input);
		if (data->irq < 0)
				return dev_err_probe(dev, data->irq,
														 "failed to get input IRQ\n");

		ret = devm_request_threaded_irq(
				dev,
				data->irq,
				NULL,
				hello_irq_thread,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"hello-gpio",
				dev
		);
		if (ret)
				return dev_err_probe(dev, ret,
														 "failed to request input IRQ\n");

		dev_info(dev, "GPIO IRQ ready: irq=%d\n", data->irq);

		ret = device_create_file(&pdev->dev, &dev_attr_message);
		if (ret)
		return ret;

		ret = device_create_file(dev, &dev_attr_level);
		if (ret) {
			device_remove_file(dev, &dev_attr_message);
			return ret;
		}

		dev_info(dev, "GPIO ready: input and output acquired\n");

		data->event_device.minor = MISC_DYNAMIC_MINOR;
		data->event_device.name = "hello-gpio-events";
		data->event_device.fops = &hello_event_fops;
		data->event_device.parent = dev;

		ret = misc_register(&data->event_device);
		if (ret) {
				device_remove_file(dev, &dev_attr_level);
				device_remove_file(dev, &dev_attr_message);
				return ret;
		}

		return 0;
}


/* 已绑定的设备与驱动解除绑定时调用 */
static int hello_remove(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
	struct hello_data *data = platform_get_drvdata(pdev);
  device_remove_file(&pdev->dev, &dev_attr_message);
	device_remove_file(&pdev->dev, &dev_attr_level);

	devm_free_irq(dev, data->irq, dev);
	gpiod_set_value_cansleep(data->output, 0);

	misc_deregister(&data->event_device);

	spin_lock_irq(&data->events_lock);
	data->disconnected = true;
	spin_unlock_irq(&data->events_lock);
	wake_up_interruptible_all(&data->events_wait);
	dev_info(&pdev->dev, "remove called\n");

    return 0;
}

/* 驱动支持的设备树 compatible */
static const struct of_device_id hello_of_match[] = {
    { .compatible = "qu,hello-platform" },
    { }
};

MODULE_DEVICE_TABLE(of, hello_of_match);

/* 把回调函数、驱动名称和匹配表组织起来 */
static struct platform_driver hello_driver = {
    .probe = hello_probe,
    .remove = hello_remove,
    .driver = {
        .name = "qu-hello-platform",
        .of_match_table = hello_of_match,
    },
};

module_platform_driver(hello_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qu");
MODULE_DESCRIPTION("Luckfox platform driver learning example");
