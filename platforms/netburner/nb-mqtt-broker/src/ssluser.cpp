// User-installed TLS certificate and key loaded from EFFS into RAM. Used by
// HTTPS admin UI and MQTTS when NV_Settings.sslCertSource is USER_INSTALLED.

#include "ssluser.h"

#include "nvsettings.h"

#include <crypto/ssl.h>
#include <file/fsf.h>

char gSslCert[(SSL_CERTIFICATE_SIZE_MAX_PEM + 1)];
char gSslKey[(SSL_KEY_SIZE_MAX_PEM + 1)];
bool gSslCertLoaded = false;

// Reads exactly dataSize bytes from an EFFS file (fixed-length PEM storage).
static bool UserGetData(char *dataPtr, char *fileName, int dataSize)
{
    if (dataPtr == nullptr || dataSize <= 0 || fileName == nullptr) {
        return false;
    }

    FS_FILE *fsFilePtr = fs_open(fileName, "r");
    if (fsFilePtr == nullptr) {
        iprintf("[SSL] UserGetData open failed: %s\r\n", fileName);
        return false;
    }

    long bytesRead = fs_read(dataPtr, 1, dataSize, fsFilePtr);
    fs_close(fsFilePtr);
    if (bytesRead != dataSize) {
        iprintf("[SSL] UserGetData read error: expected %d, read %ld\r\n", dataSize, bytesRead);
        return false;
    }

    return true;
}

// Loads user PEM from flash, validates pair, sets gSslCertLoaded on success.
void ReadyCertAndKeys()
{
    bool certificateOK = false;
    bool keyOK = false;

    do {
        if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_LIBRARY_DEFAULT) {
            break;
        }
        if (NV_Settings.sslCertLength == 0 || NV_Settings.sslKeyLength == 0) {
            break;
        }
        if (!UserGetData(gSslCert, (char *)SSL_FILE_NAME_CERT, NV_Settings.sslCertLength)) {
            break;
        }
        if (!UserGetData(gSslKey, (char *)SSL_FILE_NAME_KEY, NV_Settings.sslKeyLength)) {
            break;
        }
        if (!IsSSL_CertNKeyValid(gSslKey, strlen(gSslKey), gSslCert, strlen(gSslCert))) {
            iprintf("[SSL] Certificate and key validation failed\r\n");
            break;
        }
        certificateOK = true;
        keyOK = true;
    } while (0);

    if (certificateOK && keyOK) {
        gSslCertLoaded = true;
    }
}

// NetBurner SSL callbacks — return nullptr unless user cert is loaded (lazy via ReadyCertAndKeys).
const char *GetPrivateKeyPEM()
{
    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) {
        if (!gSslCertLoaded) {
            ReadyCertAndKeys();
        }
        if (gSslCertLoaded) {
            return gSslKey;
        }
    }
    return nullptr;
}

const char *GetCertificatePEM()
{
    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) {
        if (!gSslCertLoaded) {
            ReadyCertAndKeys();
        }
        if (gSslCertLoaded) {
            return gSslCert;
        }
    }
    return nullptr;
}

int GetCertificateLen()
{
    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED && gSslCertLoaded) {
        return (int)strlen(gSslCert);
    }
    return 0;
}

int GetPrivateKeyLen()
{
    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED && gSslCertLoaded) {
        return (int)strlen(gSslKey);
    }
    return 0;
}

// Clears user PEM files and NV metadata; device falls back to auto-generated cert after reboot.
void SslUserSetDefault()
{
    NV_Settings.sslCertSource = SSL_CERT_SOURCE_LIBRARY_DEFAULT;
    NV_Settings.sslCertLength = 0;
    NV_Settings.sslKeyLength = 0;
    SaveNVSettings();

    gSslCertLoaded = false;
    memset(gSslCert, 0, sizeof(gSslCert));
    memset(gSslKey, 0, sizeof(gSslKey));

    int rc = fs_delete(SSL_FILE_NAME_CERT);
    if ((rc != FS_NOERR) && (rc != F_ERR_NOTFOUND)) {
        iprintf("[SSL] Error deleting certificate file: %d\r\n", rc);
    }

    rc = fs_delete(SSL_FILE_NAME_KEY);
    if ((rc != FS_NOERR) && (rc != F_ERR_NOTFOUND)) {
        iprintf("[SSL] Error deleting key file: %d\r\n", rc);
    }

    iprintf("[SSL] Reset to auto-generated certificate\r\n");
}

// Boot hook: load user cert/key from EFFS or revert NV to LIBRARY_DEFAULT on failure.
void SslUserRetrieveCertificateNKey()
{
    bool resetToDefault = false;

    while (NV_Settings.sslCertSource != SSL_CERT_SOURCE_LIBRARY_DEFAULT) {
        if (NV_Settings.sslKeyLength <= 0) {
            resetToDefault = true;
            break;
        }

        memset(gSslKey, 0, sizeof(gSslKey));
        if (!UserGetData(gSslKey, (char *)SSL_FILE_NAME_KEY, NV_Settings.sslKeyLength)) {
            resetToDefault = true;
            break;
        }

        if (NV_Settings.sslCertLength <= 0) {
            resetToDefault = true;
            break;
        }

        memset(gSslCert, 0, sizeof(gSslCert));
        if (!UserGetData(gSslCert, (char *)SSL_FILE_NAME_CERT, NV_Settings.sslCertLength)) {
            resetToDefault = true;
            break;
        }

        if (!IsSSL_CertNKeyValid(gSslKey, strlen(gSslKey), gSslCert, strlen(gSslCert))) {
            resetToDefault = true;
            break;
        }

        gSslCertLoaded = true;
        iprintf("[SSL] User certificate loaded successfully\r\n");
        break;
    }

    if (resetToDefault) {
        NV_Settings.sslCertSource = SSL_CERT_SOURCE_LIBRARY_DEFAULT;
        NV_Settings.sslCertLength = 0;
        NV_Settings.sslKeyLength = 0;
        gSslCertLoaded = false;
        SaveNVSettings();
        iprintf("[SSL] Reset to auto-generated certificate due to load error\r\n");
    }
}
