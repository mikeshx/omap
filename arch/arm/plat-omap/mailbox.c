/*
 * OMAP mailbox driver
 *
 * Copyright (C) 2006-2009 Nokia Corporation. All rights reserved.
 *
 * Contact: Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/err.h>
#include <linux/notifier.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm_qos.h>

#include <plat/mailbox.h>

static struct omap_mbox **mboxes;

static int mbox_configured;
static DEFINE_MUTEX(mbox_configured_lock);
static struct pm_qos_request mbox_qos_request;

#define SET_MPU_CORE_CONSTRAINT 10
#define CLEAR_MPU_CORE_CONSTRAINT PM_QOS_DEFAULT_VALUE

static unsigned int mbox_kfifo_size = CONFIG_OMAP_MBOX_KFIFO_SIZE;
module_param(mbox_kfifo_size, uint, S_IRUGO);
MODULE_PARM_DESC(mbox_kfifo_size, "Size of omap's mailbox kfifo (bytes)");

/* Runtime PM */
static int omap_mbox_save_ctx(struct device *dev, void *data)
{
	struct omap_mbox *mbox = dev_get_drvdata(dev);

	if (!mbox->ops->save_ctx) {
		dev_err(mbox->dev, "%s:\tno save\n", __func__);
		return -EINVAL;
	}

	mbox->ops->save_ctx(mbox);

	return 0;
}

static int omap_mbox_restore_ctx(struct device *dev, void *data)
{
	struct omap_mbox *mbox = dev_get_drvdata(dev);

	/* mailbox is not init'ed.. no need to restore */
	if (!mbox->use_count)
		return 0;

	if (!mbox->ops->restore_ctx) {
		dev_err(mbox->dev, "%s:\tno restore\n", __func__);
		return -EINVAL;
	}

	mbox->ops->restore_ctx(mbox);

	return 0;
}

static int mbox_runtime_resume(struct device *dev)
{
	int ret;

	pm_qos_update_request(&mbox_qos_request, SET_MPU_CORE_CONSTRAINT);
	ret = device_for_each_child(dev, NULL, omap_mbox_restore_ctx);
	if (ret)
		pm_qos_update_request(&mbox_qos_request,
						CLEAR_MPU_CORE_CONSTRAINT);

	return ret;
}

static int mbox_runtime_suspend(struct device *dev)
{
	int ret = device_for_each_child(dev, NULL, omap_mbox_save_ctx);

	if (!ret)
		pm_qos_update_request(&mbox_qos_request,
					 CLEAR_MPU_CORE_CONSTRAINT);

	return ret;
}

const struct dev_pm_ops mbox_pm_ops = {
	SET_RUNTIME_PM_OPS(mbox_runtime_suspend, mbox_runtime_resume, NULL)
};

/* Mailbox FIFO handle functions */
static inline mbox_msg_t mbox_fifo_read(struct omap_mbox *mbox)
{
	return mbox->ops->fifo_read(mbox);
}
static inline void mbox_fifo_write(struct omap_mbox *mbox, mbox_msg_t msg)
{
	mbox->ops->fifo_write(mbox, msg);
}
static inline int mbox_fifo_empty(struct omap_mbox *mbox)
{
	return mbox->ops->fifo_empty(mbox);
}
static inline int mbox_fifo_full(struct omap_mbox *mbox)
{
	return mbox->ops->fifo_full(mbox);
}

/* Mailbox IRQ handle functions */
static inline void ack_mbox_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	if (mbox->ops->ack_irq)
		mbox->ops->ack_irq(mbox, irq);
}
static inline int is_mbox_irq(struct omap_mbox *mbox, omap_mbox_irq_t irq)
{
	return mbox->ops->is_irq(mbox, irq);
}

/*
 * message sender
 */
static int __mbox_poll_for_space(struct omap_mbox *mbox)
{
	int ret = 0, i = 1000;

	while (mbox_fifo_full(mbox)) {
		if (mbox->ops->type == OMAP_MBOX_TYPE2)
			return -1;
		if (--i == 0)
			return -1;
		udelay(1);
	}
	return ret;
}

