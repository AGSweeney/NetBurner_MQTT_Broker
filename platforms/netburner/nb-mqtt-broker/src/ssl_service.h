#ifndef SSL_SERVICE_H
#define SSL_SERVICE_H

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

// TLS certificate lifecycle and web-server startup for the NetBurner module.
// Selects user-installed, HAL auto-generated, or pending self-signed cert, then
// starts HTTP/HTTPS listeners per WebUIConfig.

#include "ssl_config.h"

// Loads cert (if available), starts the web server, and returns whether TLS is ready now.
bool SslServiceInit();
// RTOS task: waits for NTP time, generates a self-signed cert, and reboots on success.
void SslCertWaitTask(void *pd);
// True when a server certificate is loaded (HAL, user-installed, or ACME).
bool SslCertReady();
// Called when ACME enrollment completes — loads cert and starts HTTPS.
void SslServiceOnAcmeCertReady();

#endif
