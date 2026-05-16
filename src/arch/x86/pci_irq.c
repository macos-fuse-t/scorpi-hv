/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "pci_emul.h"
#include "pci_irq.h"

static int pci_irqs[4];

void
pci_irq_init(int intrs[static 4] __unused)
{
	pci_irqs[0] = 16;
	pci_irqs[1] = 17;
	pci_irqs[2] = 18;
	pci_irqs[3] = 19;
}

void
pci_irq_reserve(int irq __unused)
{
}

void
pci_irq_use(int irq __unused)
{
}

int
pirq_irq(int pin)
{
	if (pin < 0 || pin >= 4)
		return (0);

	return (pci_irqs[pin]);
}

uint8_t
pirq_read(int pin)
{
	return (pirq_irq(pin));
}

void
pirq_write(struct vmctx *ctx __unused, int pin __unused, uint8_t val __unused)
{
}

void
pci_irq_assert(struct pci_devinst *pi)
{
	vm_ioapic_assert_irq(pi->pi_vmctx, pi->pi_lintr.irq.ioapic_irq);
}

void
pci_irq_deassert(struct pci_devinst *pi)
{
	vm_ioapic_deassert_irq(pi->pi_vmctx, pi->pi_lintr.irq.ioapic_irq);
}

void
pci_irq_route(struct pci_devinst *pi, struct pci_irq *irq)
{
	if (irq->ioapic_irq == 0) {
		irq->pirq_pin = (pi->pi_slot + pi->pi_lintr.pin - 1) % 4;
		irq->ioapic_irq = pci_irqs[irq->pirq_pin];
	}
}
