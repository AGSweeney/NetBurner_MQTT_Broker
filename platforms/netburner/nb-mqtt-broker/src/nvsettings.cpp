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
