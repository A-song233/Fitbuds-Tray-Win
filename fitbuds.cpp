#include "fitbuds_protocol.hpp"

using namespace fitbuds;

int main(int argc, char** argv) {
    const bool dry_run = argc == 3 && std::string_view(argv[1]) == "--dry-run";
    const std::string_view name = argc == 2 || dry_run ? argv[argc - 1] : "";
    if (name.empty() || name == "--help") {
        std::cout << "fitbuds [--dry-run] <command>\n";
        for (const auto& command : commands) std::cout << "  " << command.name << '\n';
        return name.empty() ? 2 : 0;
    }
    const auto* selected = std::find_if(std::begin(commands), std::end(commands),
                                        [&](const Command& command) { return command.name == name; });
    if (selected == std::end(commands)) {
        std::cerr << "Unknown command: " << name << '\n';
        return 2;
    }

    try {
        std::cout << "TX";
        print_hex(make_request(selected->opcode, selected->payload));
        if (dry_run) {
            if (selected->readback == 0xD5) {
                std::cout << "Readback TX";
                print_hex(make_request(0xD5, {}));
            }
            return 0;
        }
        const auto address = configured_address();
        if (!address) throw std::runtime_error("Set [bluetooth] address in FitBudsTray.ini next to the executable");
        Winsock winsock;
        Rfcomm connection;
        connection.connect_device(*address);
        auto battery = read_battery(connection);
        if (name == "battery") {
            std::cout << "Decoded F2 payload:";
            print_hex(battery.payload);
            auto format = [](std::optional<int> level) {
                return level ? std::to_string(*level) + "%" : std::string("unavailable");
            };
            std::cout << "Left: " << format(battery.left) << " | Right: " << format(battery.right)
                      << " | Case: " << format(battery.case_level) << '\n';
            return 0;
        }
        if (!allowed(*selected, battery.present)) throw std::runtime_error("Unavailable in the current earbud mode");
        apply_mode(connection, *selected);
        std::cout << (name == "power-off" ? "RFCOMM disconnected after power-off.\n" : "Setting confirmed.\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
