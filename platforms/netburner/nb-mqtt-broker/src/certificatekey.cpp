// Admin UI SSL certificate upload and REST API handlers. Accepts PEM cert/key via
// multipart form, persists to EFFS, and exposes JSON endpoints for status, reset,
// protocol toggles, and deferred device reboot after cert changes.

#include "ssl_config.h"
#include "nvsettings.h"
#include "ssluser.h"
#include "webui_config.h"

#include <config_server.h>
#include <crypto/certgen.h>
#include <crypto/ssl.h>
#include <fdprintf.h>
#include <file/fsf.h>
#include <ShutDownNotifications.h>
#include <hal.h>
#include <http.h>
#include <httppost.h>
#include <init.h>
#include <iointernal.h>
#include <nbrtos.h>

static int sCertificateFileStatus = SSL_STATUS_VALID;
static int sKeyFileStatus = SSL_STATUS_VALID;

static bool sRebootPending = false;

bool UserSaveData(char *dataPtr, int dataSize, const char *fileName);

// Reboot after a short delay so the HTTP response can complete before shutdown.
static void DelayRebootTask(void *pd)
{
    (void)pd;
    OSTimeDly(TICKS_PER_SECOND * 2);
    if (NBApproveShutdown(SHUTDOWN_CONFIGURE_REBOOT)) {
        ForceReboot();
    } else {
        sRebootPending = false;
    }
}

// Single-flight guard — only one reboot task may be queued at a time.
static void schedule_reboot()
{
    if (sRebootPending) {
        return;
    }
    sRebootPending = true;
    OSSimpleTaskCreatewName(DelayRebootTask, 51, "RebootTask");
}

// Reads uploaded PEM into buffer; must fit with room for trailing NUL.
static bool captureFile(int fileDescriptor, unsigned char *bufferPtr, ssize_t bufferSize,
                        ssize_t *capturedSize)
{
    if (bufferPtr == nullptr || bufferSize <= 0 || capturedSize == nullptr) {
        return false;
    }

    *capturedSize = 0;
    ssize_t bytesRead = read(fileDescriptor, (char *)bufferPtr, bufferSize);
    if (bytesRead > 0 && bytesRead < bufferSize) {
        *capturedSize = bytesRead;
        return true;
    }
    return false;
}

// Writes PEM blob to EFFS (delete-then-create). Used for cert.crt and cert.key.
bool UserSaveData(char *dataPtr, int dataSize, const char *fileName)
{
    if (dataPtr == nullptr || dataSize <= 0 || fileName == nullptr) {
        return false;
    }

    int rc = fs_delete(fileName);
    if ((rc != FS_NOERR) && (rc != F_ERR_NOTFOUND)) {
        iprintf("[SSL] UserSaveData delete error: %d (%s)\r\n", rc, fileName);
    }

    FS_FILE *fsFilePtr = fs_open(fileName, "w");
    if (fsFilePtr == nullptr) {
        iprintf("[SSL] UserSaveData open failed: %s\r\n", fileName);
        return false;
    }

    long written = fs_write(dataPtr, 1, dataSize, fsFilePtr);
    fs_close(fsFilePtr);
    if (written != dataSize) {
        iprintf("[SSL] UserSaveData write error: expected %d, wrote %ld\r\n", dataSize, written);
        return false;
    }

    return true;
}

