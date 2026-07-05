#ifndef MQTT_BROKER_BROKER_POLICY_HPP
#define MQTT_BROKER_BROKER_POLICY_HPP

#include "mqtt_broker/mqtt_types.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

struct BrokerPolicy {
    int max_keep_alive_sec;
    int connect_handshake_timeout_sec;
};

const BrokerPolicy &broker_policy();
void broker_set_policy(const BrokerPolicy &policy);

// Optional CONNECT credential check installed by the platform layer. Called
// before any session state is created. username/password are nullptr when the
// corresponding CONNECT flag is absent. Return Success to admit the client, or
// BadUserNameOrPassword / NotAuthorized to reject with that CONNACK reason.
using AuthCheckFn = ReasonCode (*)(const char *client_id, const char *username,
                                   const uint8_t *password, size_t password_len);

void broker_set_auth_handler(AuthCheckFn fn);
AuthCheckFn broker_auth_handler();

}  // namespace mqtt_broker

#endif
