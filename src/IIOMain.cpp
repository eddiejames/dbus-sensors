#include <IIOSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

static constexpr float pollRateDefault = 0.5;

namespace fs = std::filesystem;
static constexpr std::array<const char*, 13> sensorTypes = {
    "xyz.openbmc_project.Configuration.DPS310",
    "xyz.openbmc_project.Configuration.SI7020"};

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<IIOSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection,
        std::move([&io, &objectServer, &sensors, &dbusConnection,
                   sensorsChanged](
                      const ManagedObjectType& sensorConfigurations) {
            bool firstScan = sensorsChanged == nullptr;

            std::vector<fs::path> paths;
            fs::path root("/sys/bus/iio/devices");
            findFiles(root, R"(in_temp\d*_(input|raw))", paths);
            findFiles(root, R"(in_pressure\d*_(input|raw))", paths);
            findFiles(root, R"(in_humidity\d*_(input|raw))", paths);

            if (paths.empty())
            {
                std::cerr << "No IIO sensors in system\n";
                return;
            }

            // iterate through all found sensors, and try to match them
            // with configuration
            for (auto& path : paths)
            {
                std::smatch match;
                const std::string& pathStr = path.string();
                auto directory = path.parent_path();

                // directory is something like /sys/bus/iio/devices/iio:device0
                // which is a symlink to something like
                // /sys/devices/<platform i2c>/7-0076/iio:device0
                fs::path device = fs::canonical(directory);
                std::string deviceName = device.parent_path().stem();
                auto findHyphen = deviceName.find('-');
                if (findHyphen == std::string::npos)
                {
                    std::cerr << "found bad device " << deviceName << "\n";
                    continue;
                }
                std::string busStr = deviceName.substr(0, findHyphen);
                std::string addrStr = deviceName.substr(findHyphen + 1);

                size_t bus = 0;
                size_t addr = 0;
                try
                {
                    bus = std::stoi(busStr);
                    addr = std::stoi(addrStr, nullptr, 16);
                }
                catch (std::invalid_argument&)
                {
                    continue;
                }
                const SensorData* sensorData = nullptr;
                const std::string* interfacePath = nullptr;
                const char* sensorType = nullptr;
                const SensorBaseConfiguration* baseConfiguration = nullptr;
                const SensorBaseConfigMap* baseConfigMap = nullptr;
                std::string sensorTypeName = "temperature";

                if (pathStr.find("pressure") != std::string::npos)
                    sensorTypeName = "pressure";
                else if (pathStr.find("humidity") != std::string::npos)
                    sensorTypeName = "humidity";

                for (const std::pair<sdbusplus::message::object_path,
                                     SensorData>& sensor : sensorConfigurations)
                {
                    sensorData = &(sensor.second);
                    for (const char* type : sensorTypes)
                    {
                        auto sensorBase = sensorData->find(type);
                        if (sensorBase != sensorData->end())
                        {
                            baseConfiguration = &(*sensorBase);
                            sensorType = type;
                            break;
                        }
                    }
                    if (baseConfiguration == nullptr)
                    {
                        std::cerr << "error finding base configuration for "
                                  << deviceName << "\n";
                        continue;
                    }
                    baseConfigMap = &baseConfiguration->second;
                    auto configurationBus = baseConfigMap->find("Bus");
                    auto configurationAddress = baseConfigMap->find("Address");

                    if (configurationBus == baseConfigMap->end() ||
                        configurationAddress == baseConfigMap->end())
                    {
                        std::cerr << "error finding bus or address in "
                                     "configuration\n";
                        continue;
                    }

                    if (std::get<uint64_t>(configurationBus->second) != bus ||
                        std::get<uint64_t>(configurationAddress->second) !=
                            addr)
                    {
                        continue;
                    }

                    interfacePath = &(sensor.first.str);
                    break;
                }
                if (interfacePath == nullptr)
                {
                    std::cerr << "failed to find match for " << deviceName
                              << "\n";
                    continue;
                }

                auto findType = baseConfigMap->find("SensorType");
                if (findType == baseConfigMap->end())
                {
                    std::cerr << "failed to find the sensor type for " << deviceName << "\n";
                    continue;
                }

                std::string configSensorTypeName = std::visit(VariantToStringVisitor(), findType->second);
                if (sensorTypeName.compare(configSensorTypeName))
                {
                    std::cerr << "config sensor type " << configSensorTypeName << " doesn't match sensor type " << sensorTypeName << "\n";
                }

                auto findSensorName = baseConfigMap->find("Name");
                if (findSensorName == baseConfigMap->end())
                {
                    std::cerr << "could not determine configuration name for "
                              << deviceName << "\n";
                    continue;
                }
                std::string sensorName =
                    std::get<std::string>(findSensorName->second);
                // on rescans, only update sensors we were signaled by
                auto findSensor = sensors.find(sensorName);
                if (!firstScan && findSensor != sensors.end())
                {
                    bool found = false;
                    for (auto it = sensorsChanged->begin();
                         it != sensorsChanged->end(); it++)
                    {
                        if (boost::ends_with(*it, findSensor->second->name))
                        {
                            sensorsChanged->erase(it);
                            findSensor->second = nullptr;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        continue;
                    }
                }
                std::vector<thresholds::Threshold> sensorThresholds;
                if (!parseThresholdsFromConfig(*sensorData, sensorThresholds))
                {
                    std::cerr << "error populating thresholds for "
                              << sensorName << "\n";
                }

                auto findPollRate = baseConfiguration->second.find("PollRate");
                float pollRate = pollRateDefault;
                if (findPollRate != baseConfiguration->second.end())
                {
                    pollRate = std::visit(VariantToFloatVisitor(),
                                          findPollRate->second);
                    if (pollRate <= 0.0f)
                    {
                        pollRate = pollRateDefault; // polling time too short
                    }
                }

                auto findPowerOn = baseConfiguration->second.find("PowerState");
                PowerState readState = PowerState::always;
                if (findPowerOn != baseConfiguration->second.end())
                {
                    std::string powerState = std::visit(
                        VariantToStringVisitor(), findPowerOn->second);
                    setReadState(powerState, readState);
                }

                auto permitSet = getPermitSet(*baseConfigMap);
                auto& sensor = sensors[sensorName];
                sensor = std::make_shared<IIOSensor>(
                        pathStr, sensorType, objectServer, dbusConnection,
                        io, sensorName, std::move(sensorThresholds), pollRate,
                        *interfacePath, readState, sensorTypeName);
                sensor->setupRead();
            }
        }));
    getter->getConfiguration(
        std::vector<std::string>(sensorTypes.begin(), sensorTypes.end()));
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.IIOSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::shared_ptr<IIOSensor>>
        sensors;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    io.post([&]() {
        createSensors(io, objectServer, sensors, systemBus, nullptr);
    });

    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            // this implicitly cancels the timer
            filterTimer.expires_from_now(boost::posix_time::seconds(1));

            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    /* we were canceled*/
                    return;
                }
                if (ec)
                {
                    std::cerr << "timer error\n";
                    return;
                }
                createSensors(io, objectServer, sensors, systemBus,
                              sensorsChanged);
            });
        };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    io.run();
}