// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "broker_server.hpp"

#include "broker_config.hpp"
#include "mqtt_auth_store.h"
#include "ssl_service.h"

#include <mqtt_broker/broker_policy.hpp>

#include <mqtt_broker/broker.hpp>
#include <mqtt_broker/broker_limits.hpp>
#include <mqtt_broker/debug_log.hpp>

#include <crypto/ssl.h>
#include <fdprintf.h>
#include <iointernal.h>
#include <iosys.h>
#include <nbrtos.h>
#include <stdint.h>
#include <tcp.h>

// NetBurner TCP/TLS integration for mqtt_broker::Broker. Maps each accepted
// socket to a BrokerTransport (read/write/close callbacks) and drives the
// broker from a single RTOS task: select() for readability, on_readable() for
// inbound MQTT, tick() for keepalive/session expiry, drain_tx() for outbound
// frames. Plain and MQTTS listeners are opened independently from config.

static const int kListenBacklog = 16;
// Two slots reserved for plain + TLS listen sockets; remainder are client fds.
static const int kMaxFds = BrokerLimits::MaxTcpClients + 2;

enum class FdKind : uint8_t {
    None = 0,
    PlainListen,
    TlsListen,
    PlainClient,
    TlsClient,
};

// Tracks every fd the task selects on; transport_id links client fds to Broker.
struct FdSlot {
    int fd;
    uint16_t transport_id;
    FdKind kind;
};

static mqtt_broker::Broker gBroker;
static FdSlot gFds[kMaxFds] = {};
static OS_CRIT gFdCrit;
static int gListenFd = -1;
static int gTlsListenFd = -1;
static int gActivePort = 0;
static int gTlsActivePort = 0;
static bool gListenerActive = false;
static bool gTlsListenerActive = false;

static void broker_debug_to_console(const char *line)
{
    if (line != nullptr) {
        iprintf("%s\r\n", line);
    }
}

// BrokerTransport read — returns -1 when no data yet (non-blocking poll).
static int transport_read(void *ctx, uint8_t *buf, size_t cap)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    if (!dataavail(fd)) {
        return -1;
    }
    int n = read(fd, reinterpret_cast<char *>(buf), static_cast<int>(cap));
    if (n > 0) {
        ClrHaveError(fd);
    }
    return n;
}

// Partial writes are normal when the socket TX buffer fills; broker retries via drain_tx.
static int transport_write(void *ctx, const uint8_t *data, size_t len)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    size_t off = 0;
    while (off < len) {
        if (!writeavail(fd)) {
            return static_cast<int>(off);
        }
        int n = write(fd, reinterpret_cast<const char *>(data + off), static_cast<int>(len - off));
        if (n < 0) {
            return (off > 0) ? static_cast<int>(off) : n;
        }
        if (n == 0) {
            return static_cast<int>(off);
        }
        off += static_cast<size_t>(n);
        ClrHaveError(fd);
    }
    return static_cast<int>(len);
}

static void transport_close(void *ctx)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    close(fd);
}

static bool add_fd(int fd, uint16_t transport_id, FdKind kind)
{
    OSCriticalSectionObj lock(gFdCrit);
    for (int i = 0; i < kMaxFds; ++i) {
        if (gFds[i].fd == 0) {
            gFds[i].fd = fd;
            gFds[i].transport_id = transport_id;
            gFds[i].kind = kind;
            return true;
        }
    }
    return false;
}

static void remove_fd(int fd)
{
    OSCriticalSectionObj lock(gFdCrit);
    for (int i = 0; i < kMaxFds; ++i) {
        if (gFds[i].fd == fd) {
            gFds[i].fd = 0;
            gFds[i].transport_id = 0xFFFF;
            gFds[i].kind = FdKind::None;
            break;
        }
    }
}

static int count_tls_clients()
{
    int count = 0;
    for (int i = 0; i < kMaxFds; ++i) {
        if (gFds[i].fd > 0 && gFds[i].kind == FdKind::TlsClient) {
            count++;
        }
    }
    return count;
}

static bool is_client_kind(FdKind kind)
{
    return kind == FdKind::PlainClient || kind == FdKind::TlsClient;
}

