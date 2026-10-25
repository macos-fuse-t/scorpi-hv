/* SPDX-License-Identifier: BSD-2-Clause */
#include <nthalext.h>

/* Firmware CSRT descriptor, revision 1. STIMER0 remains owned by Windows'
 * built-in clock plugin; this extension exclusively owns STIMER1.
 */
typedef struct {
    CSRT_RESOURCE_DESCRIPTOR_HEADER Header;
    ULONG TimerIndex;
    ULONG Interrupt;
    ULONG Frequency;
} SCORPI_TIMER_DESCRIPTOR;

typedef struct {
    ULONG ConfigRegister;
    ULONG Interrupt;
} SCORPI_TIMER_DATA;

extern ULONG ScorpiSetVpRegister(ULONG Name, ULONGLONG Value);
extern ULONGLONG ScorpiGetVpRegister(ULONG Name);

static NTSTATUS TimerInitialize(PVOID Context)
{
    SCORPI_TIMER_DATA *data = Context;
    if (ScorpiGetVpRegister(0x90006) != 0x31237648 ||
        ScorpiSetVpRegister(data->ConfigRegister, 0) != 0 ||
        ScorpiSetVpRegister(data->ConfigRegister + 1, 0) != 0)
        return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

static NTSTATUS TimerArm(PVOID Context, TIMER_MODE Mode, ULONGLONG Ticks)
{
    SCORPI_TIMER_DATA *data = Context;
    if (Mode != TimerModePeriodic || Ticks == 0 || Ticks > MAXULONG)
        return STATUS_INVALID_PARAMETER;
    /* Disable before changing count or configuration. Every VP programs its
     * own complete configuration; no boot-CPU-only cached initialization.
     */
    if (ScorpiSetVpRegister(data->ConfigRegister, 0) != 0 ||
        ScorpiSetVpRegister(data->ConfigRegister + 1, Ticks) != 0 ||
        ScorpiSetVpRegister(data->ConfigRegister,
            0x1003 | ((ULONGLONG)data->Interrupt << 4)) != 0)
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static VOID TimerStop(PVOID Context)
{
    SCORPI_TIMER_DATA *data = Context;
    (VOID)ScorpiSetVpRegister(data->ConfigRegister + 1, 0);
}

static VOID TimerAcknowledge(PVOID Context)
{
    /* Edge-triggered PPI: GIC acknowledgment consumes the pending edge. */
    UNREFERENCED_PARAMETER(Context);
}

NTSTATUS AddResourceGroup(ULONG Handle, PCSRT_RESOURCE_GROUP_HEADER Group)
{
    PCSRT_RESOURCE_DESCRIPTOR_HEADER header = NULL;
    while ((header = GetNextResourceDescriptor(Handle, Group, header,
                CSRT_RD_TYPE_TIMER, CSRT_RD_SUBTYPE_TIMER, CSRT_RD_UID_ANY)) != NULL) {
        SCORPI_TIMER_DESCRIPTOR *desc = (SCORPI_TIMER_DESCRIPTOR *)header;
        TIMER_INITIALIZATION_BLOCK timer = {0};
        SCORPI_TIMER_DATA data;
        NTSTATUS status;
        if (Group->Revision != 1 || header->Length != sizeof(*desc) ||
            desc->TimerIndex != 1 || desc->Interrupt < 16 ||
            desc->Interrupt > 31 || desc->Frequency != 10000000)
            return STATUS_INVALID_PARAMETER;
        data.ConfigRegister = 0xb0000 + desc->TimerIndex * 2;
        data.Interrupt = desc->Interrupt;
        INITIALIZE_TIMER_HEADER(&timer);
        timer.FunctionTable.Initialize = TimerInitialize;
        timer.FunctionTable.ArmTimer = TimerArm;
        timer.FunctionTable.Stop = TimerStop;
        timer.FunctionTable.AcknowledgeInterrupt = TimerAcknowledge;
        timer.InternalData = &data;
        timer.InternalDataSize = sizeof(data);
        timer.CounterBitWidth = 32;
        timer.CounterFrequency = desc->Frequency;
        timer.MaxDivisor = 1;
        timer.Capabilities = TIMER_PER_PROCESSOR | TIMER_PERIODIC_CAPABLE |
            TIMER_GENERATES_LINE_BASED_INTERRUPTS;
        timer.Interrupt.Gsi = (LONG)desc->Interrupt;
        timer.Interrupt.Mode = Latched;
        timer.Interrupt.Polarity = InterruptActiveHigh;
        /* This is a hypervisor-provided per-VP synthetic timer. Windows
         * rejects per-processor TimerUnknown registrations; identifying the
         * actual timer family preserves its per-CPU initialization rules.
         */
        timer.KnownType = TimerHypervisor;
        timer.Identifier = header->Uid;
        status = RegisterResourceDescriptor(Handle, Group, header, &timer);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}
