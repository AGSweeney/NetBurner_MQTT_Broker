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

#ifndef _EFFS_TIME_H
#define _EFFS_TIME_H

// Set the system time using a Network Time Server
uint32_t SetTimeNTP();

// Set the system time manually
void SetTimeManual(int month, int day, int weekday, int year, int hour, int min, int sec);

// Set the system time using a Real-Time clock
void SetTimeRTC();

void DisplaySystemTime();

// This function is deprecated. Use tzsetchar (or SetNamedTimeZone) for proper timezone management
void SetTimeZone(int hour_offset, int isdst) __attribute__((deprecated));
void WasSetTimeZone(int hour_offset, int isdst);

// Set the system time zone by its formal Name from the TZRecords[] table
// (e.g. "Pacific Standard Time"). Returns TRUE if found and applied.
BOOL SetNamedTimeZone(const char *zoneName);

// Print the full TZRecords[] time zone table to the serial console.
void ListTimeZones();

#endif /* _EFFS_TIME_H */
