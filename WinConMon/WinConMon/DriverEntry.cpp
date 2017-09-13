#include <ntddk.h>
#include <ntstatus.h>

#define WINCONMON_DEVICE_NAME L"\\Device\\WinConMon"
#define WINCONMON_DOSDEVICE_NAME L"\\DosDevices\\WinConMon"
#define CONDRV_DRIVER_NAME L"\\Driver\\Condrv"
#define ALLOCATE_TAG 'DMCW'

typedef NTSTATUS (*Fn_ObReferenceObjectByName)(
	PUNICODE_STRING,
	ULONG,
	PACCESS_STATE,
	ACCESS_MASK,
	POBJECT_TYPE,
	KPROCESSOR_MODE,
	PVOID,
	PVOID*
);

Fn_ObReferenceObjectByName fnObReferenceObjectByName = NULL;

typedef struct _WINCONMON_DEVICE_EXTENSION
{
	PDEVICE_OBJECT AttachedToDeviceObject;
} WINCONMON_DEVICE_EXTENSION, *LPWINCONMON_DEVICE_EXTENSION;

typedef struct _CONSOLE_IO_MSG_T
{
	ULONG handle;
	ULONG unk;
	PVOID buffer;
	ULONG buffLength;
	ULONG type;
} CONSOLE_IO_MSG_T, *LPCONSOLE_IO_MSG_T;

FAST_IO_DISPATCH *g_FastIoDispatch = NULL;

