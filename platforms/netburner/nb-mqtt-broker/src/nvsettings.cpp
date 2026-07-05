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

// NetBurner user-parameter persistence for setupComplete and SSL cert metadata.
// verifyKey guards against uninitialized flash; mismatch triggers ResetNVSettings.

#include "nvsettings.h"

#include <iosys.h>
#include <system.h>

NV_SettingsStruct NV_Settings;

// Factory defaults for a fresh or corrupt NV block.
static void SetNVDefaults()
{
    NV_Settings.setupComplete = false;
    NV_Settings.sslCertSource = SSL_CERT_SOURCE_LIBRARY_DEFAULT;
    NV_Settings.sslCertLength = 0;
    NV_Settings.sslKeyLength = 0;
    NV_Settings.verifyKey = NV_SETTINGS_VERIFY_KEY;
}

// Write factory defaults to flash.
void ResetNVSettings()
{
    SetNVDefaults();
    SaveNVSettings();
}

// Called at startup from init path. Copies user NV into NV_Settings when valid.
void CheckNVSettings()
{
    NV_SettingsStruct *pData = (NV_SettingsStruct *)GetUserParameters();

    if (pData->verifyKey != NV_SETTINGS_VERIFY_KEY) {
        ResetNVSettings();
        iprintf("[SSL] NV settings initialized to defaults\r\n");
    } else {
        NV_Settings = *pData;
        iprintf("[SSL] NV settings loaded (sslCertSource=%lu)\r\n", NV_Settings.sslCertSource);
    }
}

// Refresh verifyKey and persist the in-memory NV_Settings block.
void SaveNVSettings()
{
    NV_Settings.verifyKey = NV_SETTINGS_VERIFY_KEY;
    SaveUserParameters(&NV_Settings, sizeof(NV_Settings));
}
