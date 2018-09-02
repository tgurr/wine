/*
 * Process synchronisation
 *
 * Copyright 1996, 1997, 1998 Marcus Meissner
 * Copyright 1997, 1999 Alexandre Julliard
 * Copyright 1999, 2000 Juergen Schmied
 * Copyright 2003 Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#ifdef HAVE_SCHED_H
# include <sched.h>
#endif
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/task.h>
# include <mach/semaphore.h>
# include <mach/mach_time.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "unix_private.h"
#include "esync.h"
#include "fsync.h"

WINE_DEFAULT_DEBUG_CHANNEL(sync);

HANDLE keyed_event = 0;

static const char *debugstr_timeout( const LARGE_INTEGER *timeout )
{
    if (!timeout) return "(infinite)";
    return wine_dbgstr_longlong( timeout->QuadPart );
}

/* return a monotonic time counter, in Win32 ticks */
static inline ULONGLONG monotonic_counter(void)
{
    struct timeval now;
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;

    if (!timebase.denom) mach_timebase_info( &timebase );
#ifdef HAVE_MACH_CONTINUOUS_TIME
    if (&mach_continuous_time != NULL)
        return mach_continuous_time() * timebase.numer / timebase.denom / 100;
#endif
    return mach_absolute_time() * timebase.numer / timebase.denom / 100;
#elif defined(HAVE_CLOCK_GETTIME)
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (!clock_gettime( CLOCK_MONOTONIC_RAW, &ts ))
        return ts.tv_sec * (ULONGLONG)TICKSPERSEC + ts.tv_nsec / 100;
#endif
    if (!clock_gettime( CLOCK_MONOTONIC, &ts ))
        return ts.tv_sec * (ULONGLONG)TICKSPERSEC + ts.tv_nsec / 100;
#endif
    gettimeofday( &now, 0 );
    return ticks_from_time_t( now.tv_sec ) + now.tv_usec * 10 - server_start_time;
}


#ifdef __linux__

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int futex_private = 128;

static inline int futex_wait( const LONG *addr, int val, struct timespec *timeout )
{
#if (defined(__i386__) || defined(__arm__)) && _TIME_BITS==64
    if (timeout && sizeof(*timeout) != 8)
    {
        struct {
            long tv_sec;
            long tv_nsec;
        } timeout32 = { timeout->tv_sec, timeout->tv_nsec };

        return syscall( __NR_futex, addr, FUTEX_WAIT | futex_private, val, &timeout32, 0, 0 );
    }
#endif
    return syscall( __NR_futex, addr, FUTEX_WAIT | futex_private, val, timeout, 0, 0 );
}

static inline int futex_wake( const LONG *addr, int val )
{
    return syscall( __NR_futex, addr, FUTEX_WAKE | futex_private, val, NULL, 0, 0 );
}

static inline int use_futexes(void)
{
    static LONG supported = -1;

    if (supported == -1)
    {
        futex_wait( &supported, 10, NULL );
        if (errno == ENOSYS)
        {
            futex_private = 0;
            futex_wait( &supported, 10, NULL );
        }
        supported = (errno != ENOSYS);
    }
    return supported;
}

#endif


