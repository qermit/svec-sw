/*
* Copyright (C) 2013 CERN (www.cern.ch)
* Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
* Interrupt support code.
*/

#include <linux/interrupt.h>
#include <linux/fmc.h>
#include <linux/spinlock.h>
#include <vmebus.h>

#include "svec.h"

/* "master" SVEC interrupt handler */
static int svec_irq_handler(void *data)
{
	struct svec_dev *svec = (struct svec_dev *)data;
	int i;
	int rv = -1;
	unsigned long flags;

	svec->irq_count++;

	/* just in case we had an IRQ while messing around with the VIC registers/fmc_handlers */
	spin_lock_irqsave(&svec->irq_lock, flags);

	if (svec->vic)
		rv = svec_vic_irq_dispatch(svec);
	else {
		/* shared irq mode: call all handlers until one of them has dealt with the interrupt */
		for (i = 0; i < SVEC_N_SLOTS; i++) {
			irq_handler_t handler = svec->fmc_handlers[i];

			if (handler) {
				rv = handler(i, svec->fmcs[i]);
				if (rv == IRQ_HANDLED)
					break;
			}
		}
	}

	spin_unlock_irqrestore(&svec->irq_lock, flags);

	if (rv < 0) {
		dev_warn(svec->dev, "spurious VME interrupt, ignoring\n");
		return IRQ_HANDLED;
	}

	return rv;
}

int svec_irq_request(struct fmc_device *fmc, irq_handler_t handler,
		     char *name, int flags)
{
	struct svec_dev *svec = (struct svec_dev *)fmc->carrier_data;
	int rv = 0;

	/* Depending on IRQF_SHARED flag, choose between a VIC and shared IRQ mode */
	if (!flags)
		rv = svec_vic_irq_request(svec, fmc, fmc->irq, handler);
	else if (flags & IRQF_SHARED) {
		spin_lock(&svec->irq_lock);
		svec->fmc_handlers[fmc->slot_id] = handler;
		spin_unlock(&svec->irq_lock);
	} else
		return -EINVAL;

	/* register the master VME handler the first time somebody requests an interrupt */
	if (!rv && !test_bit(SVEC_FLAG_IRQS_REQUESTED, &svec->flags)) {

		rv = vme_request_irq(svec->cfg_cur.interrupt_vector,
				     svec_irq_handler, (void *)svec,
				     svec->name);
		svec->current_vector = svec->cfg_cur.interrupt_vector;

		if (!rv)
			set_bit(SVEC_FLAG_IRQS_REQUESTED, &svec->flags);
	}

	return rv;
}

void svec_irq_ack(struct fmc_device *fmc)
{
	struct svec_dev *svec = (struct svec_dev *)fmc->carrier_data;
	if (svec->vic)
		svec_vic_irq_ack(svec, fmc->irq);
}

int svec_irq_free(struct fmc_device *fmc)
{
	struct svec_dev *svec = (struct svec_dev *)fmc->carrier_data;
	int rv;

	/* freeing a nonexistent interrupt? */
	if (!test_bit(SVEC_FLAG_IRQS_REQUESTED, &svec->flags))
		return -EINVAL;

	if (svec->vic)
		return svec_vic_irq_free(svec, fmc->irq);

	spin_lock(&svec->irq_lock);
	svec->fmc_handlers[fmc->slot_id] = NULL;
	spin_unlock(&svec->irq_lock);

	/* shared IRQ mode: disable VME interrupt when freeing last FMC handler */
	if (!svec->vic && !svec->fmc_handlers[0] && !svec->fmc_handlers[1]) {
		rv = vme_free_irq(svec->current_vector);
		if (rv < 0)
			return rv;

		clear_bit(SVEC_FLAG_IRQS_REQUESTED, &svec->flags);
	}

	return 0;
}

/* cleanup function, disables VME master interrupt when the driver is unloaded */
void svec_irq_exit(struct svec_dev *svec)
{
	if (!test_bit(SVEC_FLAG_IRQS_REQUESTED, &svec->flags))
		return;

	vme_free_irq(svec->current_vector);
	memset(svec->fmc_handlers, 0, sizeof(svec->fmc_handlers));

	if (svec->vic)
		svec_vic_cleanup(svec);
}
