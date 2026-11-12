#include "stream/mqtt_publisher.hpp"

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }
    rksec::config::Mqtt config;
    config.device_id = "probe";
    config.host = argv[1];
    config.port = std::stoi(argv[2]);
    config.credentials = argv[3];
    config.ca_file = argv[4];
    rksec::stream::MqttPublisher publisher(config);
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "burst") {
            for (int i = 0; i < 140; ++i) {
                publisher.passage("synthetic-" + std::to_string(i), 1700000000000 + i);
            }
            std::cout << publisher.dropped() << std::endl;
        } else if (command == "active") {
            publisher.pir(rksec::stream::PirStatus::active);
        } else if (command == "idle") {
            publisher.pir(rksec::stream::PirStatus::idle);
        } else if (command == "stop") {
            break;
        }
    }
}
