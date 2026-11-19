#include "fitbuds_protocol.hpp"
#include "resource.h"
#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using namespace fitbuds;

constexpr wchar_t class_name[] = L"FitBudsPro.ControlTray";
constexpr wchar_t app_name[] = L"FitBuds Pro";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT result_message = WM_APP + 2;
constexpr UINT tray_id = 1;
constexpr UINT refresh_id = 100;
constexpr UINT exit_id = 101;
constexpr UINT first_setting_id = 200;
constexpr UINT_PTR refresh_timer = 1;
// On this device, closely spaced writes can be ignored while battery and other reads still work.
constexpr auto setting_interval = 3200ms;
constexpr size_t setting_count = std::size(commands) - 1;

constexpr const wchar_t* setting_names[]{
    L"深度降噪", L"宽频降噪", L"防风噪", L"环境声", L"降噪关",
    L"经典", L"古典", L"摇滚", L"流行", L"开启", L"关闭", L"耳机关机"
};
static_assert(std::size(setting_names) == setting_count);

struct Snapshot {
    std::optional<Battery> battery;
    int noise = -1;
    int eq = -1;
    int game = -1;
    std::wstring status = L"正在等待电量";
};

std::wstring level_text(std::optional<int> level, bool present = true) {
    if (level) return std::to_wstring(*level) + L"%";
    return present ? L"未上报" : L"未连接";
}

const wchar_t* ear_mode(int present) {
    switch (present) {
    case 1: return L"仅左耳";
    case 2: return L"仅右耳";
    case 3: return L"双耳";
    default: return L"无在线耳机";
    }
}

class TrayApp {
public:
    ~TrayApp() {
        stop_worker();
        if (worker_.joinable()) worker_.join();
    }

    int run(HINSTANCE instance) {

        icon_ = LoadIconW(instance, MAKEINTRESOURCEW(IDI_FITBUDS));
        if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = instance;
        window_class.lpfnWndProc = window_proc;
        window_class.hIcon = icon_;
        window_class.lpszClassName = class_name;
        if (!RegisterClassExW(&window_class)) return 1;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, app_name, WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!window_) return 1;

        if (!add_icon()) {
            MessageBoxW(nullptr, L"无法添加 FitBuds Pro 托盘图标。", app_name, MB_OK | MB_ICONERROR);
            DestroyWindow(window_);
            return 1;
        }
        SetTimer(window_, refresh_timer, 45000, nullptr);
        try {
            worker_ = std::thread([this] { run_worker(); });
            request_refresh(true);
        } catch (const std::system_error&) {
            snapshot_.status = L"无法启动蓝牙线程";
            update_tooltip();
        }
        MSG message{};
        int status = 0;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return status < 0 ? 1 : static_cast<int>(message.wParam);
    }

