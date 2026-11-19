#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fitbuds {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline bool is_fitbuds_name(std::wstring_view name) {
    constexpr std::wstring_view match = L"FitBuds";
    for (std::size_t i = 0; i + match.size() <= name.size(); ++i) {
        const int length = static_cast<int>(match.size());
        if (CompareStringOrdinal(name.data() + i, length, match.data(), length, TRUE) == CSTR_EQUAL)
            return true;
    }
    return false;
}

inline std::optional<BTH_ADDR> find_paired_fitbuds() {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof(search);
    search.fReturnAuthenticated = TRUE;
    // Use the paired Classic Bluetooth device list. Do not start a radio inquiry.
    BLUETOOTH_DEVICE_INFO device{};
    device.dwSize = sizeof(device);
    const auto find = BluetoothFindFirstDevice(&search, &device);
    if (!find) return std::nullopt;

    std::optional<BTH_ADDR> match;
    do {
        const auto end = std::find(std::begin(device.szName), std::end(device.szName), L'\0');
        const std::wstring_view name(device.szName, end - device.szName);
        if (!device.fAuthenticated || !device.Address.ullLong || !is_fitbuds_name(name)) continue;
        if (!match || device.fConnected) match = device.Address.ullLong;
        if (device.fConnected) break;
    } while (BluetoothFindNextDevice(find, &device));

    BluetoothFindDeviceClose(find);
    return match;
}

constexpr GUID service{0xEDF00000, 0xEDFE, 0xDFED, {0xFE, 0xDF, 0xED, 0xFE, 0xDF, 0xED, 0xFE, 0xDF}};
constexpr std::uint8_t mask = 0xA5;

struct Command {
    std::string_view name;
    std::uint8_t opcode;
    std::initializer_list<std::uint8_t> payload;
    std::uint8_t readback = 0;
};

inline const Command commands[] = {
    {"battery",      0xF2, {}},
    {"anc-deep",     0xC1, {0x16, 1}, 0xCC},
    {"anc-wide",     0xC1, {0x16, 2}, 0xCC},
    {"anc-wind",     0xC1, {0x16, 3}, 0xCC},
    {"ambient",      0xC1, {0x16, 4}, 0xCC},
    {"anc-off",      0xC1, {0x16, 5}, 0xCC},
    {"eq-classic",   0xC4, {0}, 0xD5},
    {"eq-classical", 0xC4, {2}, 0xD5},
    {"eq-rock",     0xC4, {3}, 0xD5},
    {"eq-pop",      0xC4, {1}, 0xD5},
    {"game-on",     0x09, {1}, 0x08},
    {"game-off",    0x09, {0}, 0x08},
    {"power-off",   0xCE, {}},
};

inline std::uint8_t checksum(const std::uint8_t* bytes, std::size_t count) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < count; ++i) sum += bytes[i];
    return static_cast<std::uint8_t>(sum);
}

inline Bytes make_request(std::uint8_t opcode, std::initializer_list<std::uint8_t> payload) {
    Bytes bytes{0xAA, 0xEC, opcode, 0, static_cast<std::uint8_t>(payload.size())};
    for (auto byte : payload) bytes.push_back(byte ^ mask);
    bytes.push_back(checksum(bytes.data(), bytes.size()));
    return bytes;
}

inline void print_hex(const Bytes& bytes) {
    for (auto byte : bytes) {
        std::cout << ' ' << std::hex << std::uppercase << std::setw(2)
                  << std::setfill('0') << static_cast<int>(byte);
    }
    std::cout << std::dec << '\n';
}

class RfcommError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline RfcommError winsock_error(const char* operation, int code = WSAGetLastError()) {
    return RfcommError(std::string(operation) + " failed (Winsock " + std::to_string(code) + ")");
}

class Winsock {
public:
    Winsock() {
        WSADATA info{};
        if (int code = WSAStartup(MAKEWORD(2, 2), &info)) throw winsock_error("WSAStartup", code);
    }
    ~Winsock() { WSACleanup(); }
};