// Multipart POST handler for httpsform — accumulates key/cert files across eFile
// events, validates pair on eEndOfPost, then activates SSL_ServerReadyCert.
int HttpsPost(int sock, PostEvents event, const char *pName, const char *pValue)
{
    const char *responsePage = "index.html?from=ssl";
    static char *keyDataPtr = nullptr;
    static ssize_t keyFileSize = 0;
    static char *certificateDataPtr = nullptr;
    static ssize_t certificateFileSize = 0;

    if (event == eVariable) {
        if (strcmp(pName, "ResetToDefaults") == 0) {
            SslUserSetDefault();
            gSslCertLoaded = false;
            HalEraseDeviceCertAndKey();
            iprintf("[SSL] Reset to auto-generated certificate (reboot required)\r\n");
        }
    } else if (event == eFile) {
        sCertificateFileStatus = SSL_STATUS_VALID;
        sKeyFileStatus = SSL_STATUS_VALID;
        FilePostStruct *pFps = (FilePostStruct *)pValue;

        if (strcmp(pName, "keyFile") == 0) {
            keyFileSize = 0;
            if (dataavail(pFps->fd)) {
                keyDataPtr = (char *)calloc(1, SSL_KEY_SIZE_MAX_PEM + 1);
                if (keyDataPtr != nullptr &&
                    !captureFile(pFps->fd, (unsigned char *)keyDataPtr, SSL_KEY_SIZE_MAX_PEM,
                                 &keyFileSize)) {
                    sKeyFileStatus = SSL_STATUS_INVALID;
                }
            } else {
                sKeyFileStatus = SSL_STATUS_INVALID;
            }
        } else if (strcmp(pName, "certificateFile") == 0) {
            certificateFileSize = 0;
            if (dataavail(pFps->fd)) {
                certificateDataPtr = (char *)calloc(1, SSL_CERTIFICATE_SIZE_MAX_PEM + 1);
                if (certificateDataPtr != nullptr &&
                    !captureFile(pFps->fd, (unsigned char *)certificateDataPtr,
                                 SSL_CERTIFICATE_SIZE_MAX_PEM, &certificateFileSize)) {
                    sCertificateFileStatus = SSL_STATUS_INVALID;
                }
            } else {
                sCertificateFileStatus = SSL_STATUS_NOT_FOUND;
            }
        }

        FreeExtraFd(pFps->fd);
    } else if (event == eEndOfPost) {
        // Both files must arrive in one POST; validate pair before writing EFFS/NV.
        if (keyDataPtr != nullptr || certificateDataPtr != nullptr) {
            const bool validSize = (keyFileSize > 0) && (certificateFileSize > 0);
            if (validSize &&
                IsSSL_CertNKeyValid(keyDataPtr, keyFileSize, certificateDataPtr, certificateFileSize)) {
                if (!UserSaveData(certificateDataPtr, certificateFileSize, SSL_FILE_NAME_CERT)) {
                    sCertificateFileStatus = SSL_STATUS_INVALID;
                }
                if (!UserSaveData(keyDataPtr, keyFileSize, SSL_FILE_NAME_KEY)) {
                    sKeyFileStatus = SSL_STATUS_INVALID;
                }

                if (sCertificateFileStatus != SSL_STATUS_INVALID && sKeyFileStatus != SSL_STATUS_INVALID) {
                    memset(gSslCert, 0, sizeof(gSslCert));
                    memcpy(gSslCert, certificateDataPtr, certificateFileSize);
                    NV_Settings.sslCertLength = (uint32_t)certificateFileSize;

                    memset(gSslKey, 0, sizeof(gSslKey));
                    memcpy(gSslKey, keyDataPtr, keyFileSize);
                    NV_Settings.sslKeyLength = (uint32_t)keyFileSize;
                    NV_Settings.sslCertSource = SSL_CERT_SOURCE_USER_INSTALLED;
                    gSslCertLoaded = true;

                    SSL_ServerReadyCert((const unsigned char *)GetCertificatePEM(),
                                        (const unsigned char *)GetPrivateKeyPEM());
                    SaveNVSettings();
                    gHttpsStatus = HTTPS_STATUS_USER_CERT;
                    iprintf("[SSL] User certificate installed\r\n");
                    responsePage = "index.html?from=ssl_status";
                }
            } else {
                sCertificateFileStatus = SSL_STATUS_CERT_KEY_MISMATCH;
            }

            if (certificateDataPtr != nullptr) {
                free(certificateDataPtr);
                certificateDataPtr = nullptr;
                certificateFileSize = 0;
            }
            if (keyDataPtr != nullptr) {
                free(keyDataPtr);
                keyDataPtr = nullptr;
                keyFileSize = 0;
            }
        }

        RedirectResponse(sock, responsePage);
    }

    return 0;
}

HtmlPostVariableListCallback gHttpsPost("httpsform", HttpsPost, 99);

static const char *GetHttpsStatusString(HttpsServiceStatus status)
{
    switch (status) {
        case HTTPS_STATUS_DISABLED:
            return "disabled";
        case HTTPS_STATUS_WAITING_FOR_TIME:
            return "waiting_for_time";
        case HTTPS_STATUS_GENERATING_CERT:
            return "generating";
        case HTTPS_STATUS_CERT_FAILED:
            return "failed";
        case HTTPS_STATUS_ACTIVE:
            return "active";
        case HTTPS_STATUS_USER_CERT:
            return "active_user_cert";
        default:
            return "unknown";
    }
}

// GET api/ssl.json — certificate source, HTTPS service state, and last upload errors.
int ApiSslGetHandler(int sock, HTTP_Request &req)
{
    (void)req;

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "Cache-Control: no-cache\r\n\r\n");

    fdprintf(sock, "{\r\n");
    fdprintf(sock, "  \"certificate\": {\r\n");
    fdprintf(sock, "    \"source\": \"%s\",\r\n",
             (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) ? "user" : "default");
    fdprintf(sock, "    \"length\": %lu,\r\n", (unsigned long)NV_Settings.sslCertLength);
    fdprintf(sock, "    \"installed\": %s\r\n",
             (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) ? "true" : "false");
    fdprintf(sock, "  },\r\n");
    fdprintf(sock, "  \"key\": {\r\n");
    fdprintf(sock, "    \"length\": %lu,\r\n", (unsigned long)NV_Settings.sslKeyLength);
    fdprintf(sock, "    \"installed\": %s\r\n", (NV_Settings.sslKeyLength > 0) ? "true" : "false");
    fdprintf(sock, "  },\r\n");
    fdprintf(sock, "  \"protocols\": {\r\n");
    fdprintf(sock, "    \"httpsEnabled\": %s,\r\n", gWebUIConfig.m_httpsEnabled ? "true" : "false");
    fdprintf(sock, "    \"httpEnabled\": %s,\r\n", gWebUIConfig.m_httpEnabled ? "true" : "false");
    fdprintf(sock, "    \"requireAuthForAll\": %s\r\n",
             gWebUIConfig.m_requireAuthForAll ? "true" : "false");
    fdprintf(sock, "  },\r\n");
    fdprintf(sock, "  \"httpsService\": {\r\n");
    fdprintf(sock, "    \"status\": \"%s\",\r\n", GetHttpsStatusString(gHttpsStatus));
    fdprintf(sock, "    \"statusCode\": %d\r\n", (int)gHttpsStatus);
    fdprintf(sock, "  },\r\n");
    fdprintf(sock, "  \"lastError\": {\r\n");
    fdprintf(sock, "    \"certificate\": %d,\r\n", sCertificateFileStatus);
    fdprintf(sock, "    \"key\": %d\r\n", sKeyFileStatus);
    fdprintf(sock, "  }\r\n");
    fdprintf(sock, "}\r\n");

    return 1;
}

