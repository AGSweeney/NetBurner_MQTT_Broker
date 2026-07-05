#ifndef NVSETTINGS_H
#define NVSETTINGS_H

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
