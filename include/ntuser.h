/*
 * Copyright 2021 Jacek Caban for CodeWeavers
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

#ifndef _NTUSER_
#define _NTUSER_

#include <winuser.h>
#include <wingdi.h>
#include <winternl.h>

BOOL    WINAPI NtUserAddClipboardFormatListener( HWND hwnd );
BOOL    WINAPI NtUserAttachThreadInput( DWORD from, DWORD to, BOOL attach );
BOOL    WINAPI NtUserCloseDesktop( HDESK handle );
BOOL    WINAPI NtUserCloseWindowStation( HWINSTA handle );
INT     WINAPI NtUserCountClipboardFormats(void);
HDESK   WINAPI NtUserCreateDesktopEx( OBJECT_ATTRIBUTES *attr, UNICODE_STRING *device,
                                      DEVMODEW *devmode, DWORD flags, ACCESS_MASK access,
                                      ULONG heap_size );
HWINSTA WINAPI NtUserCreateWindowStation( OBJECT_ATTRIBUTES *attr, ACCESS_MASK mask, ULONG arg3,
                                          ULONG arg4, ULONG arg5, ULONG arg6, ULONG arg7 );
INT     WINAPI NtUserGetClipboardFormatName( UINT format, WCHAR *buffer, INT maxlen );
HWND    WINAPI NtUserGetClipboardOwner(void);
DWORD   WINAPI NtUserGetClipboardSequenceNumber(void);
HWND    WINAPI NtUserGetClipboardViewer(void);
INT     WINAPI NtUserGetKeyNameText( LONG lparam, WCHAR *buffer, INT size );
SHORT   WINAPI NtUserGetKeyState( INT vkey );
HKL     WINAPI NtUserGetKeyboardLayout( DWORD thread_id );
BOOL    WINAPI NtUserGetKeyboardState( BYTE *state );
BOOL    WINAPI NtUserGetLayeredWindowAttributes( HWND hwnd, COLORREF *key, BYTE *alpha, DWORD *flags );
BOOL    WINAPI NtUserGetObjectInformation( HANDLE handle, INT index, void *info,
                                           DWORD len, DWORD *needed );
INT     WINAPI NtUserGetPriorityClipboardFormat( UINT *list, INT count );
HWINSTA WINAPI NtUserGetProcessWindowStation(void);
HANDLE  WINAPI NtUserGetProp( HWND hwnd, const WCHAR *str );
HDESK   WINAPI NtUserGetThreadDesktop( DWORD thread );
BOOL    WINAPI NtUserGetUpdatedClipboardFormats( UINT *formats, UINT size, UINT *out_size );
BOOL    WINAPI NtUserIsClipboardFormatAvailable( UINT format );
UINT    WINAPI NtUserMapVirtualKeyEx( UINT code, UINT type, HKL layout );
HWINSTA WINAPI NtUserOpenWindowStation( OBJECT_ATTRIBUTES *attr, ACCESS_MASK access );
BOOL    WINAPI NtUserSetObjectInformation( HANDLE handle, INT index, void *info, DWORD len );
HDESK   WINAPI NtUserOpenDesktop( OBJECT_ATTRIBUTES *attr, DWORD flags, ACCESS_MASK access );
HDESK   WINAPI NtUserOpenInputDesktop( DWORD flags, BOOL inherit, ACCESS_MASK access );
BOOL    WINAPI NtUserRemoveClipboardFormatListener( HWND hwnd );
HANDLE  WINAPI NtUserRemoveProp( HWND hwnd, const WCHAR *str );
BOOL    WINAPI NtUserSetKeyboardState( BYTE *state );
BOOL    WINAPI NtUserSetProcessWindowStation( HWINSTA handle );
BOOL    WINAPI NtUserSetProp( HWND hwnd, const WCHAR *str, HANDLE handle );
BOOL    WINAPI NtUserSetThreadDesktop( HDESK handle );
WORD    WINAPI NtUserVkKeyScanEx( WCHAR chr, HKL layout );

#endif /* _NTUSER_ */