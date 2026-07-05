// NetBurner application entry point. Brings up network, SSL, the admin web
// server, and the MQTT broker task. Runs indefinitely after startup.

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

    // Auto-generated cert may still be pending; background task waits for NTP then activates TLS.
    if (!ssl_cert_ready) {
        OSSimpleTaskCreatewName(SslCertWaitTask, OSGetNextPrio(OSNextPrio::Below), "SslCertWait");
    }

    BrokerServerInit();
    OSSimpleTaskCreatewName(BrokerServerTask, OSGetNextPrio(OSNextPrio::Above), "MQTTBroker");

    while (1) {
        OSTimeDly(TICKS_PER_SECOND * 60);
    }
}
