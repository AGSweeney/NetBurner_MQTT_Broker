// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// NetBurner application entry point. Brings up network, SSL, the admin web
// server, and the MQTT broker task. Runs indefinitely after startup.

#include "acme_service.h"
#include "broker_server.hpp"
#include "ssl_service.h"

#include <config_server.h>
#include <init.h>
#include <nbrtos.h>
#include <remoteconsole.h>
#include <system.h>

#if !(defined(SOMRT1061) || defined(MODRT1171))
#include "fs_main.h"
#endif

const char *AppName = "NetBurner MQTT Broker";

// NNDK main thread: init platform services, then start broker and idle.
void UserMain(void *pd)
{
    (void)pd;
    init();

    EnableSystemDiagnostics();
    WaitForActiveNetwork(TICKS_PER_SECOND * 5);

    EnableConfigMirror();
#if !(defined(SOMRT1061) || defined(MODRT1171))
    fs_main();  // Mount EFFS-STD before auth store / TLS cert file access.
#endif
    const bool ssl_cert_ready = SslServiceInit();
    EnableRemoteConsole();

    iprintf("Application: %s\r\nNNDK Revision: %s\r\n", AppName, GetReleaseTag());
    iprintf("Admin UI: http://<device-ip>/ (HTTPS when certificate is ready)\r\n");

    // Deferred TLS: ACME enrollment on SOMRT1061, or self-signed after NTP on other paths.
    if (!ssl_cert_ready) {
        if (AcmeServiceIsEnabled()) {
            OSSimpleTaskCreatewName(AcmeMonitorTask, OSGetNextPrio(OSNextPrio::Below), "AcmeMonitor");
        } else {
            OSSimpleTaskCreatewName(SslCertWaitTask, OSGetNextPrio(OSNextPrio::Below), "SslCertWait");
        }
    }

    BrokerServerInit();
    OSSimpleTaskCreatewName(BrokerServerTask, OSGetNextPrio(OSNextPrio::Above), "MQTTBroker");

    while (1) {
        OSTimeDly(TICKS_PER_SECOND * 60);
    }
}
