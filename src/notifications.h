#pragma once

#include <string>

// JSON command entry point for the manager broker. The store is process-local
// and thread-safe; callers receive compact UTF-8 JSON.
std::string notificationCommand(const std::string &command, const char *request);

// Optional native API getter. Register this symbol in the manager's XOVI
// metadata with XEM_NOTIFICATIONS_METADATA = 1.
extern "C" const struct XemNotificationsApiV1 *xem_notifications_get_api_v1(void);
extern "C" const struct XemNotificationsApiV2 *xem_notifications_get_api_v2(void);
