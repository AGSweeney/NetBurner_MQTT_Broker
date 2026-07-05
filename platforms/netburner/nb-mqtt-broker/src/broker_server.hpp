#ifndef BROKER_SERVER_HPP
#define BROKER_SERVER_HPP

// NetBurner platform glue for mqtt_broker::Broker — exposes init/task entry
// points and lightweight metrics for the admin UI. BrokerServerTask owns the
// TCP/TLS event loop: select() on tracked fds, on_readable/tick/drain_tx on the
// core broker, and optional plain (1883) plus MQTTS listeners from broker_config.

#include <mqtt_broker/metrics.hpp>

#include <stdint.h>

// One-time setup: debug log hook, policy, auth store, and auth callback.
void BrokerServerInit();
// RTOS task body — runs the select loop until the task is stopped.
void BrokerServerTask(void *pd);

// Metrics surfaced to the web UI and periodic stats log.
uint32_t gBrokerConnectedCount();
uint32_t gBrokerPublishReceived();
uint32_t gBrokerPublishSent();
const mqtt_broker::BrokerMetrics &BrokerServerMetrics();

// Listener state — plain TCP and MQTTS may be enabled independently via config.
bool BrokerServerListenerActive();
bool BrokerServerTlsListenerActive();
uint32_t BrokerServerTlsClientCount();

#endif
