#ifndef RAMDISK_H
#define RAMDISK_H

#pragma warning(disable:4201)  // nameless struct/union warning

#include <ntddk.h>
#include <initguid.h>
#include <ntdddisk.h>

#pragma warning(default:4201)

#include <wdf.h>

#include "forward_progress.h"

#define NT_DEVICE_NAME                  L"\\Device\\Ramdisk"

#define RAMDISK_TAG                     'DmaR'

#define DEFAULT_DISK_SIZE               (1024 * 1024)

typedef struct {
	ULONG disk_size; /* Size in bytes. */
	UCHAR partition_type;
} DISK_INFO;

typedef struct {
	UCHAR          *disk_image;                              /* Pointer to beginning of disk image. */
	DISK_GEOMETRY  disk_geometry;                            /* Drive parameters. */
	DISK_INFO      disk_info;                                /* Disk parameters. */
} DEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION, DeviceGetExtension)

typedef struct {
	DEVICE_EXTENSION *device_extension;
} QUEUE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_EXTENSION, QueueGetExtension)

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;
EVT_WDF_DEVICE_CONTEXT_CLEANUP EvtCleanupCallback;

EVT_WDF_IO_QUEUE_IO_READ EvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE EvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

void query_disk_parameters(__in PWSTR regpath, __in DISK_INFO *disk_info);

void set_disk_geometry(__in DEVICE_EXTENSION *device_extension);
NTSTATUS query_device_name(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length);
NTSTATUS query_unique_id(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length);
NTSTATUS get_length_info(__in DEVICE_EXTENSION *device_extension, __in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length);
NTSTATUS get_hotplug_info(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length);

BOOLEAN check_parameters(__in DEVICE_EXTENSION *device_extension, __in LARGE_INTEGER offset, __in size_t length);

EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtForwardProgressRequestCleanup;
EVT_WDF_OBJECT_CONTEXT_DESTROY EvtForwardProgressRequestDestroy;
EVT_WDF_IO_WDM_IRP_FOR_FORWARD_PROGRESS EvtIoWdmIrpForForwardProgress;
EVT_WDF_IO_ALLOCATE_RESOURCES_FOR_RESERVED_REQUEST EvtIoAllocateResourcesForReservedRequest;
EVT_WDF_IO_ALLOCATE_REQUEST_RESOURCES EvtIoAllocateResources;

#endif /* RAMDISK_H */
