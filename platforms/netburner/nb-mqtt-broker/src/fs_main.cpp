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

#include <stdio.h>
#include <stdlib.h>
#include <constants.h>

#include "effs_std.h"
#include "file/flashdrv.h"
#include "fs_main.h"

__attribute__((weak)) int fs_phy_OnChipFlash(FS_FLASH *flash) {
    return FS_NO_ERROR;
}

/**
 *  fs_main
 *
 *  Initializes the flash file system.
 */
void fs_main(int drvNum)
{
    printf("File system version:  %s\r\n", fs_getversion());
    fs_init();   // Initialize the file system

    {
        long mem_size = 0;
        char *mem_ptr = nullptr;
        int rc = 0;

        printf("Initializing file system...");
        // Returns the number of bytes of ram memory required for the file system.
        mem_size = fs_getmem_flashdrive(fs_phy_OnChipFlash);
        printf(" %s (%ld bytes)\r\n", (mem_size > 0) ? "Success" : "Failed", mem_size);
        if (mem_size)
        {
            printf("Allocating memory...");
            mem_ptr = (char *)malloc(mem_size);
            printf(" %s\r\n", (mem_ptr != NULL) ? "Success" : "Failed");
            if (mem_ptr)
            {
                // Mount the file system.
                printf("Mounting drive...");
                rc = fd_mountstd(drvNum, mem_ptr, mem_size, fs_mount_flashdrive, fs_phy_OnChipFlash);
                printf(" %s\r\n", (rc == FS_NO_ERROR) ? "Success" : "Failed");

                if (rc)   // If mount was not successful, then format the drive
                {
                    printf("Mount failed with error code: %d, formatting drive...", rc);
                    rc = fd_format(drvNum, 0);
                    printf(" %s\r\n", (rc == FS_NO_ERROR) ? "Success" : "Failed");
                }
            }
        }
    }

    fd_chdrive(drvNum);
}
