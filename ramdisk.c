#include "ramdisk.h"
#include <mountdev.h>

/******************************************************************************
 ******************************************************************************
 **                                                                          **
 ** Assign text sections for each routine.                                   **
 **                                                                          **
 ******************************************************************************
 ******************************************************************************/
#ifdef ALLOC_PRAGMA
	#pragma alloc_text(INIT, DriverEntry)
	#pragma alloc_text(PAGE, EvtDriverDeviceAdd)
	#pragma alloc_text(PAGE, EvtCleanupCallback)
	#pragma alloc_text(PAGE, query_disk_parameters)
	#pragma alloc_text(PAGE, set_disk_geometry)
	#pragma alloc_text(PAGE, query_device_name)
	#pragma alloc_text(PAGE, query_unique_id)
	#pragma alloc_text(PAGE, get_length_info)
	#pragma alloc_text(PAGE, get_hotplug_info)
#endif

NTSTATUS DriverEntry(__in DRIVER_OBJECT *driver, __in UNICODE_STRING *regpath)
{
	WDF_DRIVER_CONFIG config;

	KdPrint(("Windows Ramdisk Driver.\n"));
	KdPrint(("Built %s %s.\n", __DATE__, __TIME__));

	WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);

	return WdfDriverCreate(driver, regpath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS EvtDriverDeviceAdd(__in WDFDRIVER driver, __in PWDFDEVICE_INIT device_init)
{
	DISK_INFO disk_info;
	UCHAR *disk_image;
	WDFDEVICE device;
	WDFQUEUE queue;
	WDF_OBJECT_ATTRIBUTES device_attributes;
	WDF_OBJECT_ATTRIBUTES queue_attributes;
	WDF_IO_QUEUE_CONFIG io_queue_config;
	DEVICE_EXTENSION *device_extension;
	QUEUE_EXTENSION *queue_extension;
	NTSTATUS status;

	DECLARE_CONST_UNICODE_STRING(nt_name, NT_DEVICE_NAME);

	PAGED_CODE();

	/* Get the disk parameters from the registry. */
	query_disk_parameters(WdfDriverGetRegistryPath(driver), &disk_info);

	/* Allocate memory for the disk image. */
	if ((disk_image = ExAllocatePoolWithTag(NonPagedPool, disk_info.disk_size, RAMDISK_TAG)) == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	/* Assign a device name. */
	status = WdfDeviceInitAssignName(device_init, &nt_name);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(disk_image, RAMDISK_TAG);
		return status;
	}

	WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_DISK);
	WdfDeviceInitSetIoType(device_init, WdfDeviceIoDirect);
	WdfDeviceInitSetExclusive(device_init, FALSE);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, DEVICE_EXTENSION);
	device_attributes.EvtCleanupCallback = EvtCleanupCallback;

	/* Create device object. */
	status = WdfDeviceCreate(&device_init, &device_attributes, &device);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(disk_image, RAMDISK_TAG);
		return status;
	}

	/* Create a device interface. */
	status = WdfDeviceCreateDeviceInterface(device, &MOUNTDEV_MOUNTED_DEVICE_GUID, NULL);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(disk_image, RAMDISK_TAG);
		return status;
	}

	/* Configure the default queue. */
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&io_queue_config, WdfIoQueueDispatchSequential);

	io_queue_config.EvtIoRead = EvtIoRead;
	io_queue_config.EvtIoWrite = EvtIoWrite;
	io_queue_config.EvtIoDeviceControl = EvtIoDeviceControl;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queue_attributes, QUEUE_EXTENSION);

	/* Create I/O queue. */
	status = WdfIoQueueCreate(device, &io_queue_config, &queue_attributes, &queue);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(disk_image, RAMDISK_TAG);
		return status;
	}

	status = SetForwardProgressOnQueue(queue);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(disk_image, RAMDISK_TAG);
		return status;
	}

	device_extension = DeviceGetExtension(device);
	queue_extension = QueueGetExtension(queue);

	queue_extension->device_extension = device_extension;

	/* Set up the device extension. */
	device_extension->disk_image = disk_image;

	device_extension->disk_info.disk_size = disk_info.disk_size;

	set_disk_geometry(device_extension);

	return STATUS_SUCCESS;
}

