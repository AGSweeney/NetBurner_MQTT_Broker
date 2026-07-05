#ifndef WEBUI_CONFIG_H
#define WEBUI_CONFIG_H

/******************************************************************************
* Copyright 1998-2024 NetBurner, Inc.  ALL RIGHTS RESERVED
*
*    Permission is hereby granted to purchasers of NetBurner Hardware to use or
*    modify this computer program for any use as long as the resultant program
*    is only executed on NetBurner provided hardware.
*
*    No other rights to use this program or its derivatives in part or in
*    whole are granted.
*
*    It may be possible to license this or other NetBurner software for use on
*    non-NetBurner Hardware. Contact sales@Netburner.com for more information.
*
*    NetBurner makes no representation or warranties with respect to the
*    performance of this computer program, and specifically disclaims any
*    responsibility for any damages, special or consequential, connected with
*    the use of this program.
*
* NetBurner
* 16855 W Bernardo Dr
* San Diego, CA 92127
* www.netburner.com
******************************************************************************/

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