class Rfcomm {
public:
    explicit Rfcomm(const std::atomic_bool* stopping = nullptr)
        : socket_(socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)), stopping_(stopping) {
        if (socket_ == INVALID_SOCKET) throw winsock_error("RFCOMM socket");
    }
    ~Rfcomm() { closesocket(socket_); }
    Rfcomm(const Rfcomm&) = delete;
    Rfcomm& operator=(const Rfcomm&) = delete;

    void connect_device(BTH_ADDR address) {
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) throw winsock_error("FIONBIO");

        SOCKADDR_BTH peer{};
        peer.addressFamily = AF_BTH;
        peer.btAddr = address;
        peer.serviceClassId = service;
        if (connect(socket_, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) == 0) return;
        int code = WSAGetLastError();
        if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) throw winsock_error("RFCOMM connect", code);
        if (!ready(true, Clock::now() + 10s)) throw RfcommError("RFCOMM connect timed out");

        int size = sizeof(code);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &size) != 0)
            throw winsock_error("SO_ERROR");
        if (code) throw winsock_error("RFCOMM connect", code);
    }

    void check_connected() const {
        char byte;
        const int received = recv(socket_, &byte, 1, MSG_PEEK);
        if (received == 0) throw RfcommError("RFCOMM disconnected");
        if (received == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code != WSAEWOULDBLOCK) throw winsock_error("RFCOMM receive", code);
        }
    }

    void write(const Bytes& bytes) {
        const auto deadline = Clock::now() + 3s;
        std::size_t sent = 0;
        while (sent < bytes.size() && ready(true, deadline)) {
            const auto* start = reinterpret_cast<const char*>(bytes.data() + sent);
            int length = send(socket_, start, static_cast<int>(bytes.size() - sent), 0);
            if (length == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (length == SOCKET_ERROR) throw winsock_error("RFCOMM send");
            if (length == 0) break;
            sent += length;
        }
        if (sent != bytes.size()) throw RfcommError("RFCOMM send did not complete");
    }

    bool read(Clock::time_point deadline) {
        while (ready(false, deadline)) {
            std::uint8_t buffer[2048];
            int length = recv(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (length > 0) {
                incoming_.insert(incoming_.end(), buffer, buffer + length);
                return true;
            }
            int code = length == 0 ? 0 : WSAGetLastError();
            if (code == WSAEWOULDBLOCK) continue;
            if (code && code != WSAECONNRESET && code != WSAECONNABORTED && code != WSAENETRESET)
                throw winsock_error("RFCOMM receive", code);
            disconnected_ = true;
            return false;
        }
        return false;
    }

    template<class Predicate>
    std::optional<Bytes> response_matching(Predicate matches, Clock::time_point deadline) {
        do {
            while (auto packet = take_frame()) {
                if (matches(*packet)) return packet;
            }
        } while (read(deadline));
        if (disconnected_) throw RfcommError("RFCOMM disconnected before reply");
        return std::nullopt;
    }

    std::optional<Bytes> response(std::uint8_t opcode, Clock::time_point deadline) {
        return response_matching([opcode](const Bytes& packet) { return packet[2] == opcode; }, deadline);
    }

    std::optional<Bytes> query(std::uint8_t opcode, int attempts = 3,
                               std::chrono::milliseconds timeout = 650ms) {
        for (int attempt = 0; attempt < attempts; ++attempt) {
            write(make_request(opcode, {}));
            if (auto reply = response(opcode, Clock::now() + timeout)) return reply;
        }
        return std::nullopt;
    }

    void wait_for_power_off() {
        const auto deadline = Clock::now() + 5s;
        while (read(deadline)) incoming_.clear();
        if (!disconnected_) throw std::runtime_error("Power-off sent, but RFCOMM disconnect was not observed");
    }

private:
    SOCKET socket_;
    const std::atomic_bool* stopping_;
    Bytes incoming_;
    bool disconnected_ = false;

    bool ready(bool writing, Clock::time_point deadline) const {
        for (;;) {
            if (stopping_ && stopping_->load()) throw RfcommError("RFCOMM operation cancelled");
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
            if (remaining <= 0us) return false;
            if (stopping_) remaining = (std::min)(remaining, 100000us);
            const auto micros = remaining.count();
            timeval timeout{static_cast<long>(micros / 1000000), static_cast<long>(micros % 1000000)};
            fd_set activity{}, errors{};
            FD_SET(socket_, &activity);
            FD_SET(socket_, &errors);
            int status = select(0, writing ? nullptr : &activity, writing ? &activity : nullptr, &errors, &timeout);
            if (status == SOCKET_ERROR) throw winsock_error("RFCOMM select");
            if (status > 0 || !stopping_) return status > 0;
        }
    }

    std::optional<Bytes> take_frame() {
        constexpr std::uint8_t prefix[]{0xBB, 0xEC};
        while (incoming_.size() >= 2) {
            auto start = std::search(incoming_.begin(), incoming_.end(), std::begin(prefix), std::end(prefix));
            if (start == incoming_.end() && incoming_.back() == prefix[0]) --start;
            incoming_.erase(incoming_.begin(), start);
            if (incoming_.size() < 6) break;
            const auto size = 6 + (std::size_t(incoming_[3]) << 8) + incoming_[4];
            if (size > 1024) {
                incoming_.erase(incoming_.begin());
                continue;
            }
            if (incoming_.size() < size) break;
            if (checksum(incoming_.data(), size - 1) == incoming_[size - 1]) {
                Bytes packet(incoming_.begin(), incoming_.begin() + size);
                incoming_.erase(incoming_.begin(), incoming_.begin() + size);
                return packet;
            }
            incoming_.erase(incoming_.begin());
        }
        return std::nullopt;
    }
};


struct Battery {
    int present = 0;
    std::optional<int> left;
    std::optional<int> right;
    std::optional<int> case_level;
    Bytes payload;
};

inline std::optional<int> reported_level(std::uint8_t value) {
    if (value <= 100) return value;
    return std::nullopt;
}

inline Battery decode_battery(const Bytes& reply) {
    if (reply.size() != 12) throw std::runtime_error("Unexpected battery response length");
    Battery battery;
    for (auto it = reply.begin() + 5; it != reply.end() - 1; ++it) battery.payload.push_back(*it ^ mask);
    const auto& payload = battery.payload;
    battery.present = payload[0] & 3;
    if (battery.present & 1) battery.left = reported_level(payload[1]);
    if (battery.present & 2) battery.right = reported_level(payload[2]);
    // The capture contains a nonzero, readable case level and a zero placeholder.
    if (payload[3] != 0) battery.case_level = reported_level(payload[3]);
    return battery;
}

inline bool allowed(const Command& command, int present) {
    if (!present) return false;
    if (command.opcode == 0xC1) return present == 3 || *(command.payload.begin() + 1) >= 4;
    return true;
}

inline Battery read_battery(Rfcomm& connection) {
    // The first request after opening a Windows RFCOMM socket can be missed.
    connection.query(0xD8, 1);
    auto reply = connection.query(0xF2);
    if (!reply) throw std::runtime_error("Battery reply timed out");
    return decode_battery(*reply);
}

template<class Connection>
int read_mode(Connection& connection, std::uint8_t opcode, int attempts = 2,
              std::chrono::milliseconds timeout = 650ms) {
    auto reply = connection.query(opcode, attempts, timeout);
    if (!reply) return -1;
    if (opcode == 0xCC) {
        if (reply->size() < 8 || ((*reply)[5] ^ mask) != 0x16) return -1;
        const int value = (*reply)[6] ^ mask;
        return value >= 1 && value <= 5 ? value : -1;
    }
    if (reply->size() != 7) return -1;
    const int value = (*reply)[5] ^ mask;
    return value <= (opcode == 0x08 ? 1 : 3) ? value : -1;
}

inline void apply_mode(Rfcomm& connection, const Command& command) {
    auto request = make_request(command.opcode, command.payload);
    if (command.opcode == 0xCE) {
        connection.write(request);
        connection.wait_for_power_off();
        return;
    }
    const int target = *(command.payload.end() - 1);
    if (read_mode(connection, command.readback, 1) == target) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        Sleep(120);
        connection.write(request);
        auto reply = connection.response(command.opcode, Clock::now() + 350ms);
        if (command.opcode != 0xC4 && reply && reply->size() >= request.size() &&
            std::equal(request.begin() + 5, request.end() - 1, reply->begin() + 5)) return;
        for (int verify = 0; verify < 2; ++verify) {
            Sleep(100);
            if (read_mode(connection, command.readback, 1) == target) return;
        }
    }
    throw std::runtime_error("Earbuds did not confirm the requested setting");
}

// One transmission per attempt. The worker spaces attempts to accommodate the earbuds.
inline bool apply_mode_fast(Rfcomm& connection, const Command& command) {
    const auto request = make_request(command.opcode, command.payload);
    connection.write(request);
    if (command.opcode == 0xCE) {
        connection.wait_for_power_off();
        return true;
    }

    const bool eq = command.opcode == 0xC4;
    const auto acknowledgement = connection.response_matching([&](const Bytes& packet) {
        if (packet[2] != command.opcode) return false;
        if (eq) return packet.size() == 7;
        return packet.size() >= request.size() &&
            std::equal(request.begin() + 5, request.end() - 1, packet.begin() + 5);
    }, Clock::now() + 250ms);
    if (!eq) return acknowledgement.has_value();

    // Every preset produces the same C4 reply. Read D5 to confirm the actual preset.
    if (acknowledgement) Sleep(35);
    return read_mode(connection, 0xD5, 1, 250ms) == *(command.payload.end() - 1);
}

} // namespace fitbuds
