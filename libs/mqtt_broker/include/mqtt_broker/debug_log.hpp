#ifndef MQTT_BROKER_DEBUG_LOG_HPP
#define MQTT_BROKER_DEBUG_LOG_HPP

// Optional broker debug logging. The platform installs a sink via
// broker_set_debug_log(); broker_debug_log() is a no-op when no sink is set.

#include "mqtt_broker/mqtt_types.hpp"

namespace mqtt_broker {

using BrokerDebugLogFn = void (*)(const char *line);

void broker_set_debug_log(BrokerDebugLogFn fn);  // Install or clear the debug sink (nullptr disables)
const char *reason_code_name(ReasonCode rc);     // Human-readable MQTT 5 reason code for log lines
const char *packet_type_name(PacketType type);   // Human-readable packet type label
void broker_debug_log(const char *fmt, ...);     // printf-style emit; truncated to 240 chars

}  // namespace mqtt_broker

#endif
