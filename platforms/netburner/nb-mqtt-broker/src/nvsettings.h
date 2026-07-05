#ifndef NVSETTINGS_H
#define NVSETTINGS_H

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

// Persistent user-parameter block for first-run setup and SSL certificate metadata.
// Stored in NetBurner user NV via SaveUserParameters; verifyKey detects uninitialized flash.

#include <basictypes.h>

#include "ssl_config.h"

#define NV_SETTINGS_VERIFY_KEY (0x4D515432u)

// Packed layout written to user NV. sslCertLength/KeyLength are exact PEM byte counts.
struct NV_SettingsStruct {
    bool setupComplete;
    uint32_t sslCertSource;
    uint32_t sslCertLength;
    uint32_t sslKeyLength;
    uint32_t verifyKey;
} __attribute__((packed));

extern NV_SettingsStruct NV_Settings;

// Load from flash on boot; resets to defaults when verifyKey is missing or wrong.
void CheckNVSettings();
// Persist current NV_Settings to user parameters.
void SaveNVSettings();
// Restore factory defaults and write them to flash.
void ResetNVSettings();

#endif
