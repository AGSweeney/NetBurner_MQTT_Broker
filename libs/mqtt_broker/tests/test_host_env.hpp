#ifndef MQTT_BROKER_TEST_HOST_ENV_HPP
#define MQTT_BROKER_TEST_HOST_ENV_HPP

namespace mqtt_broker {

// Call once at the start of each test main(). Idempotent. On Windows, routes
// assert()/abort() diagnostics to stderr instead of modal "Debug Error" dialogs.
void init_test_host_env();

}  // namespace mqtt_broker

#endif
