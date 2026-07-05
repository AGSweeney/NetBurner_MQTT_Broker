#ifndef SSL_CONFIG_H
#define SSL_CONFIG_H

// Shared SSL/TLS constants: PEM size limits, EFFS filenames, upload status codes,
// and HTTPS service lifecycle states used by the admin UI and certificate handlers.

#include <basictypes.h>

#define SSL_CERT_SOURCE_LIBRARY_DEFAULT ((uint8_t)0x00)
#define SSL_CERT_SOURCE_USER_INSTALLED  ((uint8_t)0x01)

// Max raw DER and PEM buffer sizes for cert/key uploads.
#define SSL_CERTIFICATE_SIZE_MAX      ((2 * 1024) - 1)
#define SSL_CERTIFICATE_SIZE_MAX_PEM  ((3 * 1024) - 1)
#define SSL_KEY_SIZE_MAX_PEM          ((4 * 1024) - 1)

#define SSL_FILE_NAME_CERT "cert.crt"
#define SSL_FILE_NAME_KEY  "cert.key"

// Last certificate/key upload validation result (surfaced in api/ssl.json).
#define SSL_STATUS_VALID              (0)
#define SSL_STATUS_NOT_FOUND          (1)
#define SSL_STATUS_INVALID            (2)
#define SSL_STATUS_CERT_INVALID       (3)
#define SSL_STATUS_KEY_INVALID        (4)
#define SSL_STATUS_CERT_KEY_MISMATCH  (5)

// HTTPS listener state machine progress (auto-cert generation or user PEM active).
enum HttpsServiceStatus : uint8_t {
    HTTPS_STATUS_DISABLED = 0,
    HTTPS_STATUS_WAITING_FOR_TIME = 1,
    HTTPS_STATUS_GENERATING_CERT = 2,
    HTTPS_STATUS_CERT_FAILED = 3,
    HTTPS_STATUS_ACTIVE = 4,
    HTTPS_STATUS_USER_CERT = 5,
};

extern volatile HttpsServiceStatus gHttpsStatus;

#endif
