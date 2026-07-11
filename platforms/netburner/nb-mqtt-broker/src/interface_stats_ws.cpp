// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// WebSocket endpoint ws/stats — streams broker metrics and per-interface
// traffic counters as JSON for the admin dashboard (Chart.js).

#include "interface_stats_ws.h"

#include "broker_config.hpp"
#include "broker_server.hpp"

#include <init.h>
#include <ip.h>
#include <netinterface.h>
#include <nbrtos.h>
#include <predef.h>
#include <string.h>
#include <system.h>
#include <websockets.h>

#if defined(PLATFORM_SOMRT1061) || defined(SOMRT1061)
static const char *kPlatformName = "SOMRT1061";
#elif defined(PLATFORM_MODRT1171) || defined(MODRT1171)
static const char *kPlatformName = "MODRT1171";
#elif defined(PLATFORM_MODM7AE70) || defined(MODM7AE70)
static const char *kPlatformName = "MODM7AE70";
#elif defined(PLATFORM_MOD5441X) || defined(MOD5441X)
static const char *kPlatformName = "MOD5441X";
#elif defined(PLATFORM_NANO54415) || defined(NANO54415)
static const char *kPlatformName = "NANO54415";
#else
static const char *kPlatformName = "NetBurner";
#endif

static int sWsFd = -1;
static OS_SEM sWsReadySem;

static void format_mac(char *buf, size_t buf_size, const MACADR &mac)
{
    if (buf_size < 18) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac.GetByte(0), mac.GetByte(1),
             mac.GetByte(2), mac.GetByte(3), mac.GetByte(4), mac.GetByte(5));
}

static int append_interfaces_json(char *buf, int offset, int buf_size)
{
    int if_number = GetFirstInterface();
    bool first = true;

    while (if_number > 0 && offset < buf_size - 64) {
        InterfaceBlock *if_block = GetInterfaceBlock(if_number);
        if (if_block != nullptr) {
            char mac_buf[24];
            char ip_buf[16];
            char mask_buf[16];
            char gate_buf[16];
            format_mac(mac_buf, sizeof(mac_buf), InterfaceMAC(if_number));
            snprintf(ip_buf, sizeof(ip_buf), "%hI", InterfaceIP(if_number));
            snprintf(mask_buf, sizeof(mask_buf), "%hI", InterfaceMASK(if_number));
            snprintf(gate_buf, sizeof(gate_buf), "%hI", InterfaceGate(if_number));

            const bool link_up = GetInterfaceLink(if_number);
            const int link_speed = link_up ? InterfaceLinkSpeed(if_number) : 0;
            const bool full_duplex = link_up ? InterfaceLinkDuplex(if_number) : false;
            const unsigned long rx_bytes = static_cast<unsigned long>(if_block->EthifInOctets);
            const unsigned long tx_bytes = static_cast<unsigned long>(if_block->EthifOutOctets);
            const char *if_name = InterfaceName(if_number);
            if (if_name == nullptr) {
                if_name = "unknown";
            }

            const int written =
                snprintf(buf + offset, buf_size - offset,
                         "%s{\"id\":%d,\"name\":\"%s\",\"mac\":\"%s\","
                         "\"link_up\":%s,\"link_speed_mbps\":%d,\"link_full_duplex\":%s,"
                         "\"rx_bytes\":%lu,\"tx_bytes\":%lu,"
                         "\"ipv4\":\"%s\",\"ipv4_mask\":\"%s\",\"ipv4_gateway\":\"%s\"}",
                         first ? "" : ",", if_number, if_name, mac_buf, link_up ? "true" : "false",
                         link_speed, full_duplex ? "true" : "false", rx_bytes, tx_bytes, ip_buf,
                         mask_buf, gate_buf);
            if (written <= 0 || written >= buf_size - offset) {
                break;
            }
            offset += written;
            first = false;
        }
        if_number = GetNextInterface(if_number);
    }
    return offset;
}