uint32_t gBrokerConnectedCount()
{
    return static_cast<uint32_t>(gBroker.connected_count());
}

uint32_t gBrokerPublishReceived()
{
    return gBroker.metrics().publish_received;
}

uint32_t gBrokerPublishSent()
{
    return gBroker.metrics().publish_sent;
}

const mqtt_broker::BrokerMetrics &BrokerServerMetrics()
{
    return gBroker.metrics();
}

bool BrokerServerListenerActive()
{
    return gListenerActive;
}

bool BrokerServerTlsListenerActive()
{
    return gTlsListenerActive;
}

uint32_t BrokerServerTlsClientCount()
{
    return static_cast<uint32_t>(count_tls_clients());
}

static void close_listener()
{
    if (gListenFd <= 0) {
        gListenerActive = false;
        gActivePort = 0;
        return;
    }

    close(gListenFd);
    remove_fd(gListenFd);
    gListenFd = -1;
    gActivePort = 0;
    gListenerActive = false;
}

static void close_tls_listener()
{
    if (gTlsListenFd <= 0) {
        gTlsListenerActive = false;
        gTlsActivePort = 0;
        return;
    }

    close(gTlsListenFd);
    remove_fd(gTlsListenFd);
    gTlsListenFd = -1;
    gTlsActivePort = 0;
    gTlsListenerActive = false;
}

static bool open_listener(int port)
{
    close_listener();

    int listen_fd = listen(INADDR_ANY, port, kListenBacklog);
    if (listen_fd <= 0) {
        iprintf("[MQTT] listen(%d) failed: %d\r\n", port, listen_fd);
        return false;
    }

    if (!add_fd(listen_fd, 0xFFFF, FdKind::PlainListen)) {
        close(listen_fd);
        iprintf("[MQTT] fd table full, cannot track plain listen socket\r\n");
        return false;
    }

    gListenFd = listen_fd;
    gActivePort = port;
    gListenerActive = true;
    iprintf("[MQTT] Listening on plain TCP %d\r\n", port);
    return true;
}

static bool open_tls_listener(int port)
{
    close_tls_listener();

    // MQTTS requires a loaded cert; skip rather than accept on an unencrypted listen fd.
    if (!SslCertReady()) {
        iprintf("[MQTT] TLS listener skipped — certificate not ready\r\n");
        return false;
    }

    int listen_fd = listen(INADDR_ANY, port, kListenBacklog);
    if (listen_fd <= 0) {
        iprintf("[MQTT] TLS listen(%d) failed: %d\r\n", port, listen_fd);
        return false;
    }

    if (!add_fd(listen_fd, 0xFFFF, FdKind::TlsListen)) {
        close(listen_fd);
        iprintf("[MQTT] fd table full, cannot track TLS listen socket\r\n");
        return false;
    }

    gTlsListenFd = listen_fd;
    gTlsActivePort = port;
    gTlsListenerActive = true;
    iprintf("[MQTT] Listening on MQTTS %d\r\n", port);
    return true;
}

// Reconcile plain listener with runtime config (enable flag and port changes).
static void sync_plain_listener()
{
    if (!BrokerConfigPlainTcpEnabled()) {
        if (gListenFd > 0) {
            iprintf("[MQTT] Plain TCP listener disabled\r\n");
            close_listener();
        }
        return;
    }

    const int port = BrokerConfigPlainTcpPort();
    if (gListenFd <= 0 || port != gActivePort) {
        if (gListenFd > 0) {
            iprintf("[MQTT] Plain port change %d -> %d, reopening listener\r\n", gActivePort, port);
        }
        open_listener(port);
    }
}

