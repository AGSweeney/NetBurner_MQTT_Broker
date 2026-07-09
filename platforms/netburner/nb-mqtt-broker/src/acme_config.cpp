// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "acme_config.hpp"

#include "nvsettings.h"
#include "ssl_config.h"

#include <config_server.h>
#include <iosys.h>

AcmeConfig gAcmeConfig(appdata, "Acme", "Let's Encrypt ACME certificate settings.");

bool AcmeConfigActive()
{
    return AcmePlatformSupported() && gAcmeConfig.m_enabled &&
           NV_Settings.sslCertSource == SSL_CERT_SOURCE_ACME;
}

void AcmeConfigDisable()
{
    gAcmeConfig.m_enabled = false;
    SaveConfigToStorage();
}

void AcmeConfigSanitizeOnBoot()
{
    if (NV_Settings.sslCertSource != SSL_CERT_SOURCE_ACME) {
        return;
    }

    if (!AcmePlatformSupported()) {
        iprintf("[ACME] sslCertSource=ACME on unsupported platform; reverting to auto-generated\r\n");
        NV_Settings.sslCertSource = SSL_CERT_SOURCE_LIBRARY_DEFAULT;
        SaveNVSettings();
        gAcmeConfig.m_enabled = false;
        SaveConfigToStorage();
        return;
    }

    if (!gAcmeConfig.m_enabled) {
        iprintf("[ACME] sslCertSource=ACME but AcmeEnabled=false; reverting to auto-generated\r\n");
        NV_Settings.sslCertSource = SSL_CERT_SOURCE_LIBRARY_DEFAULT;
        SaveNVSettings();
    }
}