int omap_mbox_msg_send(struct omap_mbox *mbox, mbox_msg_t msg)
{
	struct omap_mbox_queue *mq = mbox->txq;
	int ret = 0, len;

	spin_lock_bh(&mq->lock);

	if (kfifo_avail(&mq->fifo) < sizeof(msg)) {
		ret = -ENOMEM;
		goto out;
	}

	if (kfifo_is_empty(&mq->fifo) && !__mbox_poll_for_space(mbox)) {
		mbox_fifo_write(mbox, msg);
		goto out;
	}

	len = kfifo_in(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
	WARN_ON(len != sizeof(msg));

	tasklet_schedule(&mbox->txq->tasklet);

out:
	spin_unlock_bh(&mq->lock);
	return ret;
}
EXPORT_SYMBOL(omap_mbox_msg_send);

static void mbox_tx_tasklet(unsigned long tx_data)
{
	struct omap_mbox *mbox = (struct omap_mbox *)tx_data;
	struct omap_mbox_queue *mq = mbox->txq;
	mbox_msg_t msg;
	int ret;

	while (kfifo_len(&mq->fifo)) {
		if (__mbox_poll_for_space(mbox)) {
			omap_mbox_enable_irq(mbox, IRQ_TX);
			break;
		}

		ret = kfifo_out(&mq->fifo, (unsigned char *)&msg,
								sizeof(msg));
		WARN_ON(ret != sizeof(msg));

		mbox_fifo_write(mbox, msg);
	}
}

/*
 * Message receiver(workqueue)
 */
static void mbox_rx_work(struct work_struct *work)
{
	struct omap_mbox_queue *mq =
			container_of(work, struct omap_mbox_queue, work);
	mbox_msg_t msg;
	int len;

	while (kfifo_len(&mq->fifo) >= sizeof(msg)) {
		len = kfifo_out(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
		WARN_ON(len != sizeof(msg));

		blocking_notifier_call_chain(&mq->mbox->notifier, len,
								(void *)msg);
		spin_lock_irq(&mq->lock);
		if (mq->full) {
			mq->full = false;
			omap_mbox_enable_irq(mq->mbox, IRQ_RX);
		}
		spin_unlock_irq(&mq->lock);
	}
}

/*
 * Mailbox interrupt handler
 */
static void __mbox_tx_interrupt(struct omap_mbox *mbox)
{
	omap_mbox_disable_irq(mbox, IRQ_TX);
	ack_mbox_irq(mbox, IRQ_TX);
	tasklet_schedule(&mbox->txq->tasklet);
}

static void __mbox_rx_interrupt(struct omap_mbox *mbox)
{
	struct omap_mbox_queue *mq = mbox->rxq;
	mbox_msg_t msg;
	int len;

	while (!mbox_fifo_empty(mbox)) {
		if (unlikely(kfifo_avail(&mq->fifo) < sizeof(msg))) {
			omap_mbox_disable_irq(mbox, IRQ_RX);
			mq->full = true;
			goto nomem;
		}

		msg = mbox_fifo_read(mbox);

		len = kfifo_in(&mq->fifo, (unsigned char *)&msg, sizeof(msg));
		WARN_ON(len != sizeof(msg));

		if (mbox->ops->type == OMAP_MBOX_TYPE1)
			break;
	}

	/* no more messages in the fifo. clear IRQ source. */
	ack_mbox_irq(mbox, IRQ_RX);
nomem:
	schedule_work(&mbox->rxq->work);
}

static irqreturn_t mbox_interrupt(int irq, void *p)
{
	struct omap_mbox *mbox = p;

	if (is_mbox_irq(mbox, IRQ_TX))
		__mbox_tx_interrupt(mbox);

	if (is_mbox_irq(mbox, IRQ_RX))
		__mbox_rx_interrupt(mbox);

	return IRQ_HANDLED;
}

static struct omap_mbox_queue *mbox_queue_alloc(struct omap_mbox *mbox,
					void (*work) (struct work_struct *),
					void (*tasklet)(unsigned long))
{
	struct omap_mbox_queue *mq;

	mq = kzalloc(sizeof(struct omap_mbox_queue), GFP_KERNEL);
	if (!mq)
		return NULL;

	spin_lock_init(&mq->lock);

	if (kfifo_alloc(&mq->fifo, mbox_kfifo_size, GFP_KERNEL))
		goto error;

	if (work)
		INIT_WORK(&mq->work, work);

	if (tasklet)
		tasklet_init(&mq->tasklet, tasklet, (unsigned long)mbox);
	return mq;
error:
	kfree(mq);
	return NULL;
}

static void mbox_queue_free(struct omap_mbox_queue *q)
{
	kfifo_free(&q->fifo);
	kfree(q);
}

static int omap_mbox_startup(struct omap_mbox *mbox)
{
	int ret = 0;
	struct omap_mbox_queue *mq;

	mutex_lock(&mbox_configured_lock);

	omap_mbox_enable(mbox);

	if (!mbox_configured++) {
		if (likely(mbox->ops->startup)) {
			ret = mbox->ops->startup(mbox);
			if (unlikely(ret))
				goto fail_startup;
		} else
			goto fail_startup;
	}

	if (!mbox->use_count++) {
		mq = mbox_queue_alloc(mbox, NULL, mbox_tx_tasklet);
		if (!mq) {
			ret = -ENOMEM;
			goto fail_alloc_txq;
		}
		mbox->txq = mq;

		mq = mbox_queue_alloc(mbox, mbox_rx_work, NULL);
		if (!mq) {
			ret = -ENOMEM;
			goto fail_alloc_rxq;
		}
		mbox->rxq = mq;
		mq->mbox = mbox;
		ret = request_irq(mbox->irq, mbox_interrupt, IRQF_SHARED,
							mbox->name, mbox);
		if (unlikely(ret)) {
			pr_err("failed to register mailbox interrupt:%d\n",
									ret);
			goto fail_request_irq;
		} else
			mbox->ops->enable_irq(mbox, IRQ_RX);
	}
	mutex_unlock(&mbox_configured_lock);
	return 0;

fail_request_irq:
	mbox_queue_free(mbox->rxq);
fail_alloc_rxq:
	mbox_queue_free(mbox->txq);
fail_alloc_txq:
	if (mbox->ops->shutdown)
		mbox->ops->shutdown(mbox);
	mbox->use_count--;
fail_startup:
	omap_mbox_disable(mbox);
	mutex_unlock(&mbox_configured_lock);
	return ret;
}

static void omap_mbox_fini(struct omap_mbox *mbox)
{
	mutex_lock(&mbox_configured_lock);

	flush_work_sync(&mbox->rxq->work);
	if (!--mbox->use_count) {
		free_irq(mbox->irq, mbox);
		tasklet_kill(&mbox->txq->tasklet);
		mbox_queue_free(mbox->txq);
		mbox_queue_free(mbox->rxq);
	}

	if (likely(mbox->ops->shutdown)) {
		if (!--mbox_configured)
			mbox->ops->shutdown(mbox);
	}

	omap_mbox_disable(mbox);

	mutex_unlock(&mbox_configured_lock);
}

struct omap_mbox *omap_mbox_get(const char *name, struct notifier_block *nb)
{
	struct omap_mbox *_mbox, *mbox = NULL;
	int i, ret;

	if (!mboxes)
		return ERR_PTR(-EINVAL);

	for (i = 0; (_mbox = mboxes[i]); i++) {
		if (!strcmp(_mbox->name, name)) {
			mbox = _mbox;
			break;
		}
	}

	if (!mbox)
		return ERR_PTR(-ENOENT);

	ret = omap_mbox_startup(mbox);
	if (ret)
		return ERR_PTR(-ENODEV);

	if (nb)
		blocking_notifier_chain_register(&mbox->notifier, nb);

	return mbox;
}
EXPORT_SYMBOL(omap_mbox_get);

void omap_mbox_put(struct omap_mbox *mbox, struct notifier_block *nb)
{
	omap_mbox_fini(mbox);
	if (nb)
		blocking_notifier_chain_unregister(&mbox->notifier, nb);
}
EXPORT_SYMBOL(omap_mbox_put);

int omap_mbox_enable(struct omap_mbox *mbox)
{
	return pm_runtime_get_sync(mbox->dev->parent);
}
EXPORT_SYMBOL(omap_mbox_enable);

int omap_mbox_disable(struct omap_mbox *mbox)
{
	return pm_runtime_put_sync(mbox->dev->parent);
}
EXPORT_SYMBOL(omap_mbox_disable);

static struct class omap_mbox_class = { .name = "mbox", };

int omap_mbox_register(struct device *parent, struct omap_mbox **list)
{
	int ret;
	int i;

	mboxes = list;
	if (!mboxes)
		return -EINVAL;

	for (i = 0; mboxes[i]; i++) {
		struct omap_mbox *mbox = mboxes[i];
		mbox->dev = device_create(&omap_mbox_class,
				parent, 0, mbox, "%s", mbox->name);
		if (IS_ERR(mbox->dev)) {
			ret = PTR_ERR(mbox->dev);
			goto err_out;
		}

		BLOCKING_INIT_NOTIFIER_HEAD(&mbox->notifier);
	}

	pm_runtime_enable(parent);

	return 0;

err_out:
	while (i--)
		device_unregister(mboxes[i]->dev);
	return ret;
}
EXPORT_SYMBOL(omap_mbox_register);

int omap_mbox_unregister(struct device *parent)
{
	int i;

	if (!mboxes)
		return -EINVAL;

	for (i = 0; mboxes[i]; i++)
		device_unregister(mboxes[i]->dev);

	mboxes = NULL;

	pm_runtime_disable(parent);

	return 0;
}
EXPORT_SYMBOL(omap_mbox_unregister);

static int __init omap_mbox_init(void)
{
	int err;

	err = class_register(&omap_mbox_class);
	if (err)
		return err;

	/* kfifo size sanity check: alignment and minimal size */
	mbox_kfifo_size = ALIGN(mbox_kfifo_size, sizeof(mbox_msg_t));
	mbox_kfifo_size = max_t(unsigned int, mbox_kfifo_size,
							sizeof(mbox_msg_t));

	pm_qos_add_request(&mbox_qos_request, PM_QOS_CPU_DMA_LATENCY,
							PM_QOS_DEFAULT_VALUE);

	return 0;
}
subsys_initcall(omap_mbox_init);

static void __exit omap_mbox_exit(void)
{
	pm_qos_remove_request(&mbox_qos_request);
	class_unregister(&omap_mbox_class);
}
module_exit(omap_mbox_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("omap mailbox: interrupt driven messaging");
MODULE_AUTHOR("Toshihiro Kobayashi");
MODULE_AUTHOR("Hiroshi DOYU");
