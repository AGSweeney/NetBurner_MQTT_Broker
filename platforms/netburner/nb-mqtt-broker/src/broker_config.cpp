// NetBurner config_server registration and accessors for MQTT broker settings.
// Defines persisted config_obj trees and thin getters used by broker_server.

#include "broker_config.hpp"

#include <mqtt_broker/broker_limits.hpp>
#include <mqtt_broker/broker_policy.hpp>

#include <config_server.h>

// Plain TCP listener enable flag and port (default 1883).
class BrokerListener : public config_obj
{
   public:
    config_bool m_plainTcpEnabled{true, "PlainTcpEnabled"};
    config_int_limit m_plainTcpPort{1883, 1, 65535, "PlainTcpPort"};
    config_bool m_tlsEnabled{false, "TlsEnabled"};
    config_int_limit m_tlsPort{8883, 1, 65535, "TlsPort"};
    ConfigEndMarker;

    BrokerListener(config_obj &owner, const char *name, const char *desc)
        : config_obj(owner, name, desc)
    {
    }
};

// CONNECT keep-alive cap and handshake timeout exposed to the portable broker.
class BrokerSessionPolicy : public config_obj
{
   public:
    config_int_limit m_maxKeepAliveSec{BrokerLimits::MaxKeepAliveSec, 1, 3600,
                                       "MaxKeepAliveSec"};
    config_int_limit m_connectHandshakeTimeoutSec{BrokerLimits::ConnectHandshakeTimeoutSec, 1, 120,
                                                  "ConnectHandshakeTimeoutSec"};
    ConfigEndMarker;

    BrokerSessionPolicy(config_obj &owner, const char *name, const char *desc)
        : config_obj(owner, name, desc)
    {
    }
};

// MQTT CONNECT authentication policy: require credentials and/or allow anonymous clients.
class BrokerAuth : public config_obj
{
   public:
    config_bool m_authRequired{false, "AuthRequired"};
    config_bool m_allowAnonymous{false, "AllowAnonymous"};
    ConfigEndMarker;

    BrokerAuth(config_obj &owner, const char *name, const char *desc)
        : config_obj(owner, name, desc)
    {
    }
};

static BrokerListener gBrokerListener(appdata, "BrokerListener", "MQTT plain TCP listener.");
static BrokerSessionPolicy gBrokerSessionPolicy(appdata, "BrokerSessionPolicy",
                                                "Session timing policy.");
static BrokerAuth gBrokerAuth(appdata, "BrokerAuth", "MQTT client authentication.");

// Keeps runtime policy within BrokerLimits caps from the portable library build.
static int clamp_policy_int(int value, int compile_max)
{
    if (compile_max > 0 && (value <= 0 || value > compile_max)) {
        return compile_max;
    }
    return value;
}

void BrokerConfigApplyPolicy()
{
    mqtt_broker::BrokerPolicy policy;
    policy.max_keep_alive_sec =
        clamp_policy_int(static_cast<int>(gBrokerSessionPolicy.m_maxKeepAliveSec),
                         BrokerLimits::MaxKeepAliveSec);
    policy.connect_handshake_timeout_sec =
        clamp_policy_int(static_cast<int>(gBrokerSessionPolicy.m_connectHandshakeTimeoutSec),
                         BrokerLimits::ConnectHandshakeTimeoutSec);
    mqtt_broker::broker_set_policy(policy);
}

bool BrokerConfigPlainTcpEnabled()
{
    return static_cast<bool>(gBrokerListener.m_plainTcpEnabled);
}

int BrokerConfigPlainTcpPort()
{
    return static_cast<int>(gBrokerListener.m_plainTcpPort);
}

bool BrokerConfigTlsEnabled()
{
    return static_cast<bool>(gBrokerListener.m_tlsEnabled);
}

int BrokerConfigTlsPort()
{
    return static_cast<int>(gBrokerListener.m_tlsPort);
}

bool BrokerConfigAuthRequired()
{
    return static_cast<bool>(gBrokerAuth.m_authRequired);
}

bool BrokerConfigAllowAnonymous()
{
    return static_cast<bool>(gBrokerAuth.m_allowAnonymous);
}

void BrokerConfigSetAuth(bool auth_required, bool allow_anonymous)
{
    gBrokerAuth.m_authRequired = auth_required;
    gBrokerAuth.m_allowAnonymous = allow_anonymous;
    SaveConfigToStorage();
}