void EvtCleanupCallback(__in WDFOBJECT device)
{
	DEVICE_EXTENSION *device_extension;

	PAGED_CODE();

	device_extension = DeviceGetExtension(device);

	if (device_extension->disk_image) {
		ExFreePoolWithTag(device_extension->disk_image, RAMDISK_TAG);
	}
}

void EvtIoRead(__in WDFQUEUE queue, __in WDFREQUEST request, __in size_t length)
{
	WDF_REQUEST_PARAMETERS parameters;
	DEVICE_EXTENSION *device_extension;
	LARGE_INTEGER offset;
	WDFMEMORY hMemory;
	NTSTATUS status;

	__analysis_assume(length > 0);

	/* Retrieve the parameters associated with the request. */
	WDF_REQUEST_PARAMETERS_INIT(&parameters);
	WdfRequestGetParameters(request, &parameters);

	offset.QuadPart = parameters.Parameters.Read.DeviceOffset;

	device_extension = QueueGetExtension(queue)->device_extension;

	if (!check_parameters(device_extension, offset, length)) {
		WdfRequestCompleteWithInformation(request, STATUS_INVALID_PARAMETER, (ULONG_PTR) length);
		return;
	}

	/* Retrieve a handle to the memory object that represents the request's output buffer. */
	status = WdfRequestRetrieveOutputMemory(request, &hMemory);
	if (!NT_SUCCESS(status)){
		WdfRequestCompleteWithInformation(request, status, (ULONG_PTR) length);
		return;
	}

	/* Copy from the disk image to the memory object's buffer. */
	status = WdfMemoryCopyFromBuffer(hMemory, 0, device_extension->disk_image + offset.LowPart, length);

	WdfRequestCompleteWithInformation(request, status, (ULONG_PTR) length);
}

void EvtIoWrite(__in WDFQUEUE queue, __in WDFREQUEST request, __in size_t length)
{
	WDF_REQUEST_PARAMETERS parameters;
	DEVICE_EXTENSION *device_extension;
	LARGE_INTEGER offset;
	WDFMEMORY hMemory;
	NTSTATUS status;

	__analysis_assume(length > 0);

	/* Retrieve the parameters associated with the request. */
	WDF_REQUEST_PARAMETERS_INIT(&parameters);
	WdfRequestGetParameters(request, &parameters);

	offset.QuadPart = parameters.Parameters.Write.DeviceOffset;

	device_extension = QueueGetExtension(queue)->device_extension;

	if (!check_parameters(device_extension, offset, length)) {
		WdfRequestCompleteWithInformation(request, STATUS_INVALID_PARAMETER, (ULONG_PTR) length);
		return;
	}

	/* Retrieve a handle to the memory object that represents the request's input buffer. */
	status = WdfRequestRetrieveInputMemory(request, &hMemory);
	if (!NT_SUCCESS(status)){
		WdfRequestCompleteWithInformation(request, status, (ULONG_PTR) length);
		return;
	}

	/* Copy from the memory object's buffer to the disk image. */
	status = WdfMemoryCopyToBuffer(hMemory, 0, device_extension->disk_image + offset.LowPart, length);

	WdfRequestCompleteWithInformation(request, status, (ULONG_PTR) length);
}

