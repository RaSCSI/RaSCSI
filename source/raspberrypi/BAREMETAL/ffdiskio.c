/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "rpi-SmartStart.h"
#include "emb-stdio.h"
#include "ff.h"					/* Obtains integer types */
#include "ffdiskio.h"			/* Declarations of disk functions */
#include "SDCard.h"

/* Definitions of physical drive number for each drive */
#define DEV_MMC		0	/* Example: Map MMC/SD card to physical drive 0 */
#define DEV_USB		1	/* Example: Map USB MSD to physical drive 1 */

#define CACHE_ENABLE
#ifdef CACHE_ENABLE
#define SECTOR_SIZE	512
#define CACHE_ENTRIES	(1 << 4)
#define CACHE_MASK		(CACHE_ENTRIES - 1)
static DWORD cached_entries[CACHE_ENTRIES];
static BYTE cache_buffer[SECTOR_SIZE * CACHE_ENTRIES];
#endif

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
	switch (pdrv) {
		case DEV_MMC :
			if (sdCardCSD() == NULL) {
				return STA_NOINIT;
			}
			return 0;
	
		case DEV_USB :
			return STA_NOINIT;
	}
	
	return STA_NODISK;
}

/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
	switch (pdrv) {
		case DEV_MMC :
			if (sdCardCSD() == NULL) {
				if (sdInitCard(NULL, &printf, false) != SD_OK) {
					return STA_NOINIT;
				}
#ifdef CACHE_ENABLE
				for (int i = 0; i < CACHE_ENTRIES; i++) {
					cached_entries[i] = 0xFFFFFFFF;
				}
#endif	// CACHE_ENABLE
			}
			return SD_OK;
	
		case DEV_USB :
			return STA_NOINIT;
	}
	return STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	DWORD sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	switch (pdrv) {
		case DEV_MMC :
#ifdef CACHE_ENABLE
			if (count == 1) {
				int index = sector & CACHE_MASK;
				void *cache_p = (void *)(cache_buffer + SECTOR_SIZE * index);

				if (cached_entries[index] != sector) {
					if (sdTransferBlocks(
						sector, count, (uint8_t *)cache_p, false) != SD_OK) {
						return RES_ERROR;
					}
					cached_entries[index] = sector;
				} else {
					// take sector from cache
				}

				memcpy(buff, cache_p, SECTOR_SIZE);
				return RES_OK;
			}
#endif	// CACHE_ENABLE
			if (sdTransferBlocks(sector, count, buff, false) != SD_OK) {
				return RES_ERROR;
			}

			return RES_OK;
	
		case DEV_USB :
			return RES_PARERR;
	}

	return RES_PARERR;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	DWORD sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
#ifdef CACHE_ENABLE
	int i;
	int j;
	int index;
#endif	// CACHE_ENABLE

	switch (pdrv) {
		case DEV_MMC :
			if (sdTransferBlocks(
				sector, count, (uint8_t*)buff, true) != SD_OK) {
				return RES_ERROR;
			}
#ifdef CACHE_ENABLE
			for (i = 0, j = 0; i < count && j < CACHE_ENTRIES; i++, j++) {
				index = (sector + i) & CACHE_MASK;
				cached_entries[index] = 0xFFFFFFFF;
			}
#endif	// CACHE_ENABLE
			return RES_OK;
	
		case DEV_USB :
			return RES_PARERR;
	}

	return RES_PARERR;
}

#endif

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	struct CSD* csd;

	switch (pdrv) {
	case DEV_MMC :
		csd = sdCardCSD();
		if (csd == NULL) {
			return RES_PARERR;
		}
		
		if (cmd == CTRL_SYNC) {
		    return RES_OK;
		}
		
		if (cmd == GET_SECTOR_COUNT) {
			if (csd->csd_structure == 0x1) {
				*(DWORD *)buff = csd->ver2_c_size * 1024;
			} else {
				*(DWORD *)buff =
					(csd->c_size + 1) * (1 << (csd->c_size_mult + 2));
			}
		    return RES_OK;
		}
		
		if (cmd == GET_SECTOR_SIZE) {
		    *(DWORD *)buff = 1 << csd->read_bl_len;
		    return RES_OK;
		}
		
		if (cmd == GET_BLOCK_SIZE) {
		    *(DWORD *)buff = 1 << csd->read_bl_len;
		    return RES_OK;
		}

		return RES_PARERR;

	case DEV_USB :
		return RES_PARERR;
	}

	return RES_PARERR;
}
