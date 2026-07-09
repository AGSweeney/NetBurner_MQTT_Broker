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

// Let's Encrypt ACME servlet integration (SOMRT1061). Pattern follows NNDK
// examples/SSL/acmeservlet. Stub no-ops on platforms without ACME support.

#include "acme_service.h"

#include "acme_config.hpp"
#include "nvsettings.h"
#include "ssl_config.h"
#include "ssl_service.h"

#include <hal.h>
#include <iosys.h>
#include <nbrtos.h>
#include <string.h>

#if kAcmePlatformSupported
#include <acmeRFC8555Servlet.h>
#include <crypto/certgen.h>
#include <new>
#endif

#if kAcmePlatformSupported

extern CertData TheCertData;

static uint8_t sAcmeClientStorage[sizeof(LetsEncryptAcmeServletObject)];
static LetsEncryptAcmeServletObject *sAcmeClient = nullptr;
static CertGenData sAcmeCertGenData;
static char sAcmeCommonName[128];
static char sAcmeAltNames[256];
static char sAcmeEmail[128];

static bool acme_hal_has_issued_cert()
{
    if (!HalDeviceCertValid()) {
        return false;
    }
    const uint8_t *pHalCert = HalGetDeviceCert();
    if (pHalCert == nullptr) {
        return false;
    }
    return strstr(reinterpret_cast<const char *>(pHalCert), "ACME ISSUE") != nullptr;
}

static void acme_fill_cert_gen_data()
{
    memset(&sAcmeCertGenData, 0, sizeof(sAcmeCertGenData));
    sAcmeCertGenData.m_country = "US";
    sAcmeCertGenData.m_state = "CA";
    sAcmeCertGenData.m_locality = "San Diego";
    sAcmeCertGenData.m_org = "NetBurner MQTT Broker";
    sAcmeCertGenData.m_unit = "Device";

    const char *cn = gAcmeConfig.m_commonName.c_str();
    const char *alt = gAcmeConfig.m_altNames.c_str();
    const char *email = gAcmeConfig.m_email.c_str();

    if (cn == nullptr || cn[0] == '\0') {
        cn = "localhost";
    }
    if (alt == nullptr || alt[0] == '\0') {
        alt = cn;
    }
    if (email == nullptr || email[0] == '\0') {
        email = "admin@example.com";
    }

    strncpy(sAcmeCommonName, cn, sizeof(sAcmeCommonName) - 1);
    strncpy(sAcmeAltNames, alt, sizeof(sAcmeAltNames) - 1);
    strncpy(sAcmeEmail, email, sizeof(sAcmeEmail) - 1);
    sAcmeCommonName[sizeof(sAcmeCommonName) - 1] = '\0';
    sAcmeAltNames[sizeof(sAcmeAltNames) - 1] = '\0';
    sAcmeEmail[sizeof(sAcmeEmail) - 1] = '\0';

    sAcmeCertGenData.m_commonName = sAcmeCommonName;
    sAcmeCertGenData.m_altNamesString = sAcmeAltNames;
    sAcmeCertGenData.m_email = sAcmeEmail;
    sAcmeCertGenData.m_yrsValid = 1;
    sAcmeCertGenData.m_certExpTime = 0;
}

CertGenData *GetDataForCertGen()
{
    if (AcmeConfigActive()) {
        acme_fill_cert_gen_data();
        return &sAcmeCertGenData;
    }
    return TheCertData.GetDataForCertGen();
}

#endif  // kAcmePlatformSupported

bool AcmeServiceIsEnabled()
{
    return AcmeConfigActive();
}

void AcmeServiceInit()
{
#if kAcmePlatformSupported
    if (!AcmeServiceIsEnabled()) {
        return;
    }

    acme_fill_cert_gen_data();

    const bool useStaging = gAcmeConfig.m_useStaging;
    sAcmeClient = new (sAcmeClientStorage) LetsEncryptAcmeServletObject(useStaging);
    sAcmeClient->SetDiag(true);
    iprintf("[ACME] Let's Encrypt client started (%s)\r\n",
            useStaging ? "staging" : "production");
    iprintf("[ACME] Common name: %s\r\n", sAcmeCommonName);
#else
    (void)0;
#endif
}

bool AcmeServiceCertReady()
{
#if kAcmePlatformSupported
    return AcmeServiceIsEnabled() && acme_hal_has_issued_cert();
#else
    return false;
#endif
}

bool AcmeServiceHasFailed()
{
#if kAcmePlatformSupported
    if (!AcmeServiceIsEnabled() || sAcmeClient == nullptr) {
        return false;
    }
    const char *state = sAcmeClient->GetStateCC();
    return state != nullptr && strcmp(state, "ErrorWaitForRetry") == 0;
#else
    return false;
#endif
}

const char *AcmeServiceGetStateString()
{
#if kAcmePlatformSupported
    if (sAcmeClient != nullptr) {
        return sAcmeClient->GetStateCC();
    }
#endif
    return "unavailable";
}

void AcmeServiceRestart()
{
#if kAcmePlatformSupported
    if (sAcmeClient != nullptr) {
        iprintf("[ACME] Restarting enrollment\r\n");
        sAcmeClient->Delete_Everything_Restart();
    }
#endif
}

void AcmeMonitorTask(void *pd)
{
    (void)pd;

#if kAcmePlatformSupported
    if (!AcmeServiceIsEnabled()) {
        return;
    }

    iprintf("[ACME] Monitoring enrollment (state: %s)\r\n", AcmeServiceGetStateString());

    for (int i = 0; i < 600; ++i) {
        if (AcmeServiceCertReady()) {
            SslServiceOnAcmeCertReady();
            return;
        }
        if (AcmeServiceHasFailed()) {
            gHttpsStatus = HTTPS_STATUS_ACME_FAILED;
            iprintf("[ACME] Enrollment failed (state: %s)\r\n", AcmeServiceGetStateString());
            return;
        }
        if ((i % 10) == 0) {
            iprintf("[ACME] State: %s\r\n", AcmeServiceGetStateString());
        }
        OSTimeDly(3 * TICKS_PER_SECOND);
    }

    gHttpsStatus = HTTPS_STATUS_ACME_FAILED;
    iprintf("[ACME] Enrollment timed out\r\n");
#endif
}