void EvtIoDeviceControl(__in WDFQUEUE queue, __in WDFREQUEST request, __in size_t output_buffer_length, __in size_t input_buffer_length, __in ULONG code)
{
	DEVICE_EXTENSION *device_extension;
	WDF_REQUEST_PARAMETERS parameters;
	PARTITION_INFORMATION *partition_information;
	SET_PARTITION_INFORMATION *set_partition_information;
	DISK_GEOMETRY *disk_geometry;
	ULONG_PTR information;
	size_t length;
	NTSTATUS status;

	/* Retrieve the parameters associated with the request. */
	WDF_REQUEST_PARAMETERS_INIT(&parameters);
	WdfRequestGetParameters(request, &parameters);

	device_extension = QueueGetExtension(queue)->device_extension;

	switch (code) {
		case IOCTL_DISK_GET_PARTITION_INFO:
			/* If the buffer is too small... */
			if (output_buffer_length < sizeof(PARTITION_INFORMATION)) {
				WdfRequestCompleteWithInformation(request, STATUS_BUFFER_TOO_SMALL, sizeof(PARTITION_INFORMATION));
				return;
			}

			status = WdfRequestRetrieveOutputBuffer(request, sizeof(PARTITION_INFORMATION), &partition_information, NULL);
			if (!NT_SUCCESS(status)) {
				WdfRequestCompleteWithInformation(request, status, 0);
				return;
			}

			partition_information->StartingOffset.QuadPart = 0;
			partition_information->PartitionLength.QuadPart = device_extension->disk_info.disk_size;
			partition_information->HiddenSectors = (ULONG) (1L);
			partition_information->PartitionNumber = (ULONG) (-1L);
			partition_information->PartitionType = device_extension->disk_info.partition_type;
			partition_information->BootIndicator = FALSE;
			partition_information->RecognizedPartition = TRUE;
			partition_information->RewritePartition = FALSE;

			information = sizeof(PARTITION_INFORMATION);
			break;
		case IOCTL_DISK_SET_PARTITION_INFO:
			/* If the buffer is too small... */
			if (input_buffer_length < sizeof(SET_PARTITION_INFORMATION)) {
				WdfRequestCompleteWithInformation(request, STATUS_BUFFER_TOO_SMALL, sizeof(SET_PARTITION_INFORMATION));
				return;
			}

			status = WdfRequestRetrieveInputBuffer(request, sizeof(SET_PARTITION_INFORMATION), &set_partition_information, NULL);
			if (!NT_SUCCESS(status)) {
				WdfRequestCompleteWithInformation(request, status, 0);
				return;
			}

			device_extension->disk_info.partition_type = set_partition_information->PartitionType;

			KdPrint(("IOCTL_DISK_SET_PARTITION_INFO: 0x%x.\n", device_extension->disk_info.partition_type));

			information = 0;
			break;
		case IOCTL_DISK_GET_DRIVE_GEOMETRY:
			/* If the buffer is too small... */
			if (output_buffer_length < sizeof(DISK_GEOMETRY)) {
				WdfRequestCompleteWithInformation(request, STATUS_BUFFER_TOO_SMALL, sizeof(DISK_GEOMETRY));
				return;
			}

			status = WdfRequestRetrieveOutputBuffer(request, sizeof(DISK_GEOMETRY), &disk_geometry, NULL);
			if (!NT_SUCCESS(status)) {
				WdfRequestCompleteWithInformation(request, status, 0);
				return;
			}

			RtlCopyMemory(disk_geometry, &device_extension->disk_geometry, sizeof(DISK_GEOMETRY));

			information = sizeof(DISK_GEOMETRY);
			break;
		case IOCTL_DISK_GET_MEDIA_TYPES:
		case IOCTL_STORAGE_GET_MEDIA_TYPES:
			/* If the buffer is too small... */
			if (output_buffer_length < sizeof(DISK_GEOMETRY)) {
				WdfRequestCompleteWithInformation(request, STATUS_BUFFER_TOO_SMALL, sizeof(DISK_GEOMETRY));
				return;
			}

			status = WdfRequestRetrieveOutputBuffer(request, sizeof(DISK_GEOMETRY), &disk_geometry, NULL);
			if (!NT_SUCCESS(status)) {
				WdfRequestCompleteWithInformation(request, status, 0);
				return;
			}

			RtlCopyMemory(disk_geometry, &device_extension->disk_geometry, sizeof(DISK_GEOMETRY));

			information = sizeof(DISK_GEOMETRY);
			break;
		case IOCTL_DISK_CHECK_VERIFY: /* The media has not changed. */
		case IOCTL_STORAGE_CHECK_VERIFY: /* The media has not changed. */
		case IOCTL_DISK_IS_WRITABLE:
			status = STATUS_SUCCESS;
			information = 0;
			break;
		case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
			status = query_device_name(request, parameters, &length);
			information = length;
			break;
		case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
			status = query_unique_id(request, parameters, &length);
			information = length;
			break;
		case IOCTL_DISK_MEDIA_REMOVAL:
		case IOCTL_STORAGE_MEDIA_REMOVAL:
			/* If the buffer is too small... */
			if (input_buffer_length < sizeof(BOOLEAN)) {
				WdfRequestCompleteWithInformation(request, STATUS_INVALID_DEVICE_REQUEST, sizeof(BOOLEAN));
				return;
			}

			status = STATUS_SUCCESS;
			information = 0;
			break;
		case IOCTL_DISK_GET_LENGTH_INFO:
			status = get_length_info(device_extension, request, parameters, &length);
			information = length;
			break;
		case IOCTL_STORAGE_GET_HOTPLUG_INFO:
			status = get_hotplug_info(request, parameters, &length);
			information = length;
			break;
		default:
			KdPrint(("IOCTL code: 0x%x\n", code));

			status = STATUS_INVALID_DEVICE_REQUEST;
			information = 0;
	}

	WdfRequestCompleteWithInformation(request, status, information);
}

