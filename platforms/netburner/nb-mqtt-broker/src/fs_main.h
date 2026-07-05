/* Revision: 3.5.8 */

/******************************************************************************
* Copyright 1998-2024 NetBurner, Inc.  ALL RIGHTS RESERVED
*
*    Permission is hereby granted to purchasers of NetBurner Hardware to use or
*    modify this computer program for any use as long as the resultant program
*    is only executed on NetBurner provided hardware.
*
*    No other rights to use this program or its derivatives in part or in
*    whole are granted.
*
*    It may be possible to license this or other NetBurner software for use on
*    non-NetBurner Hardware. Contact sales@Netburner.com for more information.
*
*    NetBurner makes no representation or warranties with respect to the
*    performance of this computer program, and specifically disclaims any
*    responsibility for any damages, special or consequential, connected with
*    the use of this program.
*
* NetBurner
* 16855 W Bernardo Dr
* San Diego, CA 92127
* www.netburner.com
******************************************************************************/

#ifndef __FS_MAIN_H
#define __FS_MAIN_H

#define USE_NOR

/* Drive numbers */
#define NOR_DRV_NUM 0
#define STDRAM_DRV_NUM 1
#define MMC_DRV_NUM 2
#define CFC_DRV_NUM 3
#define HDD_DRV_NUM 3
#define FATRAM_DRV_NUM 4

#if ((defined USE_NOR) | (defined USE_STDRAM)) & ((defined USE_CFC) | (defined USE_HDD) | (defined USE_MMC) | (defined USE_FATRAM))
#define FS_WRAPPER
#elif (defined USE_NOR) | (defined USE_STDRAM)
#define FS_STD
#elif (defined USE_CFC) | (defined USE_HDD) | (defined USE_MMC) | (defined USE_FATRAM)
#define FS_FAT
#endif

#include "file/fsf.h"
#define fd_mountstd fs_mountdrive
#define fd_format(d, t) fs_format(d)
#define fd_hardformat(d, t) fs_format(d)
#define fd_getdrive fs_getdrive
#define fd_chdrive fs_chdrive
#define fd_getcwd fs_getcwd
#define fd_chdir fs_chdir
#define fd_mkdir fs_mkdir
#define fd_rmdir fs_rmdir
#define fd_getfreespace fs_getfreespace
#define fd_findfirst fs_findfirst
#define fd_findnext fs_findnext
#define fd_filelength fs_filelength
#define fd_delete fs_delete
#define fd_rename fs_rename
#define fd_open fs_open
#define fd_close fs_close
#define fd_read fs_read
#define fd_write fs_write
#define FD_FIND FS_FIND
#define FD_FILE FS_FILE
#define FD_SPACE FS_SPACE
#define FD_ATTR_DIR FS_ATTR_DIR

/*
******************************************************************************

Initializes the flash file system.

Parameters:
drvNum               - Drive number

Return:
None

Notes:
Starts EFFS-STD file system, formats if necessary.

******************************************************************************
*/
void fs_main(int drvNum = NOR_DRV_NUM);

#endif /* __FS_MAIN_H */
