#include <ntddk.h>
#include "..\\shared\\n4tey_protocol.h"

/* IoCreateDriver is exported by ntoskrnl but not declared in the WDK headers.
   kdmapper calls DriverEntry with null parameters, so we use the same pattern
   as the km/ driver: a parameterless DriverEntry stub that calls IoCreateDriver,
   which then invokes driver_main with proper DriverObject / RegistryPath. */
NTKERNELAPI NTSTATUS IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction);

#define N4TEY_DEVICE_NAME L"\\Device\\N4TEYW4RE"
#define N4TEY_DOS_NAME    L"\\DosDevices\\N4TEYW4RE"

static volatile LONG64 g_RequestCount = 0;
static volatile LONG g_OpenHandles = 0;
static ULONGLONG g_StartTime = 0;
static KSPIN_LOCK g_MessageLock;
static ULONG g_MessageLength = 0;
static UCHAR g_Message[N4TEY_MESSAGE_CAPACITY];

static NTSTATUS CompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    const UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
    if (major == IRP_MJ_CREATE)
        InterlockedIncrement(&g_OpenHandles);
    else if (major == IRP_MJ_CLOSE)
        InterlockedDecrement(&g_OpenHandles);
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    InterlockedIncrement64(&g_RequestCount);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (code)
    {
    case IOCTL_N4TEY_PING:
        if (inputLength < sizeof(N4TEY_PING_REQUEST) || outputLength < sizeof(N4TEY_PING_RESPONSE))
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (((N4TEY_PING_REQUEST*)buffer)->Protocol != N4TEY_PROTOCOL_VERSION)
            return CompleteIrp(Irp, STATUS_REVISION_MISMATCH, 0);
        ((N4TEY_PING_RESPONSE*)buffer)->Protocol = N4TEY_PROTOCOL_VERSION;
        ((N4TEY_PING_RESPONSE*)buffer)->DriverVersion = N4TEY_DRIVER_VERSION;
        ((N4TEY_PING_RESPONSE*)buffer)->Nonce = KeQueryInterruptTime();
        return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(N4TEY_PING_RESPONSE));

    case IOCTL_N4TEY_GET_STATUS:
        if (outputLength < sizeof(N4TEY_STATUS_RESPONSE))
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            N4TEY_STATUS_RESPONSE* response = (N4TEY_STATUS_RESPONSE*)buffer;
            response->Protocol = N4TEY_PROTOCOL_VERSION;
            response->DriverVersion = N4TEY_DRIVER_VERSION;
            response->Uptime100ns = KeQueryInterruptTime() - g_StartTime;
            response->RequestCount = (ULONGLONG)InterlockedCompareExchange64(&g_RequestCount, 0, 0);
            response->OpenHandles = (ULONG)InterlockedCompareExchange(&g_OpenHandles, 0, 0);
            KIRQL irql;
            KeAcquireSpinLock(&g_MessageLock, &irql);
            response->MessageLength = g_MessageLength;
            KeReleaseSpinLock(&g_MessageLock, irql);
            return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(*response));
        }

    case IOCTL_N4TEY_ECHO:
        if (inputLength < sizeof(N4TEY_ECHO_PACKET) || outputLength < sizeof(N4TEY_ECHO_PACKET))
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            N4TEY_ECHO_PACKET* packet = (N4TEY_ECHO_PACKET*)buffer;
            if (packet->Protocol != N4TEY_PROTOCOL_VERSION || packet->Length > N4TEY_MESSAGE_CAPACITY)
                return CompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
            return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(*packet));
        }

    case IOCTL_N4TEY_SET_MESSAGE:
        if (inputLength < sizeof(N4TEY_MESSAGE_PACKET))
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            N4TEY_MESSAGE_PACKET* packet = (N4TEY_MESSAGE_PACKET*)buffer;
            if (packet->Protocol != N4TEY_PROTOCOL_VERSION || packet->Length > N4TEY_MESSAGE_CAPACITY)
                return CompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
            KIRQL irql;
            KeAcquireSpinLock(&g_MessageLock, &irql);
            g_MessageLength = packet->Length;
            if (g_MessageLength)
                RtlCopyMemory(g_Message, packet->Data, g_MessageLength);
            KeReleaseSpinLock(&g_MessageLock, irql);
            return CompleteIrp(Irp, STATUS_SUCCESS, 0);
        }

    case IOCTL_N4TEY_GET_MESSAGE:
        if (outputLength < sizeof(N4TEY_MESSAGE_PACKET))
            return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            N4TEY_MESSAGE_PACKET* packet = (N4TEY_MESSAGE_PACKET*)buffer;
            packet->Protocol = N4TEY_PROTOCOL_VERSION;
            KIRQL irql;
            KeAcquireSpinLock(&g_MessageLock, &irql);
            packet->Length = g_MessageLength;
            if (packet->Length)
                RtlCopyMemory(packet->Data, g_Message, packet->Length);
            KeReleaseSpinLock(&g_MessageLock, irql);
            return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(*packet));
        }

    default:
        return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, N4TEY_DOS_NAME);
    IoDeleteSymbolicLink(&dosName);
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);
}

/* Real initialisation — called by IoCreateDriver with valid pointers. */
static NTSTATUS driver_main(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING deviceName;
    UNICODE_STRING dosName;
    PDEVICE_OBJECT deviceObject = NULL;

    RtlInitUnicodeString(&deviceName, N4TEY_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, N4TEY_DOS_NAME);

    NTSTATUS status = IoCreateDevice(DriverObject, 0, &deviceName,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &deviceObject);
    if (!NT_SUCCESS(status))
        return status;

    status = IoCreateSymbolicLink(&dosName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    KeInitializeSpinLock(&g_MessageLock);
    g_StartTime = KeQueryInterruptTime();
    g_MessageLength = 0;
    RtlZeroMemory(g_Message, sizeof(g_Message));

    DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    DriverObject->DriverUnload = DriverUnload;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* kdmapper entry point — parameters are NULL when manually mapped.
   IoCreateDriver allocates a real DRIVER_OBJECT and calls driver_main. */
NTSTATUS DriverEntry(void)
{
    UNICODE_STRING driverName;
    RtlInitUnicodeString(&driverName, L"\\Driver\\N4TEYW4RE");
    return IoCreateDriver(&driverName, &driver_main);
}