void query_disk_parameters(__in PWSTR regpath, __in DISK_INFO *disk_info)
{
	RTL_QUERY_REGISTRY_TABLE query_table[3];
	DISK_INFO default_disk_info;

	PAGED_CODE();

	ASSERT(regpath);

	/* Set the default value. */
	default_disk_info.disk_size = DEFAULT_DISK_SIZE;

	/* Setup the query table. */
	RtlZeroMemory(query_table, sizeof(query_table));

	query_table[0].Flags         = RTL_QUERY_REGISTRY_SUBKEY;
	query_table[0].Name          = L"Parameters";

	/* Disk parameters. */
#ifdef RTL_QUERY_REGISTRY_TYPECHECK
	query_table[1].Flags         = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
	query_table[1].DefaultType   = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
#else
	query_table[1].Flags         = RTL_QUERY_REGISTRY_DIRECT;
	query_table[1].DefaultType   = REG_DWORD;
#endif

	query_table[1].Name          = L"DiskSize";
	query_table[1].EntryContext  = &disk_info->disk_size;
	query_table[1].DefaultData   = &default_disk_info.disk_size;
	query_table[1].DefaultLength = sizeof(ULONG);

	if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL, regpath, query_table, NULL, NULL))) {
		/* Use default value. */
		disk_info->disk_size = default_disk_info.disk_size;
	}

	KdPrint(("DiskSize = 0x%lx.\n", disk_info->disk_size));
}

void set_disk_geometry(__in DEVICE_EXTENSION *device_extension)
{
	PAGED_CODE();

	ASSERT(device_extension->disk_image);

	device_extension->disk_geometry.BytesPerSector = 512;
	device_extension->disk_geometry.SectorsPerTrack = 32;
	device_extension->disk_geometry.TracksPerCylinder = 2;

	/* Calculate number of cylinders. */
	device_extension->disk_geometry.Cylinders.QuadPart = device_extension->disk_info.disk_size
														/ device_extension->disk_geometry.BytesPerSector
														/ device_extension->disk_geometry.SectorsPerTrack
														/ device_extension->disk_geometry.TracksPerCylinder;

	device_extension->disk_geometry.MediaType = FixedMedia;

	KdPrint(("Cylinders: %lld.\n", device_extension->disk_geometry.Cylinders.QuadPart));
	KdPrint(("TracksPerCylinder: %lu.\n", device_extension->disk_geometry.TracksPerCylinder));
	KdPrint(("SectorsPerTrack: %lu.\n", device_extension->disk_geometry.SectorsPerTrack));
	KdPrint(("BytesPerSector: %lu.\n", device_extension->disk_geometry.BytesPerSector));
}

BOOLEAN check_parameters(__in DEVICE_EXTENSION *device_extension, __in LARGE_INTEGER offset, __in size_t length)
{
	if ((offset.QuadPart < 0) || ((ULONGLONG) offset.QuadPart + length > device_extension->disk_info.disk_size) || \
	(length & (device_extension->disk_geometry.BytesPerSector - 1))) {
		KdPrint(("Error invalid parameter.\nByteOffset: %I64x.\nLength: %u.\n", offset.QuadPart, length));
		return FALSE;
	}

	return TRUE;
}

