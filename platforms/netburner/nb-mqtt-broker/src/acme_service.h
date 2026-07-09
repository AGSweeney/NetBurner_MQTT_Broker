#ifndef ACME_SERVICE_H
#define ACME_SERVICE_H

// Let's Encrypt ACME integration (SOMRT1061). Stub implementations on other platforms.

#include <basictypes.h>

void AcmeServiceInit();
bool AcmeServiceIsEnabled();
bool AcmeServiceCertReady();
bool AcmeServiceHasFailed();
const char *AcmeServiceGetStateString();
void AcmeServiceRestart();
void AcmeMonitorTask(void *pd);
void SslServiceOnAcmeCertReady();

#endif
