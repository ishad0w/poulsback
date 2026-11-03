// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Same private production engine lock for router, writer and CPU fixture. */
#include <linux/spinlock.h>
#include "sgx535_engine_irq.h"

static DEFINE_SPINLOCK(engine_irq_lock);

unsigned long sgx535_engine_irq_lock(void)
{
	unsigned long flags;
	spin_lock_irqsave(&engine_irq_lock, flags);
	return flags;
}

void sgx535_engine_irq_unlock(unsigned long flags)
{
	spin_unlock_irqrestore(&engine_irq_lock, flags);
}