static int build_stats_json(char *buf, int buf_size)
{
    const mqtt_broker::BrokerMetrics &m = BrokerServerMetrics();
    int offset = snprintf(
        buf, buf_size,
        "{\"ts\":%lu,\"platform\":\"%s\",\"uptime\":%lu,\"broker\":{"
        "\"clients_connected\":%lu,\"clients_connected_peak\":%lu,"
        "\"publish_received\":%lu,\"publish_sent\":%lu,"
        "\"connect_accept\":%lu,\"connect_reject\":%lu,"
        "\"parser_errors\":%lu,\"slow_consumer_disconnects\":%lu,"
        "\"tls_clients_connected\":%lu,"
        "\"listener_active\":%s,\"tls_listener_active\":%s,"
        "\"plain_tcp_enabled\":%s,\"plain_tcp_port\":%d,"
        "\"tls_enabled\":%s,\"tls_port\":%d"
        "},\"interfaces\":[",
        static_cast<unsigned long>(Secs), kPlatformName, static_cast<unsigned long>(Secs),
        static_cast<unsigned long>(m.clients_connected),
        static_cast<unsigned long>(m.clients_connected_peak),
        static_cast<unsigned long>(m.publish_received), static_cast<unsigned long>(m.publish_sent),
        static_cast<unsigned long>(m.connect_accept), static_cast<unsigned long>(m.connect_reject),
        static_cast<unsigned long>(m.parser_errors),
        static_cast<unsigned long>(m.slow_consumer_disconnects),
        static_cast<unsigned long>(BrokerServerTlsClientCount()),
        BrokerServerListenerActive() ? "true" : "false",
        BrokerServerTlsListenerActive() ? "true" : "false",
        BrokerConfigPlainTcpEnabled() ? "true" : "false", BrokerConfigPlainTcpPort(),
        BrokerConfigTlsEnabled() ? "true" : "false", BrokerConfigTlsPort());

    if (offset <= 0 || offset >= buf_size - 32) {
        return -1;
    }

    offset = append_interfaces_json(buf, offset, buf_size);
    if (offset < 0 || offset >= buf_size - 4) {
        return -1;
    }

    const int end_written = snprintf(buf + offset, buf_size - offset, "]}");
    if (end_written <= 0) {
        return -1;
    }
    return offset + end_written;
}

static void on_ws_connect(int ws_fd)
{
    if (sWsFd >= 0) {
        close(ws_fd);
        return;
    }
    sWsFd = ws_fd;
    OSSemPost(&sWsReadySem);
}

static CallBackWSEndPoint sStatsWsEndpoint("ws/stats", on_ws_connect);

static void StatsWsInputTask(void *pd)
{
    (void)pd;
    fd_set read_fds;
    fd_set error_fds;

    while (1) {
        if (sWsFd <= 0) {
            OSSemPend(&sWsReadySem, 0);
            continue;
        }

        FD_ZERO(&read_fds);
        FD_ZERO(&error_fds);
        FD_SET(sWsFd, &read_fds);
        FD_SET(sWsFd, &error_fds);
        if (select(1, &read_fds, nullptr, &error_fds, TICKS_PER_SECOND / 4)) {
            if (FD_ISSET(sWsFd, &error_fds)) {
                close(sWsFd);
                sWsFd = -1;
                continue;
            }
            if (FD_ISSET(sWsFd, &read_fds) && dataavail(sWsFd)) {
                char drain_buf[64];
                while (dataavail(sWsFd)) {
                    read(sWsFd, drain_buf, sizeof(drain_buf));
                }
            }
        }
    }
}

static void InterfaceStatsWsTask(void *pd)
{
    (void)pd;
    char json_buf[2048];

    while (1) {
        if (sWsFd <= 0) {
            OSTimeDly(TICKS_PER_SECOND);
            continue;
        }

        const int payload_len = build_stats_json(json_buf, static_cast<int>(sizeof(json_buf)));
        if (payload_len > 0) {
            if (writeall(sWsFd, json_buf, payload_len) != payload_len) {
                close(sWsFd);
                sWsFd = -1;
                continue;
            }
            NB::WebSocket::ws_flush(sWsFd);
        }

        OSTimeDly(TICKS_PER_SECOND * 2);
    }
}

void InterfaceStatsWsInit()
{
    OSSemInit(&sWsReadySem, 0);
    OSSimpleTaskCreatewName(StatsWsInputTask, OSGetNextPrio(OSNextPrio::Below), "StatsWsIn");
    OSSimpleTaskCreatewName(InterfaceStatsWsTask, OSGetNextPrio(OSNextPrio::Below), "StatsWs");
}
