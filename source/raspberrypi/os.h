//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2016-2021 GIMONS(Twitter:@kugimoto0715)
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//
//	[ OS固有 ]
//
//---------------------------------------------------------------------------

#if !defined(os_h)
#define os_h

//---------------------------------------------------------------------------
//
//	#define
//
//---------------------------------------------------------------------------
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#ifndef  _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

//---------------------------------------------------------------------------
//
//	#include
//
//---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <utime.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include <iconv.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifndef BAREMETAL
#include <poll.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <linux/gpio.h>
#else
#include <machine/endian.h>
#define	htonl(_x)	__htonl(_x)
#define	htons(_x)	__htons(_x)
#define	ntohl(_x)	__ntohl(_x)
#define	ntohs(_x)	__ntohs(_x)
#endif	// BAREMETAL

#if defined(__linux__)
#include <linux/if.h>
#include <linux/if_tun.h>
#elif defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_tap.h>
#include <ifaddrs.h>
#endif

//---------------------------------------------------------------------------
//
//	基本マクロ
//
//---------------------------------------------------------------------------
#undef FASTCALL
#define FASTCALL

#undef CDECL
#define CDECL

#undef INLINE
#define INLINE

#if !defined(ASSERT)
#if !defined(NDEBUG)
#define ASSERT(cond)	assert(cond)
#else
#define ASSERT(cond)	((void)0)
#endif	// NDEBUG
#endif	// ASSERT

#if !defined(ASSERT_DIAG)
#if !defined(NDEBUG)
#define ASSERT_DIAG()	AssertDiag()
#else
#define ASSERT_DIAG()	((void)0)
#endif	// NDEBUG
#endif	// ASSERT_DIAG

//---------------------------------------------------------------------------
//
//	基本型定義
//
//---------------------------------------------------------------------------
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long UL64;
typedef int BOOL;
typedef char TCHAR;
typedef char *LPTSTR;
typedef const char *LPCTSTR;
typedef const char *LPCSTR;

#if !defined(FALSE)
#define FALSE               0
#endif

#if !defined(TRUE)
#define TRUE                1
#endif

#if !defined(_T)
#define _T(x)	x
#endif

#define _MAX_PATH   260
#define _MAX_DRIVE  3
#define _MAX_DIR    256
#define _MAX_FNAME  256
#define _MAX_EXT    256

#define _xstrcasecmp strcasecmp
#define _xstrncasecmp strncasecmp

#if __ARM_ARCH == 6
#define ISB() asm ("mcr p15, 0, r0, c7, c5,  4");
#define DSB() asm ("mcr p15, 0, r0, c7, c10, 4");
#define DMB() asm ("mcr p15, 0, r0, c7, c10, 5");
#else
#define ISB() asm ("isb");
#define DSB() asm ("dsb");
#define DMB() asm ("dmb");
#endif

#define SEV() asm ("sev");
#define WFE() asm ("wfe");
#define WFI() asm ("wfi");

#ifdef BAREMETAL
#define memcpy(d,s,n)	fmemcpy(d,s,n)
#define memset(b,c,n)	fmemset(b,c,n)

#ifdef __cplusplus
extern "C" {
#endif
void *fmemcpy(void *buf1, const void *buf2, size_t n);
void *fmemset(void *buf, int ch, size_t n);
#ifdef __cplusplus
}
#endif
#endif	// BAREMETAL

#endif	// os_h
