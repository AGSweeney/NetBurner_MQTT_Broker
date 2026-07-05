#ifndef WEBUI_CONFIG_H
#define WEBUI_CONFIG_H

// Config-server object for admin web protocol options (HTTP/HTTPS enable and
// whether all pages require login). Persisted with SaveConfigToStorage().

#include <config_obj.h>

class WebUIConfig : public config_obj
{
   public:
    config_bool m_httpsEnabled{true, "HTTPS Enabled", "Enable HTTPS on port 443"};
    config_bool m_httpEnabled{true, "HTTP Enabled", "Enable HTTP on port 80"};
    config_bool m_requireAuthForAll{false, "Require Auth For All Pages",
                                    "Require login for all web pages, not just configuration."};
    ConfigEndMarker;

    WebUIConfig(config_obj &owner, const char *name, const char *desc = nullptr)
        : config_obj(owner, name, desc)
    {
    }
};

extern WebUIConfig gWebUIConfig;

#endif