// Same as sync_plain_listener but also tears down MQTTS when the cert disappears.
static void sync_tls_listener()
{
    if (!BrokerConfigTlsEnabled()) {
        if (gTlsListenFd > 0) {
            iprintf("[MQTT] MQTTS listener disabled\r\n");
            close_tls_listener();
        }
        return;
    }

    if (!SslCertReady()) {
        if (gTlsListenFd > 0) {
            iprintf("[MQTT] MQTTS listener closed — certificate not ready\r\n");
            close_tls_listener();
        }
        return;
    }

    const int port = BrokerConfigTlsPort();
    if (gTlsListenFd <= 0 || port != gTlsActivePort) {
        if (gTlsListenFd > 0) {
            iprintf("[MQTT] TLS port change %d -> %d, reopening listener\r\n", gTlsActivePort, port);
        }
        open_tls_listener(port);
    }
}

// Called each loop iteration so admin UI changes take effect without reboot.
static void sync_listeners()
{
    BrokerConfigApplyPolicy();
    sync_plain_listener();
    sync_tls_listener();
}

static bool attach_client(int client_fd, FdKind kind)
{
    // TLS clients have a separate cap so plain connections still fit under MaxTcpClients.
    if (kind == FdKind::TlsClient &&
        count_tls_clients() >= BrokerLimits::MaxTlsClients) {
        mqtt_broker::broker_debug_log("[MQTT DBG] TLS accept fd=%d rejected: max TLS clients (%d)",
                                      client_fd, BrokerLimits::MaxTlsClients);
        return false;
    }

    setsockoption(client_fd, SO_NONAGLE | SO_PUSH);
    mqtt_broker::BrokerTransportOps ops = {transport_read, transport_write, transport_close};
    mqtt_broker::BrokerTransport tr = {
        reinterpret_cast<void *>(static_cast<intptr_t>(client_fd)), ops, true};
    uint16_t tid = gBroker.attach_transport(tr);
    if (tid != 0xFFFF && add_fd(client_fd, tid, kind)) {
        const char *label = (kind == FdKind::TlsClient) ? "MQTTS" : "TCP";
        iprintf("[MQTT] %s client fd=%d transport=%u\r\n", label, client_fd, tid);
        return true;
    }

    if (tid == 0xFFFF) {
        mqtt_broker::broker_debug_log("[MQTT DBG] accept fd=%d rejected: max clients (%d)", client_fd,
                                      BrokerLimits::MaxTcpClients);
    } else {
        mqtt_broker::broker_debug_log("[MQTT DBG] accept fd=%d rejected: fd table full", client_fd);
    }
    return false;
}

static mqtt_broker::ReasonCode broker_auth_check(const char *client_id, const char *username,
                                                 const uint8_t *password, size_t password_len)
{
    (void)client_id;
    // Runtime toggle: with auth disabled the broker stays open.
    if (!BrokerConfigAuthRequired()) {
        return mqtt_broker::ReasonCode::Success;
    }
    if (username == nullptr || username[0] == '\0') {
        return BrokerConfigAllowAnonymous() ? mqtt_broker::ReasonCode::Success
                                            : mqtt_broker::ReasonCode::NotAuthorized;
    }
    return MqttAuthVerify(username, password, password_len)
               ? mqtt_broker::ReasonCode::Success
               : mqtt_broker::ReasonCode::BadUserNameOrPassword;
}

void BrokerServerInit()
{
    mqtt_broker::broker_set_debug_log(broker_debug_to_console);
    BrokerConfigApplyPolicy();
    MqttAuthStoreInit();
    mqtt_broker::broker_set_auth_handler(broker_auth_check);
    iprintf("[MQTT] Broker core initialized (QoS 0/1/2, plain TCP + MQTTS, auth %s)\r\n",
            BrokerConfigAuthRequired() ? "required" : "open");
    iprintf("[MQTT] Admin UI: http://<device-ip>/\r\n");
}

