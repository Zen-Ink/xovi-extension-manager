#pragma once

#include <string>

// JSON command entry point for the manager broker. The store is process-local
// and thread-safe; callers receive compact UTF-8 JSON.
std::string notificationCommand(const std::string &command, const char *request);

extern "C" const struct XemNotificationsApi *xem_notifications_get_api(void);
