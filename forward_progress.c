#include "ramdisk.h"

void EvtForwardProgressRequestDestroy(WDFOBJECT request)
/*++

Routine Description:

    This event is called when the request memory is about to be freed.
    Reserved requests get deleted only when the queue gets deleted.


Arguments:

    Request - Handle to a framework request object.


Return Value:

    VOID

--*/
{    
	PFWD_PROGRESS_REQUEST_CONTEXT context;

	context = GetForwardProgressRequestContext(request); 
}

void EvtForwardProgressRequestCleanup(WDFOBJECT request)
/*++

Routine Description:

    This event is called when the reserved request is about to be deleted.
    NOTE: In case of reserved request this callback doesn't get called after the 
    I/O is done but only when the request is about to be deleted.  

Arguments:

    Request - Handle to a framework request object.


Return Value:

    VOID

--*/
{
	FWD_PROGRESS_REQUEST_CONTEXT *context;

	context = GetForwardProgressRequestContext(request);

	// Cleanup any resources allocated earlier for reserved requests here.
}

NTSTATUS AllocateAdditionalRequestContext(__in WDFREQUEST request)
/*++

Routine Description:
     Allocate  resources used by request. 
     Set the EvtCleanupCallback and  EvtDestroyCallback
     to show the lifetime of a Reserved request.

Arguments:

    Request - Handle to a framework request object.
    
Return Value:


    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES requestContextAttributes;
    PFWD_PROGRESS_REQUEST_CONTEXT reqContext;
    NTSTATUS status;
    
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestContextAttributes, 
                                            FWD_PROGRESS_REQUEST_CONTEXT);
    requestContextAttributes.EvtCleanupCallback = EvtForwardProgressRequestCleanup; 
    requestContextAttributes.EvtDestroyCallback = EvtForwardProgressRequestDestroy;
    status = WdfObjectAllocateContext(request, 
                                      &requestContextAttributes, 
                                      &reqContext);
            
    return status;
}


WDF_IO_FORWARD_PROGRESS_ACTION
EvtIoWdmIrpForForwardProgress(
    __in WDFQUEUE Queue,
    __in PIRP Irp    
    )

/*++

Routine Description:
     A driver's EvtIoWdmIrpForForwardProgress callback function is used for 
     examining an IRP and tell the framework whether to use a reserved 
     request object for the IRP or to fail the I/O request by completing 
     it with an error status value. 

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Irp   -  
Return Value:

    WDF_IO_FORWARD_PROGRESS_ACTION
         WdfIoForwardProgressActionFailRequest is returned it causes 
          the Framework to fail the IRP.
         WdfIoForwardProgressActionUseReservedRequest is returned it causes 
          the framework to use a reserved request to handle the IRP.
--*/
{
    PIO_STACK_LOCATION irpStack;
    WDF_IO_FORWARD_PROGRESS_ACTION action;

    UNREFERENCED_PARAMETER(Queue);

    irpStack = IoGetCurrentIrpStackLocation(Irp);  
    switch (irpStack->MajorFunction) {
            case IRP_MJ_READ:
            case IRP_MJ_WRITE:           
            case IRP_MJ_DEVICE_CONTROL:
            case IRP_MJ_INTERNAL_DEVICE_CONTROL:

                //
                // Use reserved request for reads, writes, IOCTL's
                //
                action = WdfIoForwardProgressActionUseReservedRequest;
                break;

            default:
                 //
                 // Just for demonstration of the available actions fail the  
                 // other I/O IRP's 
                 //
                 action = WdfIoForwardProgressActionFailRequest;
                 break;
    }

    return action;
}

NTSTATUS
EvtIoAllocateResourcesForReservedRequest(
    __in WDFQUEUE Queue,
    __in WDFREQUEST Request
    )

/*++

Routine Description:

    A driver's EvtIoAllocateResourcesForReservedRequest callback function 
    allocates and stores request-specific resources for request objects that 
    the framework is reserving for low-memory situations. 
     
    NOTE: You can't call WdfRequestGetIoQueue for Reserved requests 
          Use the Queue handle passed in.
     

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.


Return Value:

    NTSTATUS

--*/
{   
    NTSTATUS status;
    PFWD_PROGRESS_REQUEST_CONTEXT fwdReqContext;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(fwdReqContext);
    
    status = STATUS_SUCCESS;
    ASSERT(WdfRequestIsReserved(Request));

    
    //
    // Allocate all resources needed for the request here. If you need to 
    // pre-allocate memory or any other resource do it in this callback and 
    // store it in the context. 
    //
    status = AllocateAdditionalRequestContext(Request);  
    if (NT_SUCCESS(status)) {
        PFWD_PROGRESS_REQUEST_CONTEXT fwdReqContext;

        fwdReqContext = GetForwardProgressRequestContext(Request);               
    }

    return status;
}

NTSTATUS
EvtIoAllocateResources(
    __in WDFQUEUE Queue,
    __in WDFREQUEST Request
    )

/*++

Routine Description:

    This event is called for the driver to allocate request 
    resources for immediate use (unlike reserved requests which is for use under 
     low memory). 
    It is called  immediately after the framework has received an IRP and created 
    a request object for the IRP.


Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

 
Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status;
    PFWD_PROGRESS_REQUEST_CONTEXT fwdReqContext;

    UNREFERENCED_PARAMETER(Queue);
    status = STATUS_SUCCESS;

    //
    // Allocate all resources needed for the request here and store it in the request
    // context. 
    //
    fwdReqContext = GetForwardProgressRequestContext(Request);               

    return status;
}

NTSTATUS SetForwardProgressOnQueue(__in WDFQUEUE queue)

/*++

Routine Description:
    Set forward progress on the top level( handles one of the major I/O IRP) 
    queue we created. 
    The default is always a top level queue or if the queue was configured 
    with WdfDeviceConfigureRequestDispatching.
    
Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.


Return Value:

    NTSTATUS

--*/
{

    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY forwardProgressPolicy;
    NTSTATUS status;

    // The policy is configurable by the user. In the code segment below 
    // WdfIoForwardProgressReservedPolicyUseExamine 
    // is demonstrated. If your driver supports paging I/O you should select 
    // WdfIoForwardProgressReservedPolicyPagingIO.
    // MAX_RESERVED_REQUESTS should be adjusted depending on the number of parallel 
    // requests the driver wants to handle under low memory conditions. It may 
    // require some hit and trial to get the right number.
    WDF_IO_QUEUE_FORWARD_PROGRESS_POLICY_EXAMINE_INIT(&forwardProgressPolicy, MAX_RESERVED_REQUESTS, EvtIoWdmIrpForForwardProgress);

    forwardProgressPolicy.EvtIoAllocateResourcesForReservedRequest = EvtIoAllocateResourcesForReservedRequest;       
    forwardProgressPolicy.EvtIoAllocateRequestResources = EvtIoAllocateResources;           

    status = WdfIoQueueAssignForwardProgressPolicy(queue, &forwardProgressPolicy);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Error WdfIoQueueAssignForwardProgressPolicy 0x%x.\n", status));
        return status;
    }

    return status;
}