/* create a struct security_descriptor and contained information in one contiguous piece of memory */
unsigned int alloc_object_attributes( const OBJECT_ATTRIBUTES *attr, struct object_attributes **ret,
                                      data_size_t *ret_len )
{
    unsigned int len = sizeof(**ret);
    SID *owner = NULL, *group = NULL;
    ACL *dacl = NULL, *sacl = NULL;
    SECURITY_DESCRIPTOR *sd;

    *ret = NULL;
    *ret_len = 0;

    if (!attr) return STATUS_SUCCESS;

    if (attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;

    if ((sd = attr->SecurityDescriptor))
    {
        len += sizeof(struct security_descriptor);
	if (sd->Revision != SECURITY_DESCRIPTOR_REVISION) return STATUS_UNKNOWN_REVISION;
        if (sd->Control & SE_SELF_RELATIVE)
        {
            SECURITY_DESCRIPTOR_RELATIVE *rel = (SECURITY_DESCRIPTOR_RELATIVE *)sd;
            if (rel->Owner) owner = (PSID)((BYTE *)rel + rel->Owner);
            if (rel->Group) group = (PSID)((BYTE *)rel + rel->Group);
            if ((sd->Control & SE_SACL_PRESENT) && rel->Sacl) sacl = (PSID)((BYTE *)rel + rel->Sacl);
            if ((sd->Control & SE_DACL_PRESENT) && rel->Dacl) dacl = (PSID)((BYTE *)rel + rel->Dacl);
        }
        else
        {
            owner = sd->Owner;
            group = sd->Group;
            if (sd->Control & SE_SACL_PRESENT) sacl = sd->Sacl;
            if (sd->Control & SE_DACL_PRESENT) dacl = sd->Dacl;
        }

        if (owner) len += offsetof( SID, SubAuthority[owner->SubAuthorityCount] );
        if (group) len += offsetof( SID, SubAuthority[group->SubAuthorityCount] );
        if (sacl) len += sacl->AclSize;
        if (dacl) len += dacl->AclSize;

        /* fix alignment for the Unicode name that follows the structure */
        len = (len + sizeof(WCHAR) - 1) & ~(sizeof(WCHAR) - 1);
    }

    if (attr->ObjectName)
    {
        if ((ULONG_PTR)attr->ObjectName->Buffer & (sizeof(WCHAR) - 1)) return STATUS_DATATYPE_MISALIGNMENT;
        if (attr->ObjectName->Length & (sizeof(WCHAR) - 1)) return STATUS_OBJECT_NAME_INVALID;
        len += attr->ObjectName->Length;
    }
    else if (attr->RootDirectory) return STATUS_OBJECT_NAME_INVALID;

    len = (len + 3) & ~3;  /* DWORD-align the entire structure */

    if (!(*ret = calloc( len, 1 ))) return STATUS_NO_MEMORY;

    (*ret)->rootdir = wine_server_obj_handle( attr->RootDirectory );
    (*ret)->attributes = attr->Attributes;

    if (attr->SecurityDescriptor)
    {
        struct security_descriptor *descr = (struct security_descriptor *)(*ret + 1);
        unsigned char *ptr = (unsigned char *)(descr + 1);

        descr->control = sd->Control & ~SE_SELF_RELATIVE;
        if (owner) descr->owner_len = offsetof( SID, SubAuthority[owner->SubAuthorityCount] );
        if (group) descr->group_len = offsetof( SID, SubAuthority[group->SubAuthorityCount] );
        if (sacl) descr->sacl_len = sacl->AclSize;
        if (dacl) descr->dacl_len = dacl->AclSize;

        memcpy( ptr, owner, descr->owner_len );
        ptr += descr->owner_len;
        memcpy( ptr, group, descr->group_len );
        ptr += descr->group_len;
        memcpy( ptr, sacl, descr->sacl_len );
        ptr += descr->sacl_len;
        memcpy( ptr, dacl, descr->dacl_len );
        (*ret)->sd_len = (sizeof(*descr) + descr->owner_len + descr->group_len + descr->sacl_len +
                          descr->dacl_len + sizeof(WCHAR) - 1) & ~(sizeof(WCHAR) - 1);
    }

    if (attr->ObjectName)
    {
        unsigned char *ptr = (unsigned char *)(*ret + 1) + (*ret)->sd_len;
        (*ret)->name_len = attr->ObjectName->Length;
        memcpy( ptr, attr->ObjectName->Buffer, (*ret)->name_len );
    }

    *ret_len = len;
    return STATUS_SUCCESS;
}


static unsigned int validate_open_object_attributes( const OBJECT_ATTRIBUTES *attr )
{
    if (!attr || attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;

    if (attr->ObjectName)
    {
        if ((ULONG_PTR)attr->ObjectName->Buffer & (sizeof(WCHAR) - 1)) return STATUS_DATATYPE_MISALIGNMENT;
        if (attr->ObjectName->Length & (sizeof(WCHAR) - 1)) return STATUS_OBJECT_NAME_INVALID;
    }
    else if (attr->RootDirectory) return STATUS_OBJECT_NAME_INVALID;

    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtCreateSemaphore (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateSemaphore( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                   LONG initial, LONG max )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if (max <= 0 || initial < 0 || initial > max) return STATUS_INVALID_PARAMETER;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    if (do_fsync())
        return fsync_create_semaphore( handle, access, attr, initial, max );

    if (do_esync())
        return esync_create_semaphore( handle, access, attr, initial, max );

    SERVER_START_REQ( create_semaphore )
    {
        req->access  = access;
        req->initial = initial;
        req->max     = max;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/******************************************************************************
 *              NtOpenSemaphore (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenSemaphore( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;

    if (do_esync())
        return esync_open_semaphore( handle, access, attr );

    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_semaphore )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtQuerySemaphore (NTDLL.@)
 */
NTSTATUS WINAPI NtQuerySemaphore( HANDLE handle, SEMAPHORE_INFORMATION_CLASS class,
                                  void *info, ULONG len, ULONG *ret_len )
{
    unsigned int ret;
    SEMAPHORE_BASIC_INFORMATION *out = info;

    TRACE("(%p, %u, %p, %u, %p)\n", handle, class, info, (int)len, ret_len);

    if (class != SemaphoreBasicInformation)
    {
        FIXME("(%p,%d,%u) Unknown class\n", handle, class, (int)len);
        return STATUS_INVALID_INFO_CLASS;
    }

    if (len != sizeof(SEMAPHORE_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;

    if (do_esync())
        return esync_query_semaphore( handle, info, ret_len );

    SERVER_START_REQ( query_semaphore )
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call( req )))
        {
            out->CurrentCount = reply->current;
            out->MaximumCount = reply->max;
            if (ret_len) *ret_len = sizeof(SEMAPHORE_BASIC_INFORMATION);
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtReleaseSemaphore (NTDLL.@)
 */
NTSTATUS WINAPI NtReleaseSemaphore( HANDLE handle, ULONG count, ULONG *previous )
{
    unsigned int ret;

    if (do_fsync())
        return fsync_release_semaphore( handle, count, previous );

    if (do_esync())
        return esync_release_semaphore( handle, count, previous );

    SERVER_START_REQ( release_semaphore )
    {
        req->handle = wine_server_obj_handle( handle );
        req->count  = count;
        if (!(ret = wine_server_call( req )))
        {
            if (previous) *previous = reply->prev_count;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *              NtCreateEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateEvent( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                               EVENT_TYPE type, BOOLEAN state )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if (type != NotificationEvent && type != SynchronizationEvent) return STATUS_INVALID_PARAMETER;

    if (do_fsync())
        return fsync_create_event( handle, access, attr, type, state );

    if (do_esync())
        return esync_create_event( handle, access, attr, type, state );

    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_event )
    {
        req->access = access;
        req->manual_reset = (type == NotificationEvent);
        req->initial_state = state;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/******************************************************************************
 *              NtOpenEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenEvent( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    if (do_esync())
        return esync_open_event( handle, access, attr );

    SERVER_START_REQ( open_event )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtSetEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtSetEvent( HANDLE handle, LONG *prev_state )
{
    /* This comment is a dummy to make sure this patch applies in the right place. */
    unsigned int ret;

    if (do_fsync())
        return fsync_set_event( handle, prev_state );

    if (do_esync())
        return esync_set_event( handle );

    SERVER_START_REQ( event_op )
    {
        req->handle = wine_server_obj_handle( handle );
        req->op     = SET_EVENT;
        ret = wine_server_call( req );
        if (!ret && prev_state) *prev_state = reply->state;
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtResetEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtResetEvent( HANDLE handle, LONG *prev_state )
{
    /* This comment is a dummy to make sure this patch applies in the right place. */
    unsigned int ret;

    if (do_fsync())
        return fsync_reset_event( handle, prev_state );

    if (do_esync())
        return esync_reset_event( handle );


    SERVER_START_REQ( event_op )
    {
        req->handle = wine_server_obj_handle( handle );
        req->op     = RESET_EVENT;
        ret = wine_server_call( req );
        if (!ret && prev_state) *prev_state = reply->state;
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtClearEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtClearEvent( HANDLE handle )
{
    /* FIXME: same as NtResetEvent ??? */
    return NtResetEvent( handle, NULL );
}


/******************************************************************************
 *              NtPulseEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtPulseEvent( HANDLE handle, LONG *prev_state )
{
    unsigned int ret;

    if (do_esync())
        return esync_pulse_event( handle );

    SERVER_START_REQ( event_op )
    {
        req->handle = wine_server_obj_handle( handle );
        req->op     = PULSE_EVENT;
        ret = wine_server_call( req );
        if (!ret && prev_state) *prev_state = reply->state;
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtQueryEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryEvent( HANDLE handle, EVENT_INFORMATION_CLASS class,
                              void *info, ULONG len, ULONG *ret_len )
{
    unsigned int ret;
    EVENT_BASIC_INFORMATION *out = info;

    TRACE("(%p, %u, %p, %u, %p)\n", handle, class, info, (int)len, ret_len);

    if (class != EventBasicInformation)
    {
        FIXME("(%p, %d, %d) Unknown class\n", handle, class, (int)len);
        return STATUS_INVALID_INFO_CLASS;
    }

    if (len != sizeof(EVENT_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;

    if (do_esync())
        return esync_query_event( handle, info, ret_len );

    SERVER_START_REQ( query_event )
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call( req )))
        {
            out->EventType  = reply->manual_reset ? NotificationEvent : SynchronizationEvent;
            out->EventState = reply->state;
            if (ret_len) *ret_len = sizeof(EVENT_BASIC_INFORMATION);
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtCreateMutant (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateMutant( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                BOOLEAN owned )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;

    if (do_fsync())
        return fsync_create_mutex( handle, access, attr, owned );

    if (do_esync())
        return esync_create_mutex( handle, access, attr, owned );

    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_mutex )
    {
        req->access  = access;
        req->owned   = owned;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/**************************************************************************
 *              NtOpenMutant (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenMutant( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    if (do_esync())
        return esync_open_mutex( handle, access, attr );

    SERVER_START_REQ( open_mutex )
    {
        req->access  = access;
        req->attributes = attr->Attributes;
        req->rootdir = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *              NtReleaseMutant (NTDLL.@)
 */
NTSTATUS WINAPI NtReleaseMutant( HANDLE handle, LONG *prev_count )
{
    unsigned int ret;

    if (do_fsync())
        return fsync_release_mutex( handle, prev_count );

    if (do_esync())
        return esync_release_mutex( handle, prev_count );

    SERVER_START_REQ( release_mutex )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
        if (prev_count) *prev_count = 1 - reply->prev_count;
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************
 *              NtQueryMutant (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryMutant( HANDLE handle, MUTANT_INFORMATION_CLASS class,
                               void *info, ULONG len, ULONG *ret_len )
{
    unsigned int ret;
    MUTANT_BASIC_INFORMATION *out = info;

    TRACE("(%p, %u, %p, %u, %p)\n", handle, class, info, (int)len, ret_len);

    if (class != MutantBasicInformation)
    {
        FIXME( "(%p, %d, %d) Unknown class\n", handle, class, (int)len );
        return STATUS_INVALID_INFO_CLASS;
    }

    if (len != sizeof(MUTANT_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;

    if (do_esync())
        return esync_query_mutex( handle, info, ret_len );

    SERVER_START_REQ( query_mutex )
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call( req )))
        {
            out->CurrentCount   = 1 - reply->count;
            out->OwnedByCaller  = reply->owned;
            out->AbandonedState = reply->abandoned;
            if (ret_len) *ret_len = sizeof(MUTANT_BASIC_INFORMATION);
        }
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *		NtCreateJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateJobObject( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_job )
    {
        req->access = access;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    free( objattr );
    return ret;
}


/**************************************************************************
 *		NtOpenJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenJobObject( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_job )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *		NtTerminateJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtTerminateJobObject( HANDLE handle, NTSTATUS status )
{
    unsigned int ret;

    TRACE( "(%p, %d)\n", handle, (int)status );

    SERVER_START_REQ( terminate_job )
    {
        req->handle = wine_server_obj_handle( handle );
        req->status = status;
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;

    return ret;
}


/**************************************************************************
 *		NtQueryInformationJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryInformationJobObject( HANDLE handle, JOBOBJECTINFOCLASS class, void *info,
                                             ULONG len, ULONG *ret_len )
{
    unsigned int ret;

    TRACE( "semi-stub: %p %u %p %u %p\n", handle, class, info, (int)len, ret_len );

    if (class >= MaxJobObjectInfoClass) return STATUS_INVALID_PARAMETER;

    switch (class)
    {
    case JobObjectBasicAccountingInformation:
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *accounting = info;

        if (len < sizeof(*accounting)) return STATUS_INFO_LENGTH_MISMATCH;
        SERVER_START_REQ(get_job_info)
        {
            req->handle = wine_server_obj_handle( handle );
            if (!(ret = wine_server_call( req )))
            {
                memset( accounting, 0, sizeof(*accounting) );
                accounting->TotalProcesses = reply->total_processes;
                accounting->ActiveProcesses = reply->active_processes;
            }
        }
        SERVER_END_REQ;
        if (ret_len) *ret_len = sizeof(*accounting);
        return ret;
    }
    case JobObjectBasicProcessIdList:
    {
        JOBOBJECT_BASIC_PROCESS_ID_LIST *process = info;
        DWORD count, i;

        if (len < sizeof(*process)) return STATUS_INFO_LENGTH_MISMATCH;

        count  = len - offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList );
        count /= sizeof(process->ProcessIdList[0]);

        SERVER_START_REQ( get_job_info )
        {
            req->handle = wine_server_user_handle(handle);
            wine_server_set_reply(req, process->ProcessIdList, count * sizeof(process_id_t));
            if (!(ret = wine_server_call(req)))
            {
                process->NumberOfAssignedProcesses = reply->active_processes;
                process->NumberOfProcessIdsInList = min(count, reply->active_processes);
            }
        }
        SERVER_END_REQ;

        if (ret != STATUS_SUCCESS) return ret;

        if (sizeof(process_id_t) < sizeof(process->ProcessIdList[0]))
        {
            /* start from the end to not overwrite */
            for (i = process->NumberOfProcessIdsInList; i--;)
            {
                ULONG_PTR id = ((process_id_t *)process->ProcessIdList)[i];
                process->ProcessIdList[i] = id;
            }
        }

        if (ret_len)
            *ret_len = offsetof( JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList[process->NumberOfProcessIdsInList] );
        return count < process->NumberOfAssignedProcesses ? STATUS_MORE_ENTRIES : STATUS_SUCCESS;
    }
    case JobObjectExtendedLimitInformation:
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION *extended_limit = info;

        if (len < sizeof(*extended_limit)) return STATUS_INFO_LENGTH_MISMATCH;
        memset( extended_limit, 0, sizeof(*extended_limit) );
        if (ret_len) *ret_len = sizeof(*extended_limit);
        return STATUS_SUCCESS;
    }
    case JobObjectBasicLimitInformation:
    {
        JOBOBJECT_BASIC_LIMIT_INFORMATION *basic_limit = info;

        if (len < sizeof(*basic_limit)) return STATUS_INFO_LENGTH_MISMATCH;
        memset( basic_limit, 0, sizeof(*basic_limit) );
        if (ret_len) *ret_len = sizeof(*basic_limit);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}


/**************************************************************************
 *		NtSetInformationJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtSetInformationJobObject( HANDLE handle, JOBOBJECTINFOCLASS class, void *info, ULONG len )
{
    unsigned int status = STATUS_NOT_IMPLEMENTED;
    JOBOBJECT_BASIC_LIMIT_INFORMATION *basic_limit;
    ULONG info_size = sizeof(JOBOBJECT_BASIC_LIMIT_INFORMATION);
    DWORD limit_flags = JOB_OBJECT_BASIC_LIMIT_VALID_FLAGS;

    TRACE( "(%p, %u, %p, %u)\n", handle, class, info, (int)len );

    if (class >= MaxJobObjectInfoClass) return STATUS_INVALID_PARAMETER;

    switch (class)
    {

    case JobObjectExtendedLimitInformation:
        info_size = sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
        limit_flags = JOB_OBJECT_EXTENDED_LIMIT_VALID_FLAGS;
        /* fall through */
    case JobObjectBasicLimitInformation:
        if (len != info_size) return STATUS_INVALID_PARAMETER;
        basic_limit = info;
        if (basic_limit->LimitFlags & ~limit_flags) return STATUS_INVALID_PARAMETER;
        SERVER_START_REQ( set_job_limits )
        {
            req->handle = wine_server_obj_handle( handle );
            req->limit_flags = basic_limit->LimitFlags;
            status = wine_server_call( req );
        }
        SERVER_END_REQ;
        break;
    case JobObjectAssociateCompletionPortInformation:
        if (len != sizeof(JOBOBJECT_ASSOCIATE_COMPLETION_PORT)) return STATUS_INVALID_PARAMETER;
        SERVER_START_REQ( set_job_completion_port )
        {
            JOBOBJECT_ASSOCIATE_COMPLETION_PORT *port_info = info;
            req->job = wine_server_obj_handle( handle );
            req->port = wine_server_obj_handle( port_info->CompletionPort );
            req->key = wine_server_client_ptr( port_info->CompletionKey );
            status = wine_server_call( req );
        }
        SERVER_END_REQ;
        break;
    case JobObjectBasicUIRestrictions:
        status = STATUS_SUCCESS;
        /* fall through */
    default:
        FIXME( "stub: %p %u %p %u\n", handle, class, info, (int)len );
    }
    return status;
}


/**************************************************************************
 *		NtIsProcessInJob (NTDLL.@)
 */
NTSTATUS WINAPI NtIsProcessInJob( HANDLE process, HANDLE job )
{
    unsigned int status;

    TRACE( "(%p %p)\n", job, process );

    SERVER_START_REQ( process_in_job )
    {
        req->job     = wine_server_obj_handle( job );
        req->process = wine_server_obj_handle( process );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}


/**************************************************************************
 *		NtAssignProcessToJobObject (NTDLL.@)
 */
NTSTATUS WINAPI NtAssignProcessToJobObject( HANDLE job, HANDLE process )
{
    unsigned int status;

    TRACE( "(%p %p)\n", job, process );

    SERVER_START_REQ( assign_job )
    {
        req->job     = wine_server_obj_handle( job );
        req->process = wine_server_obj_handle( process );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}


/**********************************************************************
 *           NtCreateDebugObject  (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateDebugObject( HANDLE *handle, ACCESS_MASK access,
                                     OBJECT_ATTRIBUTES *attr, ULONG flags )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if (flags & ~DEBUG_KILL_ON_CLOSE) return STATUS_INVALID_PARAMETER;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_debug_obj )
    {
        req->access = access;
        req->flags  = flags;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    free( objattr );
    return ret;
}


/**********************************************************************
 *           NtSetInformationDebugObject  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetInformationDebugObject( HANDLE handle, DEBUGOBJECTINFOCLASS class,
                                             void *info, ULONG len, ULONG *ret_len )
{
    unsigned int ret;
    ULONG flags;

    if (class != DebugObjectKillProcessOnExitInformation) return STATUS_INVALID_PARAMETER;
    if (len != sizeof(ULONG))
    {
        if (ret_len) *ret_len = sizeof(ULONG);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    flags = *(ULONG *)info;
    if (flags & ~DEBUG_KILL_ON_CLOSE) return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( set_debug_obj_info )
    {
        req->debug = wine_server_obj_handle( handle );
        req->flags = flags;
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (!ret && ret_len) *ret_len = 0;
    return ret;
}


/* convert the server event data to an NT state change; helper for NtWaitForDebugEvent */
static NTSTATUS event_data_to_state_change( const debug_event_t *data, DBGUI_WAIT_STATE_CHANGE *state )
{
    int i;

    switch (data->code)
    {
    case DbgIdle:
    case DbgReplyPending:
        return STATUS_PENDING;
    case DbgCreateThreadStateChange:
    {
        DBGUI_CREATE_THREAD *info = &state->StateInfo.CreateThread;
        info->HandleToThread         = wine_server_ptr_handle( data->create_thread.handle );
        info->NewThread.StartAddress = wine_server_get_ptr( data->create_thread.start );
        return STATUS_SUCCESS;
    }
    case DbgCreateProcessStateChange:
    {
        DBGUI_CREATE_PROCESS *info = &state->StateInfo.CreateProcessInfo;
        info->HandleToProcess                       = wine_server_ptr_handle( data->create_process.process );
        info->HandleToThread                        = wine_server_ptr_handle( data->create_process.thread );
        info->NewProcess.FileHandle                 = wine_server_ptr_handle( data->create_process.file );
        info->NewProcess.BaseOfImage                = wine_server_get_ptr( data->create_process.base );
        info->NewProcess.DebugInfoFileOffset        = data->create_process.dbg_offset;
        info->NewProcess.DebugInfoSize              = data->create_process.dbg_size;
        info->NewProcess.InitialThread.StartAddress = wine_server_get_ptr( data->create_process.start );
        return STATUS_SUCCESS;
    }
    case DbgExitThreadStateChange:
        state->StateInfo.ExitThread.ExitStatus = data->exit.exit_code;
        return STATUS_SUCCESS;
    case DbgExitProcessStateChange:
        state->StateInfo.ExitProcess.ExitStatus = data->exit.exit_code;
        return STATUS_SUCCESS;
    case DbgExceptionStateChange:
    case DbgBreakpointStateChange:
    case DbgSingleStepStateChange:
    {
        DBGKM_EXCEPTION *info = &state->StateInfo.Exception;
        info->FirstChance = data->exception.first;
        info->ExceptionRecord.ExceptionCode    = data->exception.exc_code;
        info->ExceptionRecord.ExceptionFlags   = data->exception.flags;
        info->ExceptionRecord.ExceptionRecord  = wine_server_get_ptr( data->exception.record );
        info->ExceptionRecord.ExceptionAddress = wine_server_get_ptr( data->exception.address );
        info->ExceptionRecord.NumberParameters = data->exception.nb_params;
        for (i = 0; i < data->exception.nb_params; i++)
            info->ExceptionRecord.ExceptionInformation[i] = data->exception.params[i];
        return STATUS_SUCCESS;
    }
    case DbgLoadDllStateChange:
    {
        DBGKM_LOAD_DLL *info = &state->StateInfo.LoadDll;
        info->FileHandle          = wine_server_ptr_handle( data->load_dll.handle );
        info->BaseOfDll           = wine_server_get_ptr( data->load_dll.base );
        info->DebugInfoFileOffset = data->load_dll.dbg_offset;
        info->DebugInfoSize       = data->load_dll.dbg_size;
        info->NamePointer         = wine_server_get_ptr( data->load_dll.name );
        return STATUS_SUCCESS;
    }
    case DbgUnloadDllStateChange:
        state->StateInfo.UnloadDll.BaseAddress = wine_server_get_ptr( data->unload_dll.base );
        return STATUS_SUCCESS;
    }
    return STATUS_INTERNAL_ERROR;
}

/**********************************************************************
 *           NtWaitForDebugEvent  (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForDebugEvent( HANDLE handle, BOOLEAN alertable, LARGE_INTEGER *timeout,
                                     DBGUI_WAIT_STATE_CHANGE *state )
{
    debug_event_t data;
    unsigned int ret;
    BOOL wait = TRUE;

    for (;;)
    {
        SERVER_START_REQ( wait_debug_event )
        {
            req->debug = wine_server_obj_handle( handle );
            wine_server_set_reply( req, &data, sizeof(data) );
            ret = wine_server_call( req );
            if (!ret && !(ret = event_data_to_state_change( &data, state )))
            {
                state->NewState = data.code;
                state->AppClientId.UniqueProcess = ULongToHandle( reply->pid );
                state->AppClientId.UniqueThread  = ULongToHandle( reply->tid );
            }
        }
        SERVER_END_REQ;

        if (ret != STATUS_PENDING) return ret;
        if (!wait) return STATUS_TIMEOUT;
        wait = FALSE;
        ret = NtWaitForSingleObject( handle, alertable, timeout );
        if (ret != STATUS_WAIT_0) return ret;
    }
}


/**************************************************************************
 *           NtCreateDirectoryObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateDirectoryObject( HANDLE *handle, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_directory )
    {
        req->access = access;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    free( objattr );
    return ret;
}


/**************************************************************************
 *           NtOpenDirectoryObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenDirectoryObject( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_directory )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *           NtQueryDirectoryObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryDirectoryObject( HANDLE handle, DIRECTORY_BASIC_INFORMATION *buffer,
                                        ULONG size, BOOLEAN single_entry, BOOLEAN restart,
                                        ULONG *context, ULONG *ret_size )
{
    ULONG index = restart ? 0 : *context;
    unsigned int ret;

    if (single_entry)
    {
        SERVER_START_REQ( get_directory_entry )
        {
            req->handle = wine_server_obj_handle( handle );
            req->index = index;
            if (size >= 2 * sizeof(*buffer) + 2 * sizeof(WCHAR))
                wine_server_set_reply( req, buffer + 2, size - 2 * sizeof(*buffer) - 2 * sizeof(WCHAR) );
            if (!(ret = wine_server_call( req )))
            {
                buffer->ObjectName.Buffer = (WCHAR *)(buffer + 2);
                buffer->ObjectName.Length = reply->name_len;
                buffer->ObjectName.MaximumLength = reply->name_len + sizeof(WCHAR);
                buffer->ObjectTypeName.Buffer = (WCHAR *)(buffer + 2) + reply->name_len/sizeof(WCHAR) + 1;
                buffer->ObjectTypeName.Length = wine_server_reply_size( reply ) - reply->name_len;
                buffer->ObjectTypeName.MaximumLength = buffer->ObjectTypeName.Length + sizeof(WCHAR);
                /* make room for the terminating null */
                memmove( buffer->ObjectTypeName.Buffer, buffer->ObjectTypeName.Buffer - 1,
                         buffer->ObjectTypeName.Length );
                buffer->ObjectName.Buffer[buffer->ObjectName.Length/sizeof(WCHAR)] = 0;
                buffer->ObjectTypeName.Buffer[buffer->ObjectTypeName.Length/sizeof(WCHAR)] = 0;

                memset( &buffer[1], 0, sizeof(buffer[1]) );

                *context = index + 1;
            }
            else if (ret == STATUS_NO_MORE_ENTRIES)
            {
                if (size > sizeof(*buffer))
                    memset( buffer, 0, sizeof(*buffer) );
                if (ret_size) *ret_size = sizeof(*buffer);
            }

            if (ret_size && (!ret || ret == STATUS_BUFFER_TOO_SMALL))
                *ret_size = 2 * sizeof(*buffer) + reply->total_len + 2 * sizeof(WCHAR);
        }
        SERVER_END_REQ;
    }
    else
    {
        FIXME("multiple entries not implemented\n");
        ret = STATUS_NOT_IMPLEMENTED;
    }
    return ret;
}


/**************************************************************************
 *           NtCreateSymbolicLinkObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateSymbolicLinkObject( HANDLE *handle, ACCESS_MASK access,
                                            OBJECT_ATTRIBUTES *attr, UNICODE_STRING *target )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if (!target->MaximumLength) return STATUS_INVALID_PARAMETER;
    if (!target->Buffer) return STATUS_ACCESS_VIOLATION;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_symlink )
    {
        req->access = access;
        wine_server_add_data( req, objattr, len );
        wine_server_add_data( req, target->Buffer, target->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    free( objattr );
    return ret;
}


/**************************************************************************
 *           NtOpenSymbolicLinkObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenSymbolicLinkObject( HANDLE *handle, ACCESS_MASK access,
                                          const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_symlink )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *           NtQuerySymbolicLinkObject   (NTDLL.@)
 */
NTSTATUS WINAPI NtQuerySymbolicLinkObject( HANDLE handle, UNICODE_STRING *target, ULONG *length )
{
    unsigned int ret;

    if (!target) return STATUS_ACCESS_VIOLATION;

    SERVER_START_REQ( query_symlink )
    {
        req->handle = wine_server_obj_handle( handle );
        if (target->MaximumLength >= sizeof(WCHAR))
            wine_server_set_reply( req, target->Buffer, target->MaximumLength - sizeof(WCHAR) );
        if (!(ret = wine_server_call( req )))
        {
            target->Length = wine_server_reply_size(reply);
            target->Buffer[target->Length / sizeof(WCHAR)] = 0;
            if (length) *length = reply->total + sizeof(WCHAR);
        }
        else if (length && ret == STATUS_BUFFER_TOO_SMALL) *length = reply->total + sizeof(WCHAR);
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *		NtMakeTemporaryObject (NTDLL.@)
 */
NTSTATUS WINAPI NtMakeTemporaryObject( HANDLE handle )
{
    unsigned int ret;

    TRACE("%p\n", handle);

    SERVER_START_REQ( make_temporary )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *		NtCreateTimer (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateTimer( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                               TIMER_TYPE type )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if (type != NotificationTimer && type != SynchronizationTimer) return STATUS_INVALID_PARAMETER;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_timer )
    {
        req->access  = access;
        req->manual  = (type == NotificationTimer);
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;

}


/**************************************************************************
 *		NtOpenTimer (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenTimer( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_timer )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/**************************************************************************
 *		NtSetTimer (NTDLL.@)
 */
NTSTATUS WINAPI NtSetTimer( HANDLE handle, const LARGE_INTEGER *when, PTIMER_APC_ROUTINE callback,
                            void *arg, BOOLEAN resume, ULONG period, BOOLEAN *state )
{
    unsigned int ret = STATUS_SUCCESS;

    TRACE( "(%p,%p,%p,%p,%08x,0x%08x,%p)\n", handle, when, callback, arg, resume, (int)period, state );

    SERVER_START_REQ( set_timer )
    {
        req->handle   = wine_server_obj_handle( handle );
        req->period   = period;
        req->expire   = when->QuadPart;
        req->callback = wine_server_client_ptr( callback );
        req->arg      = wine_server_client_ptr( arg );
        ret = wine_server_call( req );
        if (state) *state = reply->signaled;
    }
    SERVER_END_REQ;

    /* set error but can still succeed */
    if (resume && ret == STATUS_SUCCESS) return STATUS_TIMER_RESUME_IGNORED;
    return ret;
}


/**************************************************************************
 *		NtCancelTimer (NTDLL.@)
 */
NTSTATUS WINAPI NtCancelTimer( HANDLE handle, BOOLEAN *state )
{
    unsigned int ret;

    SERVER_START_REQ( cancel_timer )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
        if (state) *state = reply->signaled;
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *		NtQueryTimer (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryTimer( HANDLE handle, TIMER_INFORMATION_CLASS class,
                              void *info, ULONG len, ULONG *ret_len )
{
    TIMER_BASIC_INFORMATION *basic_info = info;
    unsigned int ret;
    LARGE_INTEGER now;

    TRACE( "(%p,%d,%p,0x%08x,%p)\n", handle, class, info, (int)len, ret_len );

    switch (class)
    {
    case TimerBasicInformation:
        if (len < sizeof(TIMER_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;

        SERVER_START_REQ( get_timer_info )
        {
            req->handle = wine_server_obj_handle( handle );
            ret = wine_server_call(req);
            /* convert server time to absolute NTDLL time */
            basic_info->RemainingTime.QuadPart = reply->when;
            basic_info->TimerState = reply->signaled;
        }
        SERVER_END_REQ;

        /* convert into relative time */
        if (basic_info->RemainingTime.QuadPart > 0) NtQuerySystemTime( &now );
        else
        {
            NtQueryPerformanceCounter( &now, NULL );
            basic_info->RemainingTime.QuadPart = -basic_info->RemainingTime.QuadPart;
        }

        if (now.QuadPart > basic_info->RemainingTime.QuadPart)
            basic_info->RemainingTime.QuadPart = 0;
        else
            basic_info->RemainingTime.QuadPart -= now.QuadPart;

        if (ret_len) *ret_len = sizeof(TIMER_BASIC_INFORMATION);
        return ret;
    }

    FIXME( "Unhandled class %d\n", class );
    return STATUS_INVALID_INFO_CLASS;
}


/******************************************************************
 *		NtWaitForMultipleObjects (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForMultipleObjects( DWORD count, const HANDLE *handles, BOOLEAN wait_any,
                                          BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    select_op_t select_op;
    UINT i, flags = SELECT_INTERRUPTIBLE;

    if (!count || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER_1;

    if (do_fsync())
    {
        NTSTATUS ret = fsync_wait_objects( count, handles, wait_any, alertable, timeout );
        if (ret != STATUS_NOT_IMPLEMENTED)
            return ret;
    }

    if (do_esync())
    {
        NTSTATUS ret = esync_wait_objects( count, handles, wait_any, alertable, timeout );
        if (ret != STATUS_NOT_IMPLEMENTED)
            return ret;
    }

    if (alertable) flags |= SELECT_ALERTABLE;
    select_op.wait.op = wait_any ? SELECT_WAIT : SELECT_WAIT_ALL;
    for (i = 0; i < count; i++) select_op.wait.handles[i] = wine_server_obj_handle( handles[i] );
    return server_wait( &select_op, offsetof( select_op_t, wait.handles[count] ), flags, timeout );
}


/******************************************************************
 *		NtWaitForSingleObject (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForSingleObject( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    return NtWaitForMultipleObjects( 1, &handle, FALSE, alertable, timeout );
}


/******************************************************************
 *		NtSignalAndWaitForSingleObject (NTDLL.@)
 */
NTSTATUS WINAPI NtSignalAndWaitForSingleObject( HANDLE signal, HANDLE wait,
                                                BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    select_op_t select_op;
    UINT flags = SELECT_INTERRUPTIBLE;

    if (do_fsync())
        return fsync_signal_and_wait( signal, wait, alertable, timeout );

    if (do_esync())
        return esync_signal_and_wait( signal, wait, alertable, timeout );

    if (!signal) return STATUS_INVALID_HANDLE;

    if (alertable) flags |= SELECT_ALERTABLE;
    select_op.signal_and_wait.op = SELECT_SIGNAL_AND_WAIT;
    select_op.signal_and_wait.wait = wine_server_obj_handle( wait );
    select_op.signal_and_wait.signal = wine_server_obj_handle( signal );
    return server_wait( &select_op, sizeof(select_op.signal_and_wait), flags, timeout );
}


/******************************************************************
 *		NtYieldExecution (NTDLL.@)
 */
NTSTATUS WINAPI NtYieldExecution(void)
{
#ifdef HAVE_SCHED_YIELD
    sched_yield();
    return STATUS_SUCCESS;
#else
    return STATUS_NO_YIELD_PERFORMED;
#endif
}


/******************************************************************
 *		NtDelayExecution (NTDLL.@)
 */
NTSTATUS WINAPI NtDelayExecution( BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    /* if alertable, we need to query the server */
    if (alertable) return server_wait( NULL, 0, SELECT_INTERRUPTIBLE | SELECT_ALERTABLE, timeout );

    if (!timeout || timeout->QuadPart == TIMEOUT_INFINITE)  /* sleep forever */
    {
        for (;;) select( 0, NULL, NULL, NULL, NULL );
    }
    else
    {
        LARGE_INTEGER now;
        timeout_t when, diff;

        if ((when = timeout->QuadPart) < 0)
        {
            NtQuerySystemTime( &now );
            when = now.QuadPart - when;
        }

        /* Note that we yield after establishing the desired timeout */
        NtYieldExecution();
        if (!when) return STATUS_SUCCESS;

        for (;;)
        {
            struct timeval tv;
            NtQuerySystemTime( &now );
            diff = (when - now.QuadPart + 9) / 10;
            if (diff <= 0) break;
            tv.tv_sec  = diff / 1000000;
            tv.tv_usec = diff % 1000000;
            if (select( 0, NULL, NULL, NULL, &tv ) != -1) break;
        }
    }
    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtQueryPerformanceCounter (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryPerformanceCounter( LARGE_INTEGER *counter, LARGE_INTEGER *frequency )
{
    counter->QuadPart = monotonic_counter();
    if (frequency) frequency->QuadPart = TICKSPERSEC;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtQuerySystemTime (NTDLL.@)
 */
NTSTATUS WINAPI NtQuerySystemTime( LARGE_INTEGER *time )
{
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;
    static clockid_t clock_id = CLOCK_MONOTONIC; /* placeholder */

    if (clock_id == CLOCK_MONOTONIC)
    {
#ifdef CLOCK_REALTIME_COARSE
        struct timespec res;

        /* Use CLOCK_REALTIME_COARSE if it has 1 ms or better resolution */
        if (!clock_getres( CLOCK_REALTIME_COARSE, &res ) && res.tv_sec == 0 && res.tv_nsec <= 1000000)
            clock_id = CLOCK_REALTIME_COARSE;
        else
#endif /* CLOCK_REALTIME_COARSE */
            clock_id = CLOCK_REALTIME;
    }

    if (!clock_gettime( clock_id, &ts ))
    {
        time->QuadPart = ticks_from_time_t( ts.tv_sec ) + (ts.tv_nsec + 50) / 100;
    }
    else
#endif /* HAVE_CLOCK_GETTIME */
    {
        struct timeval now;

        gettimeofday( &now, 0 );
        time->QuadPart = ticks_from_time_t( now.tv_sec ) + now.tv_usec * 10;
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtSetSystemTime (NTDLL.@)
 */
NTSTATUS WINAPI NtSetSystemTime( const LARGE_INTEGER *new, LARGE_INTEGER *old )
{
    LARGE_INTEGER now;
    LONGLONG diff;

    NtQuerySystemTime( &now );
    if (old) *old = now;
    diff = new->QuadPart - now.QuadPart;
    if (diff > -TICKSPERSEC / 2 && diff < TICKSPERSEC / 2) return STATUS_SUCCESS;
    ERR( "not allowed: difference %d ms\n", (int)(diff / 10000) );
    return STATUS_PRIVILEGE_NOT_HELD;
}


/***********************************************************************
 *              NtQueryTimerResolution (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryTimerResolution( ULONG *min_res, ULONG *max_res, ULONG *current_res )
{
    TRACE( "(%p,%p,%p)\n", min_res, max_res, current_res );
    *max_res = *current_res = 10000; /* See NtSetTimerResolution() */
    *min_res = 156250;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtSetTimerResolution (NTDLL.@)
 */
NTSTATUS WINAPI NtSetTimerResolution( ULONG res, BOOLEAN set, ULONG *current_res )
{
    static BOOL has_request = FALSE;

    TRACE( "(%u,%u,%p), semi-stub!\n", (int)res, set, current_res );

    /* Wine has no support for anything other that 1 ms and does not keep of
     * track resolution requests anyway.
     * Fortunately NtSetTimerResolution() should ignore requests to lower the
     * timer resolution. So by claiming that 'some other process' requested the
     * max resolution already, there no need to actually change it.
     */
    *current_res = 10000;

    /* Just keep track of whether this process requested a specific timer
     * resolution.
     */
    if (!has_request && !set)
        return STATUS_TIMER_RESOLUTION_NOT_SET;
    has_request = set;

    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtSetIntervalProfile (NTDLL.@)
 */
NTSTATUS WINAPI NtSetIntervalProfile( ULONG interval, KPROFILE_SOURCE source )
{
    FIXME( "%u,%d\n", (int)interval, source );
    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtGetTickCount (NTDLL.@)
 */
ULONG WINAPI NtGetTickCount(void)
{
    /* note: we ignore TickCountMultiplier */
    return user_shared_data->u.TickCount.LowPart;
}


/******************************************************************************
 *              RtlGetSystemTimePrecise (NTDLL.@)
 */
NTSTATUS system_time_precise( void *args )
{
    LONGLONG *ret = args;
    struct timeval now;
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;

    if (!clock_gettime( CLOCK_REALTIME, &ts ))
    {
        *ret = ticks_from_time_t( ts.tv_sec ) + (ts.tv_nsec + 50) / 100;
        return STATUS_SUCCESS;
    }
#endif
    gettimeofday( &now, 0 );
    *ret = ticks_from_time_t( now.tv_sec ) + now.tv_usec * 10;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtCreateKeyedEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateKeyedEvent( HANDLE *handle, ACCESS_MASK access,
                                    const OBJECT_ATTRIBUTES *attr, ULONG flags )
{
    unsigned int ret;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;
    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_keyed_event )
    {
        req->access = access;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/******************************************************************************
 *              NtOpenKeyedEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenKeyedEvent( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_keyed_event )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}

/******************************************************************************
 *              NtWaitForKeyedEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForKeyedEvent( HANDLE handle, const void *key,
                                     BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    select_op_t select_op;
    UINT flags = SELECT_INTERRUPTIBLE;

    if (!handle) handle = keyed_event;
    if ((ULONG_PTR)key & 1) return STATUS_INVALID_PARAMETER_1;
    if (alertable) flags |= SELECT_ALERTABLE;
    select_op.keyed_event.op     = SELECT_KEYED_EVENT_WAIT;
    select_op.keyed_event.handle = wine_server_obj_handle( handle );
    select_op.keyed_event.key    = wine_server_client_ptr( key );
    return server_wait( &select_op, sizeof(select_op.keyed_event), flags, timeout );
}


/******************************************************************************
 *              NtReleaseKeyedEvent (NTDLL.@)
 */
NTSTATUS WINAPI NtReleaseKeyedEvent( HANDLE handle, const void *key,
                                     BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    select_op_t select_op;
    UINT flags = SELECT_INTERRUPTIBLE;

    if (!handle) handle = keyed_event;
    if ((ULONG_PTR)key & 1) return STATUS_INVALID_PARAMETER_1;
    if (alertable) flags |= SELECT_ALERTABLE;
    select_op.keyed_event.op     = SELECT_KEYED_EVENT_RELEASE;
    select_op.keyed_event.handle = wine_server_obj_handle( handle );
    select_op.keyed_event.key    = wine_server_client_ptr( key );
    return server_wait( &select_op, sizeof(select_op.keyed_event), flags, timeout );
}


/***********************************************************************
 *             NtCreateIoCompletion (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateIoCompletion( HANDLE *handle, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr,
                                      ULONG threads )
{
    unsigned int status;
    data_size_t len;
    struct object_attributes *objattr;

    TRACE( "(%p, %x, %p, %d)\n", handle, (int)access, attr, (int)threads );

    *handle = 0;
    if ((status = alloc_object_attributes( attr, &objattr, &len ))) return status;

    SERVER_START_REQ( create_completion )
    {
        req->access     = access;
        req->concurrent = threads;
        wine_server_add_data( req, objattr, len );
        if (!(status = wine_server_call( req ))) *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return status;
}


/***********************************************************************
 *             NtOpenIoCompletion (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenIoCompletion( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int status;

    *handle = 0;
    if ((status = validate_open_object_attributes( attr ))) return status;

    SERVER_START_REQ( open_completion )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        status = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return status;
}


/***********************************************************************
 *             NtSetIoCompletion (NTDLL.@)
 */
NTSTATUS WINAPI NtSetIoCompletion( HANDLE handle, ULONG_PTR key, ULONG_PTR value,
                                   NTSTATUS status, SIZE_T count )
{
    unsigned int ret;

    TRACE( "(%p, %lx, %lx, %x, %lx)\n", handle, key, value, (int)status, count );

    SERVER_START_REQ( add_completion )
    {
        req->handle      = wine_server_obj_handle( handle );
        req->ckey        = key;
        req->cvalue      = value;
        req->status      = status;
        req->information = count;
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *             NtRemoveIoCompletion (NTDLL.@)
 */
NTSTATUS WINAPI NtRemoveIoCompletion( HANDLE handle, ULONG_PTR *key, ULONG_PTR *value,
                                      IO_STATUS_BLOCK *io, LARGE_INTEGER *timeout )
{
    unsigned int status;

    TRACE( "(%p, %p, %p, %p, %p)\n", handle, key, value, io, timeout );

    for (;;)
    {
        SERVER_START_REQ( remove_completion )
        {
            req->handle = wine_server_obj_handle( handle );
            if (!(status = wine_server_call( req )))
            {
                *key            = reply->ckey;
                *value          = reply->cvalue;
                io->Information = reply->information;
                io->u.Status    = reply->status;
            }
        }
        SERVER_END_REQ;
        if (status != STATUS_PENDING) return status;
        status = NtWaitForSingleObject( handle, FALSE, timeout );
        if (status != WAIT_OBJECT_0) return status;
    }
}


/***********************************************************************
 *             NtRemoveIoCompletionEx (NTDLL.@)
 */
NTSTATUS WINAPI NtRemoveIoCompletionEx( HANDLE handle, FILE_IO_COMPLETION_INFORMATION *info, ULONG count,
                                        ULONG *written, LARGE_INTEGER *timeout, BOOLEAN alertable )
{
    unsigned int status;
    ULONG i = 0;

    TRACE( "%p %p %u %p %p %u\n", handle, info, (int)count, written, timeout, alertable );

    for (;;)
    {
        while (i < count)
        {
            SERVER_START_REQ( remove_completion )
            {
                req->handle = wine_server_obj_handle( handle );
                if (!(status = wine_server_call( req )))
                {
                    info[i].CompletionKey             = reply->ckey;
                    info[i].CompletionValue           = reply->cvalue;
                    info[i].IoStatusBlock.Information = reply->information;
                    info[i].IoStatusBlock.u.Status    = reply->status;
                }
            }
            SERVER_END_REQ;
            if (status != STATUS_SUCCESS) break;
            ++i;
        }
        if (i || status != STATUS_PENDING)
        {
            if (status == STATUS_PENDING) status = STATUS_SUCCESS;
            break;
        }
        status = NtWaitForSingleObject( handle, alertable, timeout );
        if (status != WAIT_OBJECT_0) break;
    }
    *written = i ? i : 1;
    return status;
}


/***********************************************************************
 *             NtQueryIoCompletion (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryIoCompletion( HANDLE handle, IO_COMPLETION_INFORMATION_CLASS class,
                                     void *buffer, ULONG len, ULONG *ret_len )
{
    unsigned int status;

    TRACE( "(%p, %d, %p, 0x%x, %p)\n", handle, class, buffer, (int)len, ret_len );

    if (!buffer) return STATUS_INVALID_PARAMETER;

    switch (class)
    {
    case IoCompletionBasicInformation:
    {
        ULONG *info = buffer;
        if (ret_len) *ret_len = sizeof(*info);
        if (len == sizeof(*info))
        {
            SERVER_START_REQ( query_completion )
            {
                req->handle = wine_server_obj_handle( handle );
                if (!(status = wine_server_call( req ))) *info = reply->depth;
            }
            SERVER_END_REQ;
        }
        else status = STATUS_INFO_LENGTH_MISMATCH;
        break;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return status;
}


/***********************************************************************
 *             NtCreateSection (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateSection( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                 const LARGE_INTEGER *size, ULONG protect,
                                 ULONG sec_flags, HANDLE file )
{
    unsigned int ret;
    unsigned int file_access;
    data_size_t len;
    struct object_attributes *objattr;

    *handle = 0;

    switch (protect & 0xff)
    {
    case PAGE_READONLY:
    case PAGE_EXECUTE_READ:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_WRITECOPY:
        file_access = FILE_READ_DATA;
        break;
    case PAGE_READWRITE:
    case PAGE_EXECUTE_READWRITE:
        if (sec_flags & SEC_IMAGE) file_access = FILE_READ_DATA;
        else file_access = FILE_READ_DATA | FILE_WRITE_DATA;
        break;
    case PAGE_EXECUTE:
    case PAGE_NOACCESS:
        file_access = 0;
        break;
    default:
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    SERVER_START_REQ( create_mapping )
    {
        req->access      = access;
        req->flags       = sec_flags;
        req->file_handle = wine_server_obj_handle( file );
        req->file_access = file_access;
        req->size        = size ? size->QuadPart : 0;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    free( objattr );
    return ret;
}


/***********************************************************************
 *             NtOpenSection (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenSection( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    *handle = 0;
    if ((ret = validate_open_object_attributes( attr ))) return ret;

    SERVER_START_REQ( open_mapping )
    {
        req->access     = access;
        req->attributes = attr->Attributes;
        req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
        if (attr->ObjectName)
            wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *             NtCreatePort (NTDLL.@)
 */
NTSTATUS WINAPI NtCreatePort( HANDLE *handle, OBJECT_ATTRIBUTES *attr, ULONG info_len,
                              ULONG data_len, ULONG *reserved )
{
    FIXME( "(%p,%p,%u,%u,%p),stub!\n", handle, attr, (int)info_len, (int)data_len, reserved );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtConnectPort (NTDLL.@)
 */
NTSTATUS WINAPI NtConnectPort( HANDLE *handle, UNICODE_STRING *name, SECURITY_QUALITY_OF_SERVICE *qos,
                               LPC_SECTION_WRITE *write, LPC_SECTION_READ *read, ULONG *max_len,
                               void *info, ULONG *info_len )
{
    FIXME( "(%p,%s,%p,%p,%p,%p,%p,%p),stub!\n", handle, debugstr_us(name), qos,
           write, read, max_len, info, info_len );
    if (info && info_len) TRACE("msg = %s\n", debugstr_an( info, *info_len ));
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtSecureConnectPort (NTDLL.@)
 */
NTSTATUS WINAPI NtSecureConnectPort( HANDLE *handle, UNICODE_STRING *name, SECURITY_QUALITY_OF_SERVICE *qos,
                                     LPC_SECTION_WRITE *write, PSID sid, LPC_SECTION_READ *read,
                                     ULONG *max_len, void *info, ULONG *info_len )
{
    FIXME( "(%p,%s,%p,%p,%p,%p,%p,%p,%p),stub!\n", handle, debugstr_us(name), qos,
           write, sid, read, max_len, info, info_len );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtListenPort (NTDLL.@)
 */
NTSTATUS WINAPI NtListenPort( HANDLE handle, LPC_MESSAGE *msg )
{
    FIXME("(%p,%p),stub!\n", handle, msg );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtAcceptConnectPort (NTDLL.@)
 */
NTSTATUS WINAPI NtAcceptConnectPort( HANDLE *handle, ULONG id, LPC_MESSAGE *msg, BOOLEAN accept,
                                     LPC_SECTION_WRITE *write, LPC_SECTION_READ *read )
{
    FIXME("(%p,%u,%p,%d,%p,%p),stub!\n", handle, (int)id, msg, accept, write, read );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtCompleteConnectPort (NTDLL.@)
 */
NTSTATUS WINAPI NtCompleteConnectPort( HANDLE handle )
{
    FIXME( "(%p),stub!\n", handle );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtRegisterThreadTerminatePort (NTDLL.@)
 */
NTSTATUS WINAPI NtRegisterThreadTerminatePort( HANDLE handle )
{
    FIXME( "(%p),stub!\n", handle );
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtRequestWaitReplyPort (NTDLL.@)
 */
NTSTATUS WINAPI NtRequestWaitReplyPort( HANDLE handle, LPC_MESSAGE *msg_in, LPC_MESSAGE *msg_out )
{
    FIXME( "(%p,%p,%p),stub!\n", handle, msg_in, msg_out );
    if (msg_in)
        TRACE("datasize %u msgsize %u type %u ranges %u client %p/%p msgid %lu size %lu data %s\n",
              msg_in->DataSize, msg_in->MessageSize, msg_in->MessageType, msg_in->VirtualRangesOffset,
              msg_in->ClientId.UniqueProcess, msg_in->ClientId.UniqueThread, msg_in->MessageId,
              msg_in->SectionSize, debugstr_an( (const char *)msg_in->Data, msg_in->DataSize ));
    return STATUS_NOT_IMPLEMENTED;
}


/***********************************************************************
 *             NtReplyWaitReceivePort (NTDLL.@)
 */
NTSTATUS WINAPI NtReplyWaitReceivePort( HANDLE handle, ULONG *id, LPC_MESSAGE *reply, LPC_MESSAGE *msg )
{
    FIXME("(%p,%p,%p,%p),stub!\n", handle, id, reply, msg );
    return STATUS_NOT_IMPLEMENTED;
}


#define MAX_ATOM_LEN  255
#define IS_INTATOM(x) (((ULONG_PTR)(x) >> 16) == 0)

static unsigned int is_integral_atom( const WCHAR *atomstr, ULONG len, RTL_ATOM *ret_atom )
{
    RTL_ATOM atom;

    if ((ULONG_PTR)atomstr >> 16)
    {
        const WCHAR* ptr = atomstr;
        if (!len) return STATUS_OBJECT_NAME_INVALID;

        if (*ptr++ == '#')
        {
            atom = 0;
            while (ptr < atomstr + len && *ptr >= '0' && *ptr <= '9')
            {
                atom = atom * 10 + *ptr++ - '0';
            }
            if (ptr > atomstr + 1 && ptr == atomstr + len) goto done;
        }
        if (len > MAX_ATOM_LEN) return STATUS_INVALID_PARAMETER;
        return STATUS_MORE_ENTRIES;
    }
    else atom = LOWORD( atomstr );
done:
    if (!atom || atom >= MAXINTATOM) return STATUS_INVALID_PARAMETER;
    *ret_atom = atom;
    return STATUS_SUCCESS;
}

static ULONG integral_atom_name( WCHAR *buffer, ULONG len, RTL_ATOM atom )
{
    char tmp[16];
    int ret = sprintf( tmp, "#%u", atom );

    len /= sizeof(WCHAR);
    if (len)
    {
        if (len <= ret) ret = len - 1;
        ascii_to_unicode( buffer, tmp, ret );
        buffer[ret] = 0;
    }
    return ret * sizeof(WCHAR);
}


/***********************************************************************
 *             NtAddAtom (NTDLL.@)
 */
NTSTATUS WINAPI NtAddAtom( const WCHAR *name, ULONG length, RTL_ATOM *atom )
{
    unsigned int status = is_integral_atom( name, length / sizeof(WCHAR), atom );

    if (status == STATUS_MORE_ENTRIES)
    {
        SERVER_START_REQ( add_atom )
        {
            wine_server_add_data( req, name, length );
            status = wine_server_call( req );
            *atom = reply->atom;
        }
        SERVER_END_REQ;
    }
    TRACE( "%s -> %x\n", debugstr_wn(name, length/sizeof(WCHAR)), status == STATUS_SUCCESS ? *atom : 0 );
    return status;
}


/***********************************************************************
 *             NtDeleteAtom (NTDLL.@)
 */
NTSTATUS WINAPI NtDeleteAtom( RTL_ATOM atom )
{
    unsigned int status;

    SERVER_START_REQ( delete_atom )
    {
        req->atom = atom;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}


/***********************************************************************
 *             NtFindAtom (NTDLL.@)
 */
NTSTATUS WINAPI NtFindAtom( const WCHAR *name, ULONG length, RTL_ATOM *atom )
{
    unsigned int status = is_integral_atom( name, length / sizeof(WCHAR), atom );

    if (status == STATUS_MORE_ENTRIES)
    {
        SERVER_START_REQ( find_atom )
        {
            wine_server_add_data( req, name, length );
            status = wine_server_call( req );
            *atom = reply->atom;
        }
        SERVER_END_REQ;
    }
    TRACE( "%s -> %x\n", debugstr_wn(name, length/sizeof(WCHAR)), status == STATUS_SUCCESS ? *atom : 0 );
    return status;
}


/***********************************************************************
 *             NtQueryInformationAtom (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryInformationAtom( RTL_ATOM atom, ATOM_INFORMATION_CLASS class,
                                        void *ptr, ULONG size, ULONG *retsize )
{
    unsigned int status;

    switch (class)
    {
    case AtomBasicInformation:
    {
        ULONG name_len;
        ATOM_BASIC_INFORMATION *abi = ptr;

        if (size < sizeof(ATOM_BASIC_INFORMATION)) return STATUS_INVALID_PARAMETER;
        name_len = size - sizeof(ATOM_BASIC_INFORMATION);

        if (atom < MAXINTATOM)
        {
            if (atom)
            {
                abi->NameLength = integral_atom_name( abi->Name, name_len, atom );
                status = name_len ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
                abi->ReferenceCount = 1;
                abi->Pinned = 1;
            }
            else status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            SERVER_START_REQ( get_atom_information )
            {
                req->atom = atom;
                if (name_len) wine_server_set_reply( req, abi->Name, name_len );
                status = wine_server_call( req );
                if (status == STATUS_SUCCESS)
                {
                    name_len = wine_server_reply_size( reply );
                    if (name_len)
                    {
                        abi->NameLength = name_len;
                        abi->Name[name_len / sizeof(WCHAR)] = 0;
                    }
                    else
                    {
                        name_len = reply->total;
                        abi->NameLength = name_len;
                        status = STATUS_BUFFER_TOO_SMALL;
                    }
                    abi->ReferenceCount = reply->count;
                    abi->Pinned = reply->pinned;
                }
                else name_len = 0;
            }
            SERVER_END_REQ;
        }
        TRACE( "%x -> %s (%u)\n", atom, debugstr_wn(abi->Name, abi->NameLength / sizeof(WCHAR)), status );
        if (retsize) *retsize = sizeof(ATOM_BASIC_INFORMATION) + name_len;
        break;
    }

    default:
        FIXME( "Unsupported class %u\n", class );
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    return status;
}


union tid_alert_entry
{
#ifdef __APPLE__
    semaphore_t sem;
#else
    HANDLE event;
#ifdef __linux__
    LONG futex;
#endif
#endif
};

#define TID_ALERT_BLOCK_SIZE (65536 / sizeof(union tid_alert_entry))
static union tid_alert_entry *tid_alert_blocks[4096];

static unsigned int handle_to_index( HANDLE handle, unsigned int *block_idx )
{
    unsigned int idx = (wine_server_obj_handle(handle) >> 2) - 1;
    *block_idx = idx / TID_ALERT_BLOCK_SIZE;
    return idx % TID_ALERT_BLOCK_SIZE;
}

static union tid_alert_entry *get_tid_alert_entry( HANDLE tid )
{
    unsigned int block_idx, idx = handle_to_index( tid, &block_idx );
    union tid_alert_entry *entry;

    if (block_idx > ARRAY_SIZE(tid_alert_blocks))
    {
        FIXME( "tid %p is too high\n", tid );
        return NULL;
    }

    if (!tid_alert_blocks[block_idx])
    {
        static const size_t size = TID_ALERT_BLOCK_SIZE * sizeof(union tid_alert_entry);
        void *ptr = anon_mmap_alloc( size, PROT_READ | PROT_WRITE );
        if (ptr == MAP_FAILED) return NULL;
        if (InterlockedCompareExchangePointer( (void **)&tid_alert_blocks[block_idx], ptr, NULL ))
            munmap( ptr, size ); /* someone beat us to it */
    }

    entry = &tid_alert_blocks[block_idx][idx % TID_ALERT_BLOCK_SIZE];

#ifdef __APPLE__
    if (!entry->sem)
    {
        semaphore_t sem;

        if (semaphore_create( mach_task_self(), &sem, SYNC_POLICY_FIFO, 0 ))
            return NULL;
        if (InterlockedCompareExchange( (LONG *)&entry->sem, sem, 0 ))
            semaphore_destroy( mach_task_self(), sem );
    }
#else
#ifdef __linux__
    if (use_futexes())
        return entry;
#endif

    if (!entry->event)
    {
        HANDLE event;

        if (NtCreateEvent( &event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE ))
            return NULL;
        if (InterlockedCompareExchangePointer( &entry->event, event, NULL ))
            NtClose( event );
    }
#endif

    return entry;
}


/***********************************************************************
 *             NtAlertThreadByThreadId (NTDLL.@)
 */
NTSTATUS WINAPI NtAlertThreadByThreadId( HANDLE tid )
{
    union tid_alert_entry *entry = get_tid_alert_entry( tid );

    TRACE( "%p\n", tid );

    if (!entry) return STATUS_INVALID_CID;

#ifdef __APPLE__
    semaphore_signal( entry->sem );
    return STATUS_SUCCESS;
#else
#ifdef __linux__
    if (use_futexes())
    {
        LONG *futex = &entry->futex;
        if (!InterlockedExchange( futex, 1 ))
            futex_wake( futex, 1 );
        return STATUS_SUCCESS;
    }
#endif

    return NtSetEvent( entry->event, NULL );
#endif
}


#if defined(__linux__) || defined(__APPLE__)
static LONGLONG get_absolute_timeout( const LARGE_INTEGER *timeout )
{
    LARGE_INTEGER now;

    if (timeout->QuadPart >= 0) return timeout->QuadPart;
    NtQuerySystemTime( &now );
    return now.QuadPart - timeout->QuadPart;
}

static LONGLONG update_timeout( ULONGLONG end )
{
    LARGE_INTEGER now;
    LONGLONG timeleft;

    NtQuerySystemTime( &now );
    timeleft = end - now.QuadPart;
    if (timeleft < 0) timeleft = 0;
    return timeleft;
}
#endif


#ifdef __APPLE__

/***********************************************************************
 *             NtWaitForAlertByThreadId (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForAlertByThreadId( const void *address, const LARGE_INTEGER *timeout )
{
    union tid_alert_entry *entry = get_tid_alert_entry( NtCurrentTeb()->ClientId.UniqueThread );
    semaphore_t sem;
    ULONGLONG end;
    kern_return_t ret;

    TRACE( "%p %s\n", address, debugstr_timeout( timeout ) );

    if (!entry) return STATUS_INVALID_CID;
    sem = entry->sem;

    if (timeout)
    {
        if (timeout->QuadPart == TIMEOUT_INFINITE)
            timeout = NULL;
        else
            end = get_absolute_timeout( timeout );
    }

    for (;;)
    {
        if (timeout)
        {
            LONGLONG timeleft = update_timeout( end );
            mach_timespec_t timespec;

            timespec.tv_sec = timeleft / (ULONGLONG)TICKSPERSEC;
            timespec.tv_nsec = (timeleft % TICKSPERSEC) * 100;
            ret = semaphore_timedwait( sem, timespec );
        }
        else
            ret = semaphore_wait( sem );

        switch (ret)
        {
        case KERN_SUCCESS: return STATUS_ALERTED;
        case KERN_ABORTED: continue;
        case KERN_OPERATION_TIMED_OUT: return STATUS_TIMEOUT;
        default: return STATUS_INVALID_HANDLE;
        }
    }
}

#else

/***********************************************************************
 *             NtWaitForAlertByThreadId (NTDLL.@)
 */
NTSTATUS WINAPI NtWaitForAlertByThreadId( const void *address, const LARGE_INTEGER *timeout )
{
    union tid_alert_entry *entry = get_tid_alert_entry( NtCurrentTeb()->ClientId.UniqueThread );
    NTSTATUS status;

    TRACE( "%p %s\n", address, debugstr_timeout( timeout ) );

    if (!entry) return STATUS_INVALID_CID;

#ifdef __linux__
    if (use_futexes())
    {
        LONG *futex = &entry->futex;
        ULONGLONG end;
        int ret;

        if (timeout)
        {
            if (timeout->QuadPart == TIMEOUT_INFINITE)
                timeout = NULL;
            else
                end = get_absolute_timeout( timeout );
        }

        while (!InterlockedExchange( futex, 0 ))
        {
            if (timeout)
            {
                LONGLONG timeleft = update_timeout( end );
                struct timespec timespec;

                timespec.tv_sec = timeleft / (ULONGLONG)TICKSPERSEC;
                timespec.tv_nsec = (timeleft % TICKSPERSEC) * 100;
                ret = futex_wait( futex, 0, &timespec );
            }
            else
                ret = futex_wait( futex, 0, NULL );

            if (ret == -1 && errno == ETIMEDOUT) return STATUS_TIMEOUT;
        }
        return STATUS_ALERTED;
    }
#endif

    status = NtWaitForSingleObject( entry->event, FALSE, timeout );
    if (!status) return STATUS_ALERTED;
    return status;
}

#endif

/* Notify direct completion of async and close the wait handle if it is no longer needed.
 * This function is a no-op (returns status as-is) if the supplied handle is NULL.
 */
void set_async_direct_result( HANDLE *async_handle, NTSTATUS status, ULONG_PTR information, BOOL mark_pending )
{
    unsigned int ret;

    /* if we got STATUS_ALERTED, we must have a valid async handle */
    assert( *async_handle );

    SERVER_START_REQ( set_async_direct_result )
    {
        req->handle       = wine_server_obj_handle( *async_handle );
        req->status       = status;
        req->information  = information;
        req->mark_pending = mark_pending;
        ret = wine_server_call( req );
        if (ret == STATUS_SUCCESS)
            *async_handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    if (ret != STATUS_SUCCESS)
        ERR( "cannot report I/O result back to server: %08x\n", ret );

    return;
}
