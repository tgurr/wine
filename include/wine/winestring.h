#ifndef __WINE_WINE_WINESTRING_H
#define __WINE_WINE_WINESTRING_H

#include "windef.h"

INT16       WINAPI WideCharToLocal16(LPSTR,LPWSTR,INT16);
INT         WINAPI WideCharToLocal(LPSTR,LPWSTR,INT);
INT16       WINAPI LocalToWideChar16(LPWSTR,LPSTR,INT16);
INT         WINAPI LocalToWideChar(LPWSTR,LPSTR,INT);
INT         WINAPI lstrncmpA(LPCSTR,LPCSTR,INT);
INT         WINAPI lstrncmpW(LPCWSTR,LPCWSTR,INT);
#define     lstrncmp WINELIB_NAME_AW(lstrncmp)
INT         WINAPI lstrncmpiA(LPCSTR,LPCSTR,INT);
INT         WINAPI lstrncmpiW(LPCWSTR,LPCWSTR,INT);
#define     lstrncmpi WINELIB_NAME_AW(lstrncmpi)
LPWSTR      WINAPI lstrcpyAtoW(LPWSTR,LPCSTR);
LPSTR       WINAPI lstrcpyWtoA(LPSTR,LPCWSTR);
LPWSTR      WINAPI lstrcpynAtoW(LPWSTR,LPCSTR,INT);
LPSTR       WINAPI lstrcpynWtoA(LPSTR,LPCWSTR,INT);

#endif /* __WINE_WINE_WINESTRING_H */
