// JSON status endpoint for the admin dashboard. Exposes broker metrics,
// listener state, compile-time limits, and the detected NetBurner platform.

#include "broker_config.hpp"
#include "broker_server.hpp"

#include <mqtt_broker/broker_limits.hpp>

#include <fdprintf.h>
#include <http.h>
#include <system.h>

#if defined(PLATFORM_SOMRT1061) || defined(SOMRT1061)
static const char *kPlatformName = "SOMRT1061";
#elif defined(PLATFORM_MODM7AE70) || defined(MODM7AE70)
static const char *kPlatformName = "MODM7AE70";
#elif defined(PLATFORM_NANO54415) || defined(NANO54415)
static const char *kPlatformName = "NANO54415";
#else
static const char *kPlatformName = "NetBurner";
#endif

// Emits BrokerLimits constants as JSON object fields (trailing comma omitted on last).
static void write_limits_json(int sock)
{
    fdprintf(sock, "    \"MaxTcpClients\": %d,\r\n", BrokerLimits::MaxTcpClients);
    fdprintf(sock, "    \"MaxTlsClients\": %d,\r\n", BrokerLimits::MaxTlsClients);
    fdprintf(sock, "    \"MaxPacketBytes\": %d,\r\n", BrokerLimits::MaxPacketBytes);
    fdprintf(sock, "    \"MaxTopicBytes\": %d,\r\n", BrokerLimits::MaxTopicBytes);
    fdprintf(sock, "    \"MaxPayloadBytes\": %d,\r\n", BrokerLimits::MaxPayloadBytes);
    fdprintf(sock, "    \"MaxSubsPerClient\": %d,\r\n", BrokerLimits::MaxSubsPerClient);
    fdprintf(sock, "    \"MaxSubsTotal\": %d,\r\n", BrokerLimits::MaxSubsTotal);
    fdprintf(sock, "    \"TxQueueDepthPerClient\": %d,\r\n", BrokerLimits::TxQueueDepthPerClient);
    fdprintf(sock, "    \"PayloadBlockCount\": %d,\r\n", BrokerLimits::PayloadBlockCount);
    fdprintf(sock, "    \"PayloadBlockSize\": %d,\r\n", BrokerLimits::PayloadBlockSize);
    fdprintf(sock, "    \"MaxInternedTopics\": %d,\r\n", BrokerLimits::MaxInternedTopics);
    fdprintf(sock, "    \"MaxInternedBytes\": %d,\r\n", BrokerLimits::MaxInternedBytes);
    fdprintf(sock, "    \"ParserReadChunk\": %d,\r\n", BrokerLimits::ParserReadChunk);
    fdprintf(sock, "    \"CompileMaxKeepAliveSec\": %d,\r\n", BrokerLimits::MaxKeepAliveSec);
    fdprintf(sock, "    \"CompileConnectHandshakeTimeoutSec\": %d\r\n",
             BrokerLimits::ConnectHandshakeTimeoutSec);
}

// GET api/broker/status — live metrics plus plain/TLS listener configuration.
int BrokerStatusHandler(int sock, HTTP_Request &req)
{
    (void)req;
    const mqtt_broker::BrokerMetrics &m = BrokerServerMetrics();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "Cache-Control: no-cache\r\n\r\n");

    fdprintf(sock, "{\r\n");
    fdprintf(sock, "  \"status\": \"ok\",\r\n");
    fdprintf(sock, "  \"platform\": \"%s\",\r\n", kPlatformName);
    fdprintf(sock, "  \"uptime\": %lu,\r\n", Secs);
    fdprintf(sock, "  \"plain_tcp_enabled\": %s,\r\n",
             BrokerConfigPlainTcpEnabled() ? "true" : "false");
    fdprintf(sock, "  \"plain_tcp_port\": %d,\r\n", BrokerConfigPlainTcpPort());
    fdprintf(sock, "  \"listener_active\": %s,\r\n",
             BrokerServerListenerActive() ? "true" : "false");
    fdprintf(sock, "  \"tls_enabled\": %s,\r\n", BrokerConfigTlsEnabled() ? "true" : "false");
    fdprintf(sock, "  \"tls_port\": %d,\r\n", BrokerConfigTlsPort());
    fdprintf(sock, "  \"tls_listener_active\": %s,\r\n",
             BrokerServerTlsListenerActive() ? "true" : "false");
    fdprintf(sock, "  \"tls_clients_connected\": %u,\r\n", BrokerServerTlsClientCount());
    fdprintf(sock, "  \"metrics\": {\r\n");
    fdprintf(sock, "    \"clients_connected\": %u,\r\n", m.clients_connected);
    fdprintf(sock, "    \"clients_connected_peak\": %u,\r\n", m.clients_connected_peak);
    fdprintf(sock, "    \"connect_accept\": %u,\r\n", m.connect_accept);
    fdprintf(sock, "    \"connect_reject\": %u,\r\n", m.connect_reject);
    fdprintf(sock, "    \"publish_received\": %u,\r\n", m.publish_received);
    fdprintf(sock, "    \"publish_sent\": %u,\r\n", m.publish_sent);
    fdprintf(sock, "    \"dropped_too_large\": %u,\r\n", m.dropped_too_large);
    fdprintf(sock, "    \"dropped_quota\": %u,\r\n", m.dropped_quota);
    fdprintf(sock, "    \"quota_drop_count\": %u,\r\n", m.quota_drop_count);
    fdprintf(sock, "    \"will_published\": %u,\r\n", m.will_published);
    fdprintf(sock, "    \"session_takeovers\": %u,\r\n", m.session_takeovers);
    fdprintf(sock, "    \"pool_exhaustion\": %u,\r\n", m.pool_exhaustion);
    fdprintf(sock, "    \"keepalive_disconnects\": %u,\r\n", m.keepalive_disconnects);
    fdprintf(sock, "    \"slow_consumer_disconnects\": %u,\r\n", m.slow_consumer_disconnects);
    fdprintf(sock, "    \"parser_errors\": %u,\r\n", m.parser_errors);
    fdprintf(sock, "    \"tx_logical_high_water\": %u\r\n", m.tx_logical_high_water);
    fdprintf(sock, "  },\r\n");
    fdprintf(sock, "  \"limits\": {\r\n");
    write_limits_json(sock);
    fdprintf(sock, "  }\r\n");
    fdprintf(sock, "}\r\n");

    return 1;
}

CallBackFunctionPageHandler BrokerStatusCB("api/broker/status", BrokerStatusHandler, tGet, 1);
