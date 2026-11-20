#pragma once

#if !defined(PORTAL_HOST) && defined(PORTAL_SERVICE_DAEMON)
void serviceLog(const char *format, ...);
void serviceLogReopen();
void serviceLogInstallCrashHandlers();
#define SERVICE_LOG(...) serviceLog(__VA_ARGS__)
#else
#define SERVICE_LOG(...) ((void)0)
#endif
