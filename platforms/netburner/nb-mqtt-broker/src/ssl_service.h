#ifndef SSL_SERVICE_H
#define SSL_SERVICE_H

// TLS certificate lifecycle and web-server startup for the NetBurner module.
// Selects user-installed, HAL auto-generated, or pending self-signed cert, then
// starts HTTP/HTTPS listeners per WebUIConfig.

#include "ssl_config.h"

// Loads cert (if available), starts the web server, and returns whether TLS is ready now.
bool SslServiceInit();
// RTOS task: waits for NTP time, generates a self-signed cert, and reboots on success.
void SslCertWaitTask(void *pd);
// True when a server certificate is loaded (HAL or user-installed).
bool SslCertReady();

#endif