private:
    HWND window_{};
    HICON icon_{};
    UINT taskbar_created_{};
    bool icon_added_ = false;
    bool version_four_ = false;
    bool menu_open_ = false;
    Snapshot snapshot_;
    std::thread worker_;
    std::atomic_bool stopping_{false};
    std::mutex queue_mutex_;
    std::condition_variable task_ready_;
    std::deque<const Command*> commands_pending_;
    std::atomic_size_t settings_in_progress_{0};
    bool full_refresh_requested_ = false;
    bool battery_refresh_requested_ = false;
    std::mutex result_mutex_;
    Snapshot result_;

    NOTIFYICONDATAW notification() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = tray_id;
        return data;
    }

    void set_tooltip(NOTIFYICONDATAW& data) const {
        std::wstring text = app_name;
        if (const auto& battery = snapshot_.battery) {
            text += L" · ";
            text += ear_mode(battery->present);
            text += L"\n左 " + level_text(battery->left, battery->present & 1);
            text += L"   右 " + level_text(battery->right, battery->present & 2);
            text += L"   盒 " + level_text(battery->case_level);
        } else {
            text += L"\n" + snapshot_.status;
        }
        lstrcpynW(data.szTip, text.c_str(), static_cast<int>(std::size(data.szTip)));
    }

    bool add_icon() {
        auto data = notification();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = icon_;
        set_tooltip(data);
        icon_added_ = Shell_NotifyIconW(NIM_ADD, &data);
        if (!icon_added_) return false;
        data.uVersion = NOTIFYICON_VERSION_4;
        version_four_ = Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    void update_tooltip() const {
        auto data = notification();
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        set_tooltip(data);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void stop_worker() {
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
        }
        task_ready_.notify_one();
    }

    void request_setting(const Command& command) {
        {
            std::lock_guard lock(queue_mutex_);
            commands_pending_.push_back(&command);
            ++settings_in_progress_;
        }
        task_ready_.notify_one();
    }

    void request_refresh(bool full) {
        {
            std::lock_guard lock(queue_mutex_);
            if (full) full_refresh_requested_ = true;
            else if (commands_pending_.empty()) battery_refresh_requested_ = true;
        }
        task_ready_.notify_one();
    }

    void publish(const Snapshot& snapshot) {
        {
            std::lock_guard lock(result_mutex_);
            result_ = snapshot;
        }
        if (!stopping_) PostMessageW(window_, result_message, 0, 0);
    }

    void run_worker() {
        Snapshot current;
        try {
            Winsock winsock;
            process_tasks(current);
        } catch (const RfcommError&) {
            current.status = L"无法初始化 Windows 蓝牙连接";
            publish(current);
        }
    }

    void process_tasks(Snapshot& current) {
        constexpr std::uint8_t refresh_opcodes[]{0xF2, 0xD5, 0x08, 0xCC};
        constexpr size_t refresh_done = std::size(refresh_opcodes);
        std::optional<Rfcomm> connection;
        size_t refresh_step = refresh_done;
        bool recovering_from_power_off = false;
        auto last_battery_query = Clock::time_point::min();
        auto next_setting_at = Clock::time_point::min();
        auto noise_read_at = Clock::time_point::min();
        auto battery_retry_at = Clock::time_point::min();
        int battery_attempts_left = 0;

        auto wait_for_setting_slot = [&] {
            if (Clock::now() >= next_setting_at) return !stopping_.load();
            current.status = L"等待耳机接收下一条设置…";
            publish(current);
            std::unique_lock lock(queue_mutex_);
            return !task_ready_.wait_until(lock, next_setting_at, [&] { return stopping_.load(); });
        };

        auto send_setting = [&](const Command& command) {
            next_setting_at = Clock::now() + setting_interval;
            if (command.opcode == 0xC1) noise_read_at = next_setting_at;
            return apply_mode_fast(*connection, command);
        };

        auto update_battery = [&](int attempts) {
            last_battery_query = Clock::now();
            if (auto reply = connection->query(0xF2, attempts, 300ms)) {
                current.battery = decode_battery(*reply);
                return true;
            }
            return false;
        };

        while (!stopping_) {
            const Command* command = nullptr;
            bool battery_only = false;
            {
                std::unique_lock lock(queue_mutex_);
                auto task_pending = [&] {
                    return stopping_ || !commands_pending_.empty() || full_refresh_requested_ ||
                        battery_refresh_requested_ || refresh_step != refresh_done;
                };
                if (battery_attempts_left) task_ready_.wait_until(lock, battery_retry_at, task_pending);
                else task_ready_.wait(lock, task_pending);
                if (stopping_) break;
                if (!commands_pending_.empty()) {
                    command = commands_pending_.front();
                    commands_pending_.pop_front();
                } else if (full_refresh_requested_) {
                    full_refresh_requested_ = false;
                    battery_refresh_requested_ = false;
                    battery_attempts_left = 0;
                    recovering_from_power_off = false;
                    refresh_step = 0;
                } else if (refresh_step == refresh_done) {
                    battery_refresh_requested_ = false;
                    if (recovering_from_power_off) continue;
                    const auto now = Clock::now();
                    if (battery_attempts_left) {
                        if (now < battery_retry_at) continue;
                    } else {
                        if (last_battery_query > now - 30s) continue;
                        battery_attempts_left = 3;
                    }
                    battery_only = true;
                } else if (refresh_opcodes[refresh_step] == 0xCC && Clock::now() < noise_read_at) {
                    // CC can be silent just after changing ANC. Let queued settings take priority.
                    task_ready_.wait_until(lock, noise_read_at, [&] {
                        return stopping_ || !commands_pending_.empty();
                    });
                    continue;
                }
            }

            const bool powering_off = command && command->opcode == 0xCE;
            try {
                if (command && !wait_for_setting_slot()) break;
                if (connection) {
                    try {
                        connection->check_connected();
                    } catch (const RfcommError&) {
                        connection.reset();
                    }
                }
                if (!connection) {
                    const auto address = find_paired_fitbuds();
                    current = Snapshot{};
                    if (!address) {
                        current.status = L"未找到已配对的 FitBuds，请检查 Windows 蓝牙设置";
                        refresh_step = refresh_done;
                        battery_attempts_left = 0;
                        if (command) --settings_in_progress_;
                        publish(current);
                        continue;
                    }
                    current.status = L"正在连接耳机…";
                    publish(current);
                    connection.emplace(&stopping_);
                    connection->connect_device(*address);
                    // A new Windows RFCOMM session can drop its first application request.
                    connection->query(0xD8, 1, 250ms);
                    refresh_step = 0;
                    battery_only = false;
                    battery_attempts_left = 0;
                    recovering_from_power_off = false;
                }

                if (command) {
                    if (!current.battery) {
                        update_battery(2);
                        if (current.battery && refresh_step == 0) ++refresh_step;
                    }
                    if (!current.battery) {
                        current.status = L"未能识别当前佩戴模式";
                    } else if (!allowed(*command, current.battery->present)) {
                        current.status = L"当前佩戴模式下不可用";
                    } else if (powering_off) {
                        send_setting(*command);
                        current.status = L"耳机关机后已断开";
                    } else {
                        const int target = *(command->payload.end() - 1);
                        bool confirmed = send_setting(*command);
                        if (!confirmed) {
                            // Check for a lost acknowledgement before retrying the same setting once.
                            if (!wait_for_setting_slot()) break;
                            confirmed = read_mode(*connection, command->readback, 1, 250ms) == target ||
                                send_setting(*command);
                        }
                        current.*mode_field(command->readback) = confirmed ? target : -1;
                        current.status = confirmed ? L"设置已确认" : L"耳机未确认该设置";
                    }
                } else if (battery_only) {
                    if (update_battery(1)) {
                        battery_attempts_left = 0;
                        if (current.status == L"耳机未上报电量") current.status.clear();
                    } else if (--battery_attempts_left) {
                        // After idle, the device can take time to start answering. Yield between F2 retries.
                        battery_retry_at = Clock::now() + 800ms;
                    } else {
                        current.battery.reset();
                        current.status = L"耳机未上报电量";
                    }
                } else if (refresh_step == 0) {
                    if (update_battery(3)) current.status.clear();
                    else {
                        current.battery.reset();
                        current.status = L"耳机未上报电量";
                    }
                    ++refresh_step;
                } else {
                    const auto opcode = refresh_opcodes[refresh_step++];
                    current.*mode_field(opcode) = read_mode(*connection, opcode, 1, 300ms);
                }
            } catch (const RfcommError&) {
                connection.reset();
                refresh_step = refresh_done;
                battery_attempts_left = 0;
                current = Snapshot{};
                current.status = L"蓝牙连接已断开或无法建立";
            } catch (const std::runtime_error&) {
                if (!command) refresh_step = refresh_done;
                if (battery_only) {
                    battery_attempts_left = 0;
                    current.battery.reset();
                }
                current.status = powering_off ? L"关机后未检测到断开" : L"耳机回复无法确认";
            }

            if (powering_off) {
                connection.reset();
                refresh_step = refresh_done;
                current.battery.reset();
                current.noise = current.eq = current.game = -1;
                battery_attempts_left = 0;
                recovering_from_power_off = true;
            }
            if (command) --settings_in_progress_;
            publish(current);
        }
    }

    static int Snapshot::* mode_field(std::uint8_t opcode) {
        switch (opcode) {
        case 0xCC: return &Snapshot::noise;
        case 0xD5: return &Snapshot::eq;
        default: return &Snapshot::game;
        }
    }

    void finish_job() {
        {
            std::lock_guard lock(result_mutex_);
            snapshot_ = result_;
        }
        update_tooltip();
    }

    HMENU build_menu() const {
        HMENU menu = CreatePopupMenu();
        if (!menu) return nullptr;
        const int present = snapshot_.battery ? snapshot_.battery->present : 0;
        auto label = [menu](const std::wstring& text) { AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text.c_str()); };
        label(std::wstring(app_name) + L" · " + (snapshot_.battery ? ear_mode(present) : L"无电量数据"));
        const auto& battery = snapshot_.battery;
        label(L"左耳\t" + level_text(battery ? battery->left : std::nullopt, !battery || (present & 1)));
        label(L"右耳\t" + level_text(battery ? battery->right : std::nullopt, !battery || (present & 2)));
        label(L"充电仓\t" + level_text(battery ? battery->case_level : std::nullopt));
        if (!snapshot_.status.empty()) label(snapshot_.status);
        if (const auto pending = settings_in_progress_.load()) {
            label(L"待确认设置 " + std::to_wstring(pending));
        }
        AppendMenuW(menu, MF_STRING, refresh_id, L"刷新电量与设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        auto append_setting = [&](HMENU target, size_t index, int current) {
            const auto& command = commands[index + 1];
            UINT flags = MF_STRING;
            if (snapshot_.battery && !allowed(command, present)) flags |= MF_GRAYED;
            if (current >= 0 && command.payload.size() && *(command.payload.end() - 1) == current) flags |= MF_CHECKED;
            AppendMenuW(target, flags, first_setting_id + index, setting_names[index]);
        };
        auto submenu = [&](const wchar_t* title, size_t first, size_t end, int current) {
            HMENU child = CreatePopupMenu();
            if (!child) return;
            for (size_t index = first; index < end; ++index) append_setting(child, index, current);
            if (!AppendMenuW(menu, MF_POPUP,
                             reinterpret_cast<UINT_PTR>(child), title)) DestroyMenu(child);
        };
        submenu(L"降噪与环境声", 0, 5, snapshot_.noise);
        submenu(L"音效", 5, 9, snapshot_.eq);
        submenu(L"游戏模式", 9, 11, snapshot_.game);
        append_setting(menu, 11, -1);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exit_id, L"退出托盘应用");
        return menu;
    }

    void show_menu(POINT point) {
        if (menu_open_) return;
        HMENU menu = build_menu();
        if (!menu) return;
        menu_open_ = true;
        SetForegroundWindow(window_);
        const UINT selected = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                             point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        menu_open_ = false;
        PostMessageW(window_, WM_NULL, 0, 0);
        if (selected == exit_id) DestroyWindow(window_);
        else if (selected == refresh_id) request_refresh(true);
        else if (selected >= first_setting_id && selected < first_setting_id + setting_count) {
            const auto& command = commands[selected - first_setting_id + 1];
            if (selected == first_setting_id + 11 &&
                MessageBoxW(window_, L"关闭当前连接的耳机？", app_name, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
            request_setting(command);
        }
    }

    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ && message == taskbar_created_) {
            add_icon();
            return 0;
        }
        switch (message) {
        case tray_message: {
            const UINT event = version_four_ ? LOWORD(lparam) : static_cast<UINT>(lparam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP || event == NIN_KEYSELECT) {
                POINT point{};
                GetCursorPos(&point);
                if (version_four_ && event == WM_CONTEXTMENU &&
                    (LOWORD(wparam) != 0xFFFF || HIWORD(wparam) != 0xFFFF)) {
                    point = {static_cast<short>(LOWORD(wparam)), static_cast<short>(HIWORD(wparam))};
                }
                show_menu(point);
            }
            return 0;
        }
        case result_message:
            finish_job();
            return 0;
        case WM_TIMER:
            if (wparam == refresh_timer && !menu_open_) request_refresh(false);
            return 0;
        case WM_DESTROY:
            stop_worker();
            KillTimer(window, refresh_timer);
            if (icon_added_) {
                auto data = notification();
                Shell_NotifyIconW(NIM_DELETE, &data);
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->handle(window, message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE single_instance = CreateMutexW(nullptr, FALSE, L"Local\\FitBudsPro.ControlTray");
    if (!single_instance) return 1;
    const bool already_running = GetLastError() == ERROR_ALREADY_EXISTS;
    int result = 0;
    if (!already_running) {
        TrayApp app;
        result = app.run(instance);
    }
    CloseHandle(single_instance);
    return result;
}
