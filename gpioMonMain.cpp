// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "gpioMon.hpp"

#include <systemd/sd-daemon.h>

#include <CLI/CLI.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/property.hpp>

#include <fstream>

namespace phosphor
{
namespace gpio
{

std::map<std::string, int> polarityMap = {
    /**< Only watch falling edge events. */
    {"FALLING", GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE},
    /**< Only watch rising edge events. */
    {"RISING", GPIOD_LINE_REQUEST_EVENT_RISING_EDGE},
    /**< Monitor both types of events. */
    {"BOTH", GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES}};

}
} // namespace phosphor

constexpr auto gpioMonServ = "xyz.openbmc_project.GpioMonitor";
constexpr auto gpioMonPath = "/xyz/openbmc_project/GpioMonitor";
constexpr auto gpioMonIntf = "xyz.openbmc_project.gpio.monitor";
constexpr auto gpioPinPath_prefix = "/xyz/openbmc_project/Gpio/";

std::map<
    std::string,
    std::map<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>>>
    interfaces;

std::tuple<int, std::string> setGpioMaskInterval(std::string gpioName,
                                                 uint16_t interval)
{
    std::string objPath = gpioPinPath_prefix + gpioName;
    uint16_t maskInterval = interval;

    if (interval > MAX_GPIO_MASK)
    {
        std::cerr << "Invalid mask interval. [0 - 255]\n";
        maskInterval = KEEP_GPIO_MASK;
    }

    phosphor::gpio::gpioMaskMap[gpioName] = maskInterval;

    auto ifaceFind = interfaces.find(objPath);
    auto monitorIntfFind = ifaceFind->second.find(gpioMonIntf);
    if (monitorIntfFind != ifaceFind->second.end())
    {
        auto iface = monitorIntfFind->second;
        iface->set_property("Interval", maskInterval);
    }
    else
    {
        std::cerr << "Cannot find interface: " << gpioMonIntf << "\n";
        return std::make_tuple(-ENODATA, "The mask interval cannot be set.");
    }

    return std::make_tuple(0, "The mask interval is changed!");
}

int main(int argc, char** argv)
{
    boost::asio::io_context io;

    CLI::App app{"Monitor GPIO line for requested state change"};

    // Register a dbus service for set mask interval method
    auto bus = std::make_shared<sdbusplus::asio::connection>(
        io, sdbusplus::bus::new_system().release());
    bus->request_name(gpioMonServ);
    auto server = sdbusplus::asio::object_server(bus);

    std::shared_ptr<sdbusplus::asio::dbus_interface> interface =
        server.add_interface(gpioMonPath, gpioMonIntf);
    interface->register_method("setGpioMaskInterval", setGpioMaskInterval);
    interface->initialize();

    std::string gpioFileName;

    /* Add an input option */
    app.add_option("-c,--config", gpioFileName, "Name of config json file")
        ->required()
        ->check(CLI::ExistingFile);

    /* Parse input parameter */
    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::Error& e)
    {
        return app.exit(e);
    }

    /* Get list of gpio config details from json file */
    std::ifstream file(gpioFileName);
    if (!file)
    {
        lg2::error("GPIO monitor config file not found: {FILE}", "FILE",
                   gpioFileName);
        return -1;
    }

    nlohmann::json gpioMonObj;
    file >> gpioMonObj;
    file.close();

    std::vector<std::unique_ptr<phosphor::gpio::GpioMonitor>> gpios;

    for (auto& obj : gpioMonObj)
    {
        /* GPIO Line message */
        std::string lineMsg = "GPIO Line ";

        /* GPIO line */
        gpiod_line* line = nullptr;

        /* Mask interval to indicate whether contiue monitoring GPIO event or
         * not*/
        uint16_t maskInterval;
        std::string pinName = "";

        /* GPIO line configuration, default to monitor both edge */
        struct gpiod_line_request_config config{
            "gpio_monitor", GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES, 0};

        /* flag to monitor */
        bool flag = false;

        /* target to start */
        std::map<std::string, std::vector<std::string>> target;

        /* CallbackHook name */
        std::string hook;

        /* Name of gpio for reference */
        std::string gpioName;

        /* multi targets to start */
        std::map<std::string, std::vector<std::string>> targets;

        if (obj.find("LineName") == obj.end())
        {
            /* If there is no line Name defined then gpio num nd chip
             * id must be defined. GpioNum is integer mapping to the
             * GPIO key configured by the kernel
             */
            if (obj.find("GpioNum") == obj.end() ||
                obj.find("ChipId") == obj.end())
            {
                lg2::error("Failed to find line name or gpio number: {FILE}",
                           "FILE", gpioFileName);
                return -1;
            }

            std::string chipIdStr = obj["ChipId"];
            int gpioNum = obj["GpioNum"];

            lineMsg += std::to_string(gpioNum);

            /* Get the GPIO line */
            line = gpiod_line_get(chipIdStr.c_str(), gpioNum);
        }
        else
        {
            /* Find the GPIO line */
            std::string lineName = obj["LineName"];
            lineMsg += lineName;
            line = gpiod_line_find(lineName.c_str());
        }

        if (line == nullptr)
        {
            lg2::error("Failed to find the {GPIO}", "GPIO", lineMsg);
            continue;
        }

        /* Get event to be monitored, if it is not defined then
         * Both rising falling edge will be monitored.
         */
        if (obj.find("EventMon") != obj.end())
        {
            std::string eventStr = obj["EventMon"];
            auto findEvent = phosphor::gpio::polarityMap.find(eventStr);
            if (findEvent == phosphor::gpio::polarityMap.end())
            {
                lg2::error("{GPIO}: event missing: {EVENT}", "GPIO", lineMsg,
                           "EVENT", eventStr);
                return -1;
            }

            config.request_type = findEvent->second;
        }

        /* Get flag if monitoring needs to continue after first event */
        if (obj.find("Continue") != obj.end())
        {
            flag = obj["Continue"];
        }

        /* Parse out target argument. It is fine if the user does not
         * pass this if they are not interested in calling into any target
         * on meeting a condition.
         */
        if (obj.find("Target") != obj.end())
        {
            target = obj["Target"];
        }

        /*Parse callbackHook name.*/
        if (obj.find("CallbackFunction") != obj.end())
        {
            hook = obj["CallbackFunction"];
        }
        /*Get Name of gpio */
        if (obj.find("Name") != obj.end())
        {
            gpioName = obj["Name"];
        }

        /* Parse out the targets argument if multi-targets are needed.*/
        if (obj.find("Targets") != obj.end())
        {
            obj.at("Targets").get_to(targets);
        }

        if (obj.find("Name") != obj.end())
        {
            pinName = obj["Name"];
            std::string gpioObjPath = gpioPinPath_prefix + pinName;
            auto intf = server.add_interface(gpioObjPath, gpioMonIntf);
            interfaces[gpioObjPath][gpioMonIntf] = intf;
            if (obj.find("WaitInterval") != obj.end())
            {
                auto mask = obj["WaitInterval"];
                maskInterval = static_cast<uint16_t>(mask);
                phosphor::gpio::gpioMaskMap[pinName] = maskInterval;
                intf->register_property(
                    "Interval", maskInterval,
                    sdbusplus::asio::PropertyPermission::readWrite);
            }
            intf->initialize();
        }

        /* Create a monitor object and let it do all the rest */
        gpios.push_back(std::make_unique<phosphor::gpio::GpioMonitor>(
            line, config, io, target, targets, lineMsg, flag, pinName, hook,
            gpioName));
    }
    sd_notify(0, "READY=1");
    io.run();

    return 0;
}
