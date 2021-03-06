#pragma once

#include <libudev.h>

#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


struct UdevConnection {
    UdevConnection()
    {
        context = udev_new();

        if (!context) {
            fprintf(stderr, "Failed to connect to udev\n");
            return;
        }

        udevMonitor = udev_monitor_new_from_netlink(context, "udev");

        if (!udevMonitor) {
            fprintf(stderr, "Failed to create udev monitor\n");
            return;
        }

        udev_monitor_filter_add_match_subsystem_devtype(udevMonitor, "power_supply", 0);
        udev_monitor_enable_receiving(udevMonitor);
        udevSocketFd = udev_monitor_get_fd(udevMonitor);
        udevAvailable = true;

        init();
    }

    void init()
    {
        udev_enumerate *enumerate = udev_enumerate_new(context);

        udev_enumerate_add_match_subsystem(enumerate, "power_supply");
        udev_enumerate_add_match_subsystem(enumerate, "net");
        udev_enumerate_scan_devices(enumerate);

        udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
        udev_list_entry *entry = nullptr;

        udev_list_entry_foreach(entry, devices) {
            const char *path = udev_list_entry_get_name(entry);

            if (!path) {
                fprintf(stderr, "Invalid device when listing\n");
                continue;
            }

            udev_device *dev = udev_device_new_from_syspath(context, path);

            if (!dev) {
                fprintf(stderr, "failed getting %s\n", path);
                continue;
            }

            const char *subsystem = udev_device_get_subsystem(dev);

            if (strcmp(subsystem, "net") == 0) {
                const char *interface = udev_device_get_property_value(dev, "INTERFACE");
                const char *devtype = udev_device_get_devtype(dev);

                if (!interface) {
                    fprintf(stderr, "missing interface property!\n");
                    printProperties(dev);
                    udev_device_unref(dev);
                    continue;
                }

                if (strcmp(interface, "lo") == 0) {
                    udev_device_unref(dev);
                    continue;
                }

                if (devtype && strcmp(devtype, "wlan") == 0) {
                    wlanInterfaces.push_back(interface);
                } else {
                    ethernetInterfaces.push_back(interface);
                }

                udev_device_unref(dev);
                continue;
            }

            const char *deviceName = udev_device_get_sysname(dev);

            if (strcmp(deviceName, "BAT0") == 0) {
                power.batteryDevice = dev; // so we get updates when waking from sleep
            } else if (strcmp(deviceName, "AC") == 0) {
                power.chargerDevice = dev;
            } else {
                fprintf(stderr, "Unknown power supply device %s\n", deviceName);
                printProperties(dev);
                udev_device_unref(dev);
            }

        }

        udev_enumerate_unref(enumerate);

        power.valid = false;

        if (!power.chargerDevice) {
            fprintf(stderr, "Failed to find charger device\n");
            return;
        }

        if (!updateCharger()) {
            fprintf(stderr, "Failed to update charger\n");
            return;
        }

        power.valid = true;
    }

    ~UdevConnection()
    {
        if (power.chargerDevice) {
            udev_device_unref(power.chargerDevice);
        }

        if (power.batteryDevice) {
            udev_device_unref(power.batteryDevice);
        }

        if (udevMonitor) {
            udev_monitor_unref(udevMonitor);
        }

        if (context) {
            udev_unref(context);
        }
    }

    void printProperties(udev_device *dev)
    {
        fprintf(stderr, "action: %s\n", udev_device_get_action(dev));
        fprintf(stderr, "sysname: %s\n", udev_device_get_sysname(dev));
        udev_list_entry *entry = udev_device_get_properties_list_entry(dev);

        while (entry) {
            const char *name = udev_list_entry_get_name(entry);
            const char *value = udev_list_entry_get_value(entry);
            fprintf(stderr, "property name: %s value %s\n", name, value);
            entry = udev_list_entry_get_next(entry);
        }
    }

    bool update(const bool gotEvent)
    {
        if (!udevAvailable) {
            fprintf(stderr, "udev unavailable\n");
            return false;
        }

        if (gotEvent) {
            udev_device *dev = udev_monitor_receive_device(udevMonitor);

            const char *deviceName = udev_device_get_sysname(dev);

            if (strcmp(deviceName, "BAT0") == 0) {
                if (dev != power.batteryDevice) {
                    if (power.batteryDevice) {
                        udev_device_unref(power.batteryDevice);
                    }

                    power.batteryDevice = dev;
                }
            } else if (strcmp(deviceName, "AC") == 0) {
                if (dev != power.chargerDevice) {
                    if (power.chargerDevice) {
                        udev_device_unref(power.chargerDevice);
                    }

                    power.chargerDevice = dev;
                }
            } else {
                fprintf(stderr, "Unknown power supply device notification %s\n", deviceName);
            }
        }

        power.valid = updateCharger();

        if (!power.valid) {
            power.chargerOnline = false;
        }

        return true;
    }

    bool updateCharger()
    {
        if (!power.chargerDevice) {
            //fprintf(stderr, "charger not available?\n");
            return false;
        }

        const char *online = udev_device_get_property_value(power.chargerDevice, "POWER_SUPPLY_ONLINE");

        if (strcmp(online, "1") == 0) {
            power.chargerOnline = true;
        } else if (strcmp(online, "0") == 0) {
            power.chargerOnline = false;
        } else {
            fprintf(stderr, "unknown charger online %s\n", online);
        }

        return true;
    }

    udev *context = nullptr;
    udev_monitor *udevMonitor = nullptr;

    bool udevAvailable = false;

    int udevSocketFd = -1;

    struct PowerStatus {
        udev_device *chargerDevice = nullptr;
        udev_device *batteryDevice = nullptr; // don't use this or trust this, udev is shit

        bool chargerOnline = false;
        int last_percentage = 100;
        bool valid = false;
    } power;

    std::vector<std::string> wlanInterfaces;
    std::vector<std::string> ethernetInterfaces;
};

