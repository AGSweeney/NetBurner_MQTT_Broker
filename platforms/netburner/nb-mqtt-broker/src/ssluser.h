#ifndef SSLUSER_H
#define SSLUSER_H

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