NTSTATUS
WinConMonDefaultDispatch(
	_In_ struct _DEVICE_OBJECT *DeviceObject,
	_Inout_ struct _IRP *Irp
)
{
	// Passthrough to lower device
	LPWINCONMON_DEVICE_EXTENSION devExt = (LPWINCONMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(devExt->AttachedToDeviceObject, Irp);
}

BOOLEAN
WinConMonFastIoDeviceControl(
	_In_ struct _FILE_OBJECT *FileObject,
	_In_ BOOLEAN Wait,
	_In_opt_ PVOID InputBuffer,
	_In_ ULONG InputBufferLength,
	_Out_opt_ PVOID OutputBuffer,
	_In_ ULONG OutputBufferLength,
	_In_ ULONG IoControlCode,
	_Out_ PIO_STATUS_BLOCK IoStatus,
	_In_ struct _DEVICE_OBJECT *DeviceObject
)
{
	LPWINCONMON_DEVICE_EXTENSION devExt = (LPWINCONMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PFAST_IO_DISPATCH fastIoDispatch = devExt->AttachedToDeviceObject->DriverObject->FastIoDispatch;
	BOOLEAN bResult = fastIoDispatch->FastIoDeviceControl(FileObject,
		Wait,
		InputBuffer,
		InputBufferLength,
		OutputBuffer,
		OutputBufferLength,
		IoControlCode,
		IoStatus,
		devExt->AttachedToDeviceObject);

	LPCONSOLE_IO_MSG_T pMsg = (LPCONSOLE_IO_MSG_T)InputBuffer;
	switch (IoControlCode)
	{
	case 0x500013:
	{
		// Write IO output
		ULONG outLen = pMsg->buffLength + 0x10;
		PVOID pOut = ExAllocatePoolWithTag(NonPagedPool, outLen, ALLOCATE_TAG);
		if (pOut)
		{
			RtlZeroMemory(pOut, outLen);
			RtlCopyMemory(pOut, pMsg->buffer, pMsg->buffLength);
			DbgPrint("Input: %ws\n", pOut);
			ExFreePoolWithTag(pOut, ALLOCATE_TAG);
		}
		break;
	}		
	case 0x50000F:
	{
		// Read IO input		
		ULONG inLen = pMsg->buffLength + 0x10;
		PVOID pIn = ExAllocatePoolWithTag(NonPagedPool, inLen, ALLOCATE_TAG);
		if (pIn)
		{
			RtlZeroMemory(pIn, inLen);
			RtlCopyMemory(pIn, pMsg->buffer, pMsg->buffLength);
			DbgPrint("Output: %ws\n", pIn);
			ExFreePoolWithTag(pIn, ALLOCATE_TAG);
		}
		break;
	}		
	default:
		break;
	}
	return bResult;
}

extern "C"
VOID DriverUnload(__in PDRIVER_OBJECT driverObject)
{
	LPWINCONMON_DEVICE_EXTENSION devExt = (LPWINCONMON_DEVICE_EXTENSION)driverObject->DeviceObject->DeviceExtension;
	if (devExt->AttachedToDeviceObject)
	{
		IoDetachDevice(devExt->AttachedToDeviceObject);
		devExt->AttachedToDeviceObject = NULL;
	}	

	//UNICODE_STRING symLinkName;
	//RtlInitUnicodeString(&symLinkName, WINCONMON_DOSDEVICE_NAME);
	//NTSTATUS status = IoDeleteSymbolicLink(&symLinkName);

	if (g_FastIoDispatch) ExFreePoolWithTag(g_FastIoDispatch, ALLOCATE_TAG);

	if (driverObject->DeviceObject)
		IoDeleteDevice(driverObject->DeviceObject);
}

extern "C"
NTSTATUS DriverEntry(__in PDRIVER_OBJECT driverObject, __in PUNICODE_STRING registryPath)
{
#ifdef DBG
	DbgBreakPoint();
#endif
	UNREFERENCED_PARAMETER(registryPath);	
	NTSTATUS status;
	UNICODE_STRING devName;	
	PDEVICE_OBJECT winConMonDeviceObject = NULL;

	// Create device object
	RtlInitUnicodeString(&devName, WINCONMON_DEVICE_NAME);
	status = IoCreateDevice(
		driverObject, //DriverObject
		sizeof(WINCONMON_DEVICE_EXTENSION), //DeviceExtensionSize
		&devName, //DeviceName
		FILE_DEVICE_CONSOLE, //DeviceType
		FILE_DEVICE_SECURE_OPEN, //DeviceCharacteristics
		FALSE, //Exclusive
		&winConMonDeviceObject); //DeviceObject
	if (!NT_SUCCESS(status))
		return STATUS_UNSUCCESSFUL;

	// Create device symbol link for user-mode communication
	//UNICODE_STRING symLinkName;
	//RtlInitUnicodeString(&symLinkName, WINCONMON_DOSDEVICE_NAME);
	//status = IoCreateSymbolicLink(&symLinkName, &devName);

	// Assigning IRP major functions
	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
		driverObject->MajorFunction[i] = WinConMonDefaultDispatch;
	}

	// Assigning driver unload
	driverObject->DriverUnload = DriverUnload;

	// Assigning fast I/O device control	
	g_FastIoDispatch = (FAST_IO_DISPATCH *)ExAllocatePoolWithTag(NonPagedPool, sizeof(FAST_IO_DISPATCH), ALLOCATE_TAG);
	if (!g_FastIoDispatch) goto EXIT_FAILED;
	RtlZeroMemory(g_FastIoDispatch, sizeof(FAST_IO_DISPATCH));
	g_FastIoDispatch->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
	g_FastIoDispatch->FastIoDeviceControl = WinConMonFastIoDeviceControl;
	driverObject->FastIoDispatch = g_FastIoDispatch;		

	// Get some undocument functions from ntoskrnl module
	UNICODE_STRING funcName;
	RtlInitUnicodeString(&funcName, L"ObReferenceObjectByName");
	fnObReferenceObjectByName = (Fn_ObReferenceObjectByName)MmGetSystemRoutineAddress(&funcName);
	if (!fnObReferenceObjectByName) goto EXIT_FAILED;

	// Get device object of Condrv.sys
	UNICODE_STRING driverName;
	PDRIVER_OBJECT condrvObj = NULL;
	extern POBJECT_TYPE* IoDriverObjectType;
	RtlInitUnicodeString(&driverName, CONDRV_DRIVER_NAME);
	status = fnObReferenceObjectByName(&driverName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&condrvObj);
	if (!NT_SUCCESS(status)) goto EXIT_FAILED;

	LPWINCONMON_DEVICE_EXTENSION devExt = (LPWINCONMON_DEVICE_EXTENSION)winConMonDeviceObject->DeviceExtension;
	status = IoAttachDeviceToDeviceStackSafe(
		winConMonDeviceObject, //SourceDevice
		condrvObj->DeviceObject, //TargetDevice
		&devExt->AttachedToDeviceObject); //AttachedToDeviceObject
	if (condrvObj) ObDereferenceObject(condrvObj);

	if (!NT_SUCCESS(status))
	{
EXIT_FAILED:		
		if (g_FastIoDispatch) ExFreePoolWithTag(g_FastIoDispatch, ALLOCATE_TAG);
		if (winConMonDeviceObject) IoDeleteDevice(winConMonDeviceObject);
	}

	return status;
}