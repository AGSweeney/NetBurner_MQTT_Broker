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

// NetBurner SSL bootstrap: certificate source selection, web listener startup,
// and deferred self-signed cert generation once wall-clock time is valid.

#include "ssl_service.h"

#include "acme_config.hpp"
#include "acme_service.h"
#include "nvsettings.h"
#include "ssluser.h"
#include "webui_config.h"

#include <crypto/certgen.h>
#include <crypto/ssl.h>
#include <ShutDownNotifications.h>
#include <hal.h>
#include <http.h>
#include <https.h>
#include <init.h>
#include <nbtime.h>
#include <nbrtos.h>
#include <time.h>

static NtpClientServlet gNtpClient("pool.ntp.org");
static const time_t kMinValidUnixTime = 1767225600UL;  // 2026-01-01 — cert validity needs real time

volatile HttpsServiceStatus gHttpsStatus = HTTPS_STATUS_DISABLED;

static bool load_hal_certificate(HttpsServiceStatus activeStatus)
{
    if (!HalDeviceCertValid()) {
        return false;
    }
    SSL_ServerReadyCert(HalGetDeviceCert(), HalGetDeviceKey(), HalGetDeviceFormat());
    gHttpsStatus = activeStatus;
    return true;
}

// Tries user-installed cert first, then ACME, then HAL auto-generated. Updates gHttpsStatus.
static bool load_ssl_certificate()
{
    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) {
        SslUserRetrieveCertificateNKey();
        if (gSslCertLoaded) {
            SSL_ServerReadyCert((const unsigned char *)GetCertificatePEM(),
                                (const unsigned char *)GetPrivateKeyPEM());
            gHttpsStatus = HTTPS_STATUS_USER_CERT;
            iprintf("[SSL] Using user-installed certificate\r\n");
            return true;
        }
        iprintf("[SSL] User certificate load failed; will auto-generate after time sync\r\n");
        return false;
    }

    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_ACME) {
        if (AcmeServiceCertReady()) {
            SSL_ServerReadyCert(HalGetDeviceCert(), HalGetDeviceKey(), HalGetDeviceFormat());
            gHttpsStatus = HTTPS_STATUS_ACME_ACTIVE;
            iprintf("[SSL] Using Let's Encrypt (ACME) certificate\r\n");
            return true;
        }
        gHttpsStatus = HTTPS_STATUS_ACME_PENDING;
        iprintf("[SSL] ACME enrollment pending (state: %s)\r\n", AcmeServiceGetStateString());
        return false;
    }

    if (load_hal_certificate(HTTPS_STATUS_ACTIVE)) {
        iprintf("[SSL] Using auto-generated HAL certificate\r\n");
        return true;
    }

    iprintf("[SSL] No certificate yet; will auto-generate after time sync\r\n");
    return false;
}

// Starts HTTP and/or HTTPS per gWebUIConfig. When ssl_cert_ready is false but
// HTTPS is enabled, serves HTTP only and leaves gHttpsStatus as waiting/failed.
static void start_web_server(bool ssl_cert_ready)
{
    const bool acme_pending = (NV_Settings.sslCertSource == SSL_CERT_SOURCE_ACME && !ssl_cert_ready);

    if (ssl_cert_ready) {
        if (gWebUIConfig.m_httpsEnabled && gWebUIConfig.m_httpEnabled) {
            StartHttps(443, 80);
            iprintf("[SSL] Web server: HTTPS 443 + HTTP 80\r\n");
        } else if (gWebUIConfig.m_httpsEnabled) {
            StartHttps(443, 0);
            iprintf("[SSL] Web server: HTTPS 443 only\r\n");
        } else if (gWebUIConfig.m_httpEnabled) {
            StartHttp(80);
            gHttpsStatus = HTTPS_STATUS_DISABLED;
            iprintf("[SSL] Web server: HTTP 80 only\r\n");
        } else {
            StartHttps(443, 80);
            iprintf("[SSL] Web server: HTTPS 443 + HTTP 80 (default)\r\n");
        }
        return;
    }

    if (acme_pending) {
        StartHttp(80);
        gHttpsStatus = HTTPS_STATUS_ACME_PENDING;
        iprintf("[SSL] Web server: HTTP 80 (ACME enrollment in progress)\r\n");
        return;
    }

    StartHttp(80);
    if (gWebUIConfig.m_httpsEnabled) {
        gHttpsStatus = HTTPS_STATUS_WAITING_FOR_TIME;
        iprintf("[SSL] Web server: HTTP 80 (HTTPS pending valid time)\r\n");
    } else {
        gHttpsStatus = HTTPS_STATUS_DISABLED;
        iprintf("[SSL] Web server: HTTP 80 only\r\n");
    }
}

bool SslCertReady()
{
    return gHttpsStatus == HTTPS_STATUS_ACTIVE || gHttpsStatus == HTTPS_STATUS_USER_CERT ||
           gHttpsStatus == HTTPS_STATUS_ACME_ACTIVE;
}

bool SslServiceInit()
{
    CheckNVSettings();
    AcmeConfigSanitizeOnBoot();
    SslInit();
    AcmeServiceInit();

    const bool ssl_cert_ready = load_ssl_certificate();
    start_web_server(ssl_cert_ready);
    return ssl_cert_ready;
}

void SslServiceOnAcmeCertReady()
{
    if (!AcmeServiceCertReady()) {
        return;
    }

    SSL_ServerReadyCert(HalGetDeviceCert(), HalGetDeviceKey(), HalGetDeviceFormat());
    gHttpsStatus = HTTPS_STATUS_ACME_ACTIVE;
    iprintf("[SSL] Let's Encrypt certificate ready; starting HTTPS\r\n");

    if (gWebUIConfig.m_httpsEnabled) {
        if (gWebUIConfig.m_httpEnabled) {
            StartHttps(443, 80);
        } else {
            StartHttps(443, 0);
        }
    }
}

void SslCertWaitTask(void *pd)
{
    (void)pd;

    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_ACME) {
        return;
    }

    if (HalDeviceCertValid() || gSslCertLoaded ||
        NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) {
        return;
    }

    if (!gWebUIConfig.m_httpsEnabled) {
        return;
    }

    iprintf("[SSL] Waiting for valid time to generate certificate...\r\n");
    for (int i = 0; i < 180 && time(nullptr) < kMinValidUnixTime; ++i) {
        OSTimeDly(TICKS_PER_SECOND);
    }

    if (time(nullptr) < kMinValidUnixTime) {
        gHttpsStatus = HTTPS_STATUS_CERT_FAILED;
        iprintf("[SSL] ERROR: Valid time not available for certificate generation\r\n");
        return;
    }

    gHttpsStatus = HTTPS_STATUS_GENERATING_CERT;
    iprintf("[SSL] Generating self-signed certificate...\r\n");

    CertGenData *pGenData = GetDataForCertGen();
    if (pGenData != nullptr) {
        pGenData->m_yrsValid = 20;
    }

    const CertGenReturnCode result = SSL_CreateNewSelfSignedCert(*pGenData);
    if (result == CERT_GEN_RETURN_SUCCESS) {
        iprintf("[SSL] Certificate generated; rebooting\r\n");
        OSTimeDly(TICKS_PER_SECOND * 2);
        if (NBApproveShutdown(SHUTDOWN_CONFIGURE_REBOOT)) {
            ForceReboot();
        }
    } else {
        gHttpsStatus = HTTPS_STATUS_CERT_FAILED;
        iprintf("[SSL] ERROR: Certificate generation failed (%d)\r\n", (int)result);
    }
}