// POST api/ssl/reset — revert to auto-generated device certificate (reboot required).
int ApiSslResetHandler(int sock, HTTP_Request &req)
{
    (void)req;

    SslUserSetDefault();
    gSslCertLoaded = false;
    HalEraseDeviceCertAndKey();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "\r\n");
    fdprintf(sock,
             "{\"success\": true, \"message\": \"SSL certificate reset to auto-generated. Reboot required.\"}\r\n");
    return 1;
}

// POST api/ssl/regenerate — erase auto-cert only; blocked while user cert is installed.
int ApiSslRegenerateHandler(int sock, HTTP_Request &req)
{
    (void)req;

    if (NV_Settings.sslCertSource == SSL_CERT_SOURCE_USER_INSTALLED) {
        writestring(sock, "HTTP/1.0 400 Bad Request\r\n");
        writestring(sock, "Content-Type: application/json\r\n");
        writestring(sock, "\r\n");
        fdprintf(sock,
                 "{\"success\": false, \"message\": \"Cannot regenerate user-installed certificate. Use reset first.\"}\r\n");
        return 1;
    }

    HalEraseDeviceCertAndKey();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "\r\n");
    fdprintf(sock, "{\"success\": true, \"message\": \"Certificate erased. Reboot to generate new certificate.\"}\r\n");
    return 1;
}

// POST api/ssl/protocols — toggle HTTP/HTTPS listeners and require-auth-for-all flag.
int ApiSslProtocolsHandler(int sock, HTTP_Request &req)
{
    char body[256] = {};
    if (req.content_length > 0 && req.content_length < (int)sizeof(body) - 1) {
        int nleft = req.content_length;
        int offset = 0;
        while (nleft > 0) {
            int rv = read(sock, body + offset, nleft);
            if (rv <= 0) {
                break;
            }
            offset += rv;
            nleft -= rv;
        }
        body[offset] = '\0';
    }

    // Minimal JSON body parse (no full parser) — sufficient for three boolean flags.
    const bool httpsEnabled = (strstr(body, "\"httpsEnabled\":true") != nullptr ||
                             strstr(body, "\"httpsEnabled\": true") != nullptr);
    const bool httpEnabled = (strstr(body, "\"httpEnabled\":true") != nullptr ||
                              strstr(body, "\"httpEnabled\": true") != nullptr);
    const bool requireAuthForAll = (strstr(body, "\"requireAuthForAll\":true") != nullptr ||
                                    strstr(body, "\"requireAuthForAll\": true") != nullptr);

    gWebUIConfig.m_httpsEnabled = httpsEnabled;
    gWebUIConfig.m_httpEnabled = httpEnabled;
    gWebUIConfig.m_requireAuthForAll = requireAuthForAll;
    SaveConfigToStorage();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "\r\n");
    fdprintf(sock, "{\"success\": true}\r\n");
    return 1;
}

// POST api/reboot — schedule deferred reboot (used after cert or config changes).
int ApiRebootHandler(int sock, HTTP_Request &req)
{
    (void)req;

    if (sRebootPending) {
        writestring(sock, "HTTP/1.0 200 OK\r\n");
        writestring(sock, "Content-Type: application/json\r\n");
        writestring(sock, "\r\n");
        fdprintf(sock, "{\"success\": true, \"message\": \"Reboot already pending\"}\r\n");
        return 1;
    }

    schedule_reboot();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "\r\n");
    fdprintf(sock, "{\"success\": true}\r\n");
    return 1;
}

CallBackFunctionPageHandler gApiSslGet("api/ssl.json", ApiSslGetHandler, tGet, 50);
CallBackFunctionPageHandler gApiSslReset("api/ssl/reset", ApiSslResetHandler, tPost, 99);
CallBackFunctionPageHandler gApiSslRegenerate("api/ssl/regenerate", ApiSslRegenerateHandler, tPost, 99);
CallBackFunctionPageHandler gApiSslProtocols("api/ssl/protocols", ApiSslProtocolsHandler, tPost, 99);
CallBackFunctionPageHandler gApiReboot("api/reboot", ApiRebootHandler, tPost, 99);
