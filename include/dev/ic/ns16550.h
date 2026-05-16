/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1991 The Regents of the University of California.
 * All rights reserved.
 */

#ifndef _DEV_IC_NS16550_H_
#define	_DEV_IC_NS16550_H_

#define	com_data	0
#define	REG_DATA	com_data

#define	com_ier		1
#define	REG_IER		com_ier
#define	IER_ERXRDY	0x1
#define	IER_ETXRDY	0x2
#define	IER_ERLS	0x4
#define	IER_EMSC	0x8

#define	com_iir		2
#define	REG_IIR		com_iir
#define	IIR_RXTOUT	0xc
#define	IIR_RLS		0x6
#define	IIR_TXRDY	0x2
#define	IIR_NOPEND	0x1
#define	IIR_MLSC	0x0
#define	IIR_FIFO_MASK	0xc0

#define	com_lcr		3
#define	REG_LCR		com_lcr
#define	LCR_DLAB	0x80

#define	com_mcr		4
#define	REG_MCR		com_mcr
#define	MCR_LOOPBACK	0x10
#define	MCR_RTS		0x02
#define	MCR_DTR		0x01

#define	com_lsr		5
#define	REG_LSR		com_lsr
#define	LSR_TEMT	0x40
#define	LSR_THRE	0x20
#define	LSR_OE		0x02
#define	LSR_RXRDY	0x01

#define	com_msr		6
#define	REG_MSR		com_msr
#define	MSR_DCD		0x80
#define	MSR_RI		0x40
#define	MSR_DSR		0x20
#define	MSR_CTS		0x10
#define	MSR_DDCD	0x08
#define	MSR_TERI	0x04
#define	MSR_DDSR	0x02
#define	MSR_DCTS	0x01

#define	com_dll		0
#define	com_dlm		1
#define	REG_DLL		com_dll
#define	REG_DLH		com_dlm

#define	com_scr		7

#define	com_fcr		2
#define	REG_FCR		com_fcr
#define	FCR_ENABLE	0x01
#define	FCR_RCV_RST	0x02
#define	FCR_DMA		0x08

#endif /* _DEV_IC_NS16550_H_ */
