// NetBurner SSL bootstrap: certificate source selection, web listener startup,
// and deferred self-signed cert generation once wall-clock time is valid.

#include "ssl_service.h"

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

// Tries user-installed cert first, then HAL auto-generated. Updates gHttpsStatus.
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

    if (HalDeviceCertValid()) {
        SSL_ServerReadyCert(HalGetDeviceCert(), HalGetDeviceKey(), HalGetDeviceFormat());
        gHttpsStatus = HTTPS_STATUS_ACTIVE;
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
    return gHttpsStatus == HTTPS_STATUS_ACTIVE || gHttpsStatus == HTTPS_STATUS_USER_CERT;
}

bool SslServiceInit()
{
    CheckNVSettings();
    SslInit();

    const bool ssl_cert_ready = load_ssl_certificate();
    start_web_server(ssl_cert_ready);
    return ssl_cert_ready;
}

void SslCertWaitTask(void *pd)
{
    (void)pd;

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
