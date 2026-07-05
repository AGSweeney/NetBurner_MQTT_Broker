#ifndef SSLUSER_H
#define SSLUSER_H

// In-memory PEM buffers for user-installed TLS certificate and private key.
// When sslCertSource is LIBRARY_DEFAULT, Get*PEM returns nullptr and the
// NetBurner SSL stack uses its auto-generated device certificate instead.

#include <crypto/ssl.h>

#include "ssl_config.h"

extern char gSslCert[(SSL_CERTIFICATE_SIZE_MAX_PEM + 1)];
extern char gSslKey[(SSL_KEY_SIZE_MAX_PEM + 1)];
extern bool gSslCertLoaded;

// Switch NV metadata and EFFS files back to auto-generated certificate mode.
void SslUserSetDefault();
// Boot-time load of user PEM files from EFFS; falls back to default on any error.
void SslUserRetrieveCertificateNKey();
// Validates and loads user cert/key into gSslCert/gSslKey when source is user-installed.
void ReadyCertAndKeys();

#endif