NTSTATUS query_device_name(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length)
{
	MOUNTDEV_NAME *name;
	NTSTATUS status;

	DECLARE_CONST_UNICODE_STRING(nt_name, NT_DEVICE_NAME);

	PAGED_CODE();

	/* If the buffer is too small... */
	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
		*length = sizeof(MOUNTDEV_NAME);
		return STATUS_BUFFER_TOO_SMALL;
	}

	status = WdfRequestRetrieveOutputBuffer(request, sizeof(MOUNTDEV_NAME), &name, NULL);
	if (!NT_SUCCESS(status)) {
		*length = 0;
		return status;
	}

	RtlZeroMemory(name, sizeof(MOUNTDEV_NAME));
	name->NameLength = nt_name.Length;

	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(USHORT) + nt_name.Length) {
		*length = sizeof(MOUNTDEV_NAME);
		return STATUS_BUFFER_OVERFLOW;
	}

	RtlCopyMemory(name->Name, nt_name.Buffer, nt_name.Length);

	*length = sizeof(USHORT) + nt_name.Length;

	return STATUS_SUCCESS;
}

NTSTATUS query_unique_id(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length)
{
	MOUNTDEV_UNIQUE_ID *unique_id;
	NTSTATUS status;

	DECLARE_CONST_UNICODE_STRING(nt_name, NT_DEVICE_NAME);

	PAGED_CODE();

	/* If the buffer is too small... */
	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
		*length = sizeof(MOUNTDEV_UNIQUE_ID);
		return STATUS_BUFFER_TOO_SMALL;
	}

	status = WdfRequestRetrieveOutputBuffer(request, sizeof(MOUNTDEV_UNIQUE_ID), &unique_id, NULL);
	if (!NT_SUCCESS(status)) {
		*length = 0;
		return status;
	}

	RtlZeroMemory(unique_id, sizeof(MOUNTDEV_UNIQUE_ID));
	unique_id->UniqueIdLength = nt_name.Length;

	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(USHORT) + nt_name.Length) {
		*length = sizeof(MOUNTDEV_UNIQUE_ID);
		return STATUS_BUFFER_OVERFLOW;
	}

	RtlCopyMemory(unique_id->UniqueId, nt_name.Buffer, nt_name.Length);

	*length = sizeof(USHORT) + nt_name.Length;

	return STATUS_SUCCESS;
}

NTSTATUS get_length_info(__in DEVICE_EXTENSION *device_extension, __in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length)
{
	GET_LENGTH_INFORMATION *length_information;
	NTSTATUS status;

	PAGED_CODE();

	/* If the buffer is too small... */
	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(GET_LENGTH_INFORMATION)) {
		*length = sizeof(GET_LENGTH_INFORMATION);
		return STATUS_BUFFER_TOO_SMALL;
	}

	status = WdfRequestRetrieveOutputBuffer(request, sizeof(GET_LENGTH_INFORMATION), &length_information, NULL);
	if (!NT_SUCCESS(status)) {
		*length = 0;
		return status;
	}

	length_information->Length.QuadPart = device_extension->disk_info.disk_size;

	*length = sizeof(GET_LENGTH_INFORMATION);

	return STATUS_SUCCESS;
}

NTSTATUS get_hotplug_info(__in WDFREQUEST request, __in WDF_REQUEST_PARAMETERS parameters, __out size_t *length)
{
	STORAGE_HOTPLUG_INFO *storage_hotplug_info;
	NTSTATUS status;

	PAGED_CODE();

	/* If the buffer is too small... */
	if (parameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(STORAGE_HOTPLUG_INFO)) {
		*length = sizeof(STORAGE_HOTPLUG_INFO);
		return STATUS_BUFFER_TOO_SMALL;
	}

	status = WdfRequestRetrieveOutputBuffer(request, sizeof(STORAGE_HOTPLUG_INFO), &storage_hotplug_info, NULL);
	if (!NT_SUCCESS(status)) {
		*length = 0;
		return status;
	}

	storage_hotplug_info->Size = sizeof(STORAGE_HOTPLUG_INFO);

	storage_hotplug_info->MediaRemovable = FALSE;
	storage_hotplug_info->MediaHotplug = FALSE;
	storage_hotplug_info->DeviceHotplug = FALSE;
	storage_hotplug_info->WriteCacheEnableOverride = 0;

	*length = sizeof(STORAGE_HOTPLUG_INFO);

	return STATUS_SUCCESS;
}