void BrokerServerTask(void *pd)
{
    (void)pd;
    sync_listeners();
    if (!gListenerActive && !gTlsListenerActive) {
        iprintf("[MQTT] No listeners active; broker task idle\r\n");
    }

    uint32_t last_stats_tick = Secs;
    while (1) {
        sync_listeners();
        fd_set read_fds;
        FD_ZERO(&read_fds);
        for (int i = 0; i < kMaxFds; ++i) {
            if (gFds[i].fd > 0) {
                FD_SET(gFds[i].fd, &read_fds);
            }
        }

        // NetBurner can set the exception set after a successful read/write; rely on
        // read() returning 0 or an error instead of select() err_fds for clients.
        // Zero timeout when outbound data is queued so drain_tx runs promptly.
        int select_timeout = gBroker.has_pending_tx() ? 0 : 1;
        select(FD_SETSIZE, &read_fds, NULL, NULL, select_timeout);
        gBroker.tick(Secs);

        if (Secs - last_stats_tick >= 30u) {
            last_stats_tick = Secs;
            iprintf("[MQTT] clients=%lu tls=%lu publish_rx=%lu publish_tx=%lu\r\n",
                    static_cast<unsigned long>(gBrokerConnectedCount()),
                    static_cast<unsigned long>(count_tls_clients()),
                    static_cast<unsigned long>(gBrokerPublishReceived()),
                    static_cast<unsigned long>(gBrokerPublishSent()));
        }

        for (int i = 0; i < kMaxFds; ++i) {
            int fd = gFds[i].fd;
            if (fd <= 0) {
                continue;
            }

            const FdKind kind = gFds[i].kind;
            if (kind == FdKind::PlainListen || kind == FdKind::TlsListen) {
                if (haserror(fd)) {
                    if (kind == FdKind::PlainListen) {
                        iprintf("[MQTT] plain listen socket error, reopening port %d\r\n",
                                gActivePort);
                        open_listener(gActivePort);
                    } else {
                        iprintf("[MQTT] TLS listen socket error, reopening port %d\r\n",
                                gTlsActivePort);
                        open_tls_listener(gTlsActivePort);
                    }
                    continue;
                }
                if (!FD_ISSET(fd, &read_fds)) {
                    continue;
                }

                if (kind == FdKind::PlainListen) {
                    int client = accept(fd, NULL, NULL, 0);
                    if (client > 0) {
                        if (!attach_client(client, FdKind::PlainClient)) {
                            close(client);
                        }
                    }
                } else {
                    int client = SSL_accept(fd, NULL, NULL, 0);
                    if (client > 0) {
                        if (!attach_client(client, FdKind::TlsClient)) {
                            close(client);
                        }
                    }
                }
                continue;
            }

            uint16_t tid = gFds[i].transport_id;
            // Cap reads per fd so a flood publisher cannot starve other clients'
            // on_readable() — unread PINGREQs were tripping keep-alive (~180s).
            static const int kMaxReadablePassesPerFd = 64;
            int read_passes = 0;
            while (dataavail(fd) && gBroker.transport_attached(tid) &&
                   read_passes < kMaxReadablePassesPerFd) {
                gBroker.on_readable(tid);
                read_passes++;
            }

            // NetBurner select() does not flag closed sockets readable, so poll
            // the TCP state each pass (~1s). Detaching a dead connection here is
            // what arms Will messages for abruptly disconnected clients instead
            // of waiting for the keep-alive timeout. SSL fds share the same TCP
            // state accessor (SSL_TcpGetSocketState is TcpGetSocketState).
            if (gBroker.transport_attached(tid) && !dataavail(fd) &&
                TcpGetSocketState(fd) != TCP_STATE_ESTABLISHED) {
                iprintf("[MQTT] transport %u connection lost (tcp state %d)\r\n", tid,
                        TcpGetSocketState(fd));
                gBroker.detach_transport(tid);
            }

            if (!gBroker.transport_attached(tid)) {
                remove_fd(fd);
            }
        }

        // Round-robin drain_tx across all clients; multiple passes cover fan-out
        // publishes where one client's read triggers replies to several others.
        static const int kDrainPasses = 8;
        for (int pass = 0; pass < kDrainPasses; ++pass) {
            for (int i = 0; i < kMaxFds; ++i) {
                if (gFds[i].fd > 0 && is_client_kind(gFds[i].kind)) {
                    uint16_t tid = gFds[i].transport_id;
                    gBroker.drain_tx(tid);
                    if (!gBroker.transport_attached(tid)) {
                        remove_fd(gFds[i].fd);
                    }
                }
            }
            if (!gBroker.has_pending_tx()) {
                break;
            }
        }
    }
}
