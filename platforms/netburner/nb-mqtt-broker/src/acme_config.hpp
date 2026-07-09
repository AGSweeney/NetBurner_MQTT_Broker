// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef ACME_CONFIG_HPP
#define ACME_CONFIG_HPP

// Persisted Let's Encrypt (ACME) settings and platform capability gate.
// ACME enrollment is supported on SOMRT1061 only (NNDK 3.5+ acmeRFC8555Servlet).

#include <config_obj.h>

#if defined(PLATFORM_SOMRT1061) || defined(SOMRT1061)
constexpr bool kAcmePlatformSupported = true;
#else
constexpr bool kAcmePlatformSupported = false;
#endif

inline bool AcmePlatformSupported() { return kAcmePlatformSupported; }

class AcmeConfig : public config_obj
{
   public:
    config_bool m_enabled{false, "AcmeEnabled"};
    config_string m_commonName{"", "AcmeCommonName"};
    config_string m_altNames{"", "AcmeAltNames"};
    config_string m_email{"", "AcmeEmail"};
    config_bool m_useStaging{false, "AcmeUseStaging"};
    ConfigEndMarker;

    AcmeConfig(config_obj &owner, const char *name, const char *desc = nullptr)
        : config_obj(owner, name, desc)
    {
    }
};

extern AcmeConfig gAcmeConfig;

// Revert ACME NV/config when running on an unsupported platform image.
void AcmeConfigSanitizeOnBoot();
// Disable ACME in config (user PEM upload, reset to auto-generated, etc.).
void AcmeConfigDisable();
// True when ACME is configured as the active certificate source on this platform.
bool AcmeConfigActive();

#endif
