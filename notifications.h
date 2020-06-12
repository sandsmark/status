#pragma once
#include "json_helpers.h"

#include <regex>

#include <systemd/sd-bus.h>

struct Notification {
    std::string app, message;
    int timeout = 0;
};

static void print_notification(Notification *notification)
{
    std::string message;
    if (!notification->app.empty()) {
        message += notification->app + ": ";
    }
    message += notification->message;
    if (message.size() > 50) {
        message.resize(50);
        message += "...";
    }

    fputs(message.c_str(), stdout);

    if (notification->timeout % 2 == 0) {
        print_black();
        print_white_background();
    }
}

static std::vector<Notification> g_notifications;

static int method_notify(sd_bus_message *m, void * /*userdata*/, sd_bus_error *error)
{
  //printf("Got called with userdata=%p\n", userdata);

  const char *app_name = nullptr;
  uint32_t replaces_id = 0u;
  const char *app_icon = nullptr;
  const char *summary = nullptr;
  const char *body = nullptr;

  int timeout = 0;

  int ret = sd_bus_message_read(m, "susss", &app_name, &replaces_id, &app_icon, &summary, &body);
  if (ret < 0) {
      fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-ret));
      return ret;
  }

  // Can't be bothered to parse these, which aren't used
  ret = sd_bus_message_skip(m, "asa{sv}");
  if (ret < 0) {
      fprintf(stderr, "Failed to skip parameters: %s\n", strerror(-ret));
  }

  ret = sd_bus_message_read(m, "i", &timeout);
  if (ret < 0) {
      fprintf(stderr, "Failed to parse timeout: %s\n", strerror(-ret));
      return ret;
  }

  /* Return an error on division by zero */
  if (false) {
      sd_bus_error_set_const(error, "net.poettering.DivisionByZero", "Sorry, can't allow division by zero.");
      return -EINVAL;
  }
  if (!g_notifications.empty()) {
      if (g_notifications.size() > 5) { // don't have too many..
          g_notifications.resize(5);
      }

      // Clean out old ones
      for (Notification &notification : g_notifications) {
          notification.timeout = 1;
      }
  }

  std::regex stripRegex("[^a-zA-Z0-9.,#_\\- ]");

  Notification notification;
  notification.app = std::regex_replace(app_name, stripRegex, "");
  notification.message = std::regex_replace(summary, stripRegex, "");

  if (notification.message.empty()) {
      notification.message = body;
  }

  notification.timeout = std::max(timeout, 10000) / 1000;
  g_notifications.push_back(std::move(notification));

  static int id = 0;
  fprintf(stderr, "%s %u %s %s %s %d\n", app_name, replaces_id, app_icon, summary, body, timeout);

  return sd_bus_reply_method_return(m, "u", id++);
}

static int method_getcapabilities(sd_bus_message *m, void * /*userdata*/, sd_bus_error *error)
{
    if (error) {
        fprintf(stderr, "Error from sd_bus: %s: %s\n", error->name, error->message);
    }
    return sd_bus_reply_method_return(m, "as", 5,
            "action-icons",
            "actions",
            "body",
            //"body-hyperlinks",
            //"body-images",
            //"body-markup",
            //"icon-multi",
            "persistence",
            "sound"
            );
}


static const sd_bus_vtable notifications_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD("Notify",
                "s"      // app_name
                "u"      // replaces_id
                "s"      // app_icon
                "s"      // summary
                "s"      // body
                "as"     // actions
                "a{sv}"  // hints
                "i",     // timeout
            "u",     // out
            method_notify,
        SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_METHOD("GetCapabilities", "", "as", method_getcapabilities, SD_BUS_VTABLE_UNPRIVILEGED),

    // Instead of using SD_BUS_VTABLE_END do it 'manually' to squash compiler
    // warnings
    { .type = _SD_BUS_VTABLE_END, .flags = 0, .x = { { 0, 0, 0 } } },
};

bool register_notification_service(sd_bus *bus, sd_bus_slot **slot)
{
    if (!bus) {
        fprintf(stderr, "No bus to register to\n");
        return false;
    }

    int ret = sd_bus_add_object_vtable(bus,
            slot,
            "/org/freedesktop/Notifications",  /* object path */
            "org.freedesktop.Notifications",   /* interface name */
            notifications_vtable,
            NULL);

    if (ret < 0) {
        fprintf(stderr, "Failed to register dbus vtable: %s\n", strerror(-ret));
        return false;
    }
    ret = sd_bus_request_name(bus, "org.freedesktop.Notifications", 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to request dbus name: %s\n", strerror(-ret));
        return false;
    }
    return true;
}

void process_bus(sd_bus *bus)
{
    int ret = sd_bus_process(bus, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to process bus: %s\n", strerror(-ret));
        return;
    }
}

