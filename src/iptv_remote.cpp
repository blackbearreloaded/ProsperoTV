/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_remote.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#ifdef __PROSPERO__
// Native applications use SceNet socket IDs, not restricted libkernel sockets.
extern "C"
{
    int sceNetInit(void);
    int sceNetSocket(const char *, int, int, int);
    int sceNetSocketClose(int);
    int sceNetBind(int, const sockaddr *, socklen_t);
    int sceNetListen(int, int);
    int sceNetAccept(int, sockaddr *, socklen_t *);
    int sceNetRecv(int, void *, size_t, int);
    int sceNetSend(int, const void *, size_t, int);
    int sceNetSetsockopt(int, int, int, const void *, socklen_t);
    int sceNetConnect(int, const sockaddr *, socklen_t);
    int sceNetGetsockname(int, sockaddr *, socklen_t *);
    int *sceNetErrnoLoc(void);
}
#define socket(domain, type, protocol) sceNetSocket("ProsperoTVRemote", domain, type, protocol)
#define close sceNetSocketClose
#define bind sceNetBind
#define listen sceNetListen
#define accept sceNetAccept
#define recv sceNetRecv
#define send sceNetSend
#define setsockopt sceNetSetsockopt
#define connect sceNetConnect
#define getsockname sceNetGetsockname
#endif

namespace
{
constexpr char page[] =
#include "iptv_remote_page.h"
    ;
constexpr size_t max_request = 4096;
struct Client
{
    int fd = -1;
    std::string input, output;
    size_t sent = 0;
    uint64_t deadline = 0;
};
int listener = -1;
std::array<Client, 4> clients;
std::array<iptv_input_action_t, 32> actions;
size_t read_at = 0, write_at = 0;
char pin[7], hint[160] = "Phone remote unavailable";
char search_text[IPTV_IME_MAX_TEXT_BYTES];
bool search_enabled = false, search_pending = false;
uint64_t next_pair_attempt = 0;

uint64_t Now()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
void Close(Client &client)
{
    if (client.fd >= 0)
        close(client.fd);
    client = Client{};
}
bool Nonblocking(int fd)
{
    int enabled = 1;
#ifdef __PROSPERO__
    constexpr int net_so_nbio = 0x1200;
    return sceNetSetsockopt(fd, SOL_SOCKET, net_so_nbio, &enabled, sizeof(enabled)) == 0;
#else
    return ioctl(fd, FIONBIO, &enabled) == 0;
#endif
}
int SocketError()
{
#ifdef __PROSPERO__
    return *sceNetErrnoLoc() & 0xff;
#else
    return errno;
#endif
}
bool RetrySocket()
{
    const int error = SocketError();
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}
void Reply(Client &client, int code, std::string_view body, bool html = false)
{
    client.output =
        "HTTP/1.1 " + std::to_string(code) + " " + (code == 200 ? "OK" : "Error") +
        "\r\nContent-Type: " + (html ? "text/html; charset=utf-8" : "text/plain; charset=utf-8") +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'self'\r\n\r\n";
    client.output.append(body);
}
bool ValidText(std::string_view text)
{
    if (text.size() >= IPTV_IME_MAX_TEXT_BYTES)
        return false;
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); ++characters)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        uint32_t cp = first;
        unsigned remaining = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf)
        {
            cp &= 31;
            remaining = 1;
            minimum = 0x80;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            cp &= 15;
            remaining = 2;
            minimum = 0x800;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            cp &= 7;
            remaining = 3;
            minimum = 0x10000;
        }
        else if (first < 32 || first >= 127)
            return false;
        if (i + remaining > text.size())
            return false;
        while (remaining--)
        {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (byte & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return characters <= IPTV_IME_MAX_TEXT_CHARACTERS;
}
void Request(Client &client)
{
    const size_t end = client.input.find("\r\n\r\n");
    if (end == std::string::npos)
        return;
    std::string_view request(client.input);
    const size_t first_end = request.find("\r\n");
    const auto first = request.substr(0, first_end);
    size_t length = 0;
    bool has_length = false, authorized = false, invalid = false;
    for (size_t pos = first_end + 2; pos < end;)
    {
        const size_t line_end = request.find("\r\n", pos);
        auto line = request.substr(pos, line_end - pos);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            invalid = true;
            break;
        }
        std::string key(line.substr(0, colon));
        for (char &c : key)
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
        if (key == "content-length")
        {
            if (has_length || value.empty())
                invalid = true;
            has_length = true;
            for (char c : value)
            {
                if (c < '0' || c > '9' || length > max_request)
                {
                    invalid = true;
                    break;
                }
                length = length * 10 + size_t(c - '0');
            }
        }
        else if (key == "transfer-encoding")
            invalid = true;
        else if (key == "x-remote-pin")
            authorized = value == pin;
        pos = line_end + 2;
    }
    if (invalid || length > max_request - end - 4)
    {
        Reply(client, 400, "Invalid request");
        return;
    }
    if (request.size() < end + 4 + length)
        return;
    if (first == "GET / HTTP/1.1" || first == "GET / HTTP/1.0")
    {
        Reply(client, 200, page, true);
        return;
    }
    // A custom header is required: cross-origin forms cannot issue commands and
    // preflight requests are never granted CORS access. The PIN stays off URLs.
    if (Now() < next_pair_attempt)
    {
        Reply(client, 429, "Wait a second before trying the pairing code again.");
        return;
    }
    if (!authorized)
    {
        const auto now = Now();
        Reply(client, now < next_pair_attempt ? 429 : 401,
              "Enter the pairing code shown on your TV.");
        next_pair_attempt = now + 1000;
        return;
    }
    if (first == "GET /api/status HTTP/1.1")
    {
        Reply(client, 200,
              search_enabled ? "Connected · Ready to browse"
                             : "Connected · Controls ready (Back to browse)");
        return;
    }
    const auto body = request.substr(end + 4, length);
    if (first == "POST /api/search HTTP/1.1")
    {
        if (!has_length || !ValidText(body))
        {
            Reply(client, 400, "Use up to 39 characters of valid text.");
            return;
        }
        if (!search_enabled || search_pending)
        {
            Reply(client, 409, "Return to the channel browser before searching.");
            return;
        }
        std::memcpy(search_text, body.data(), body.size());
        search_text[body.size()] = 0;
        search_pending = true;
        Reply(client, 200, body.empty() ? "Search cleared" : "Search applied on TV");
        return;
    }
    if (first == "POST /api/key HTTP/1.1")
    {
        static constexpr std::pair<std::string_view, iptv_input_action_t> keys[] = {
            {"up", IPTV_INPUT_UP},           {"down", IPTV_INPUT_DOWN},
            {"left", IPTV_INPUT_LEFT},       {"right", IPTV_INPUT_RIGHT},
            {"enter", IPTV_INPUT_CROSS},     {"back", IPTV_INPUT_CIRCLE},
            {"search", IPTV_INPUT_TRIANGLE}, {"favorite", IPTV_INPUT_SQUARE},
            {"previous", IPTV_INPUT_L1},     {"next", IPTV_INPUT_R1}};
        for (const auto &key : keys)
            if (key.first == body)
            {
                const auto next = (write_at + 1) % actions.size();
                if (next == read_at)
                {
                    Reply(client, 429, "Remote busy. Try again.");
                    return;
                }
                actions[write_at] = key.second;
                write_at = next;
                Reply(client, 200, "Sent to TV");
                return;
            }
        Reply(client, 400, "Unknown key");
        return;
    }
    Reply(client, 404, "Not found");
}
} // namespace

bool iptv_remote_start(unsigned short port)
{
    if (listener >= 0)
        return true;
#ifdef __PROSPERO__
    // Shared with the app's HTTP client; do not terminate SceNet on remote stop.
    sceNetInit();
#endif
    unsigned random = 0;
    FILE *rng = std::fopen("/dev/urandom", "rb");
    const bool random_ok = rng && std::fread(&random, sizeof(random), 1, rng) == 1;
    if (rng)
        std::fclose(rng);
    if (!random_ok)
        return false;
    std::snprintf(pin, sizeof(pin), "%06u", random % 1000000);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return false;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
#ifdef __FreeBSD__
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const char *failed_stage = nullptr;
    int result = -1;
    if (!Nonblocking(listener))
        failed_stage = "nonblocking";
    else
    {
        result = bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        if (result != 0 && (SocketError() == EACCES || SocketError() == EADDRINUSE))
        {
            // Request an allowed/free port when the preferred port is unavailable.
            address.sin_port = 0;
            result = bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        }
        if (result != 0)
            failed_stage = "bind";
        else if ((result = listen(listener, 4)) != 0)
            failed_stage = "listen";
        else
        {
            socklen_t size = sizeof(address);
            if ((result = getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size)) !=
                0)
                failed_stage = "address";
            else
                port = ntohs(address.sin_port);
        }
    }
    if (failed_stage)
    {
        const int error = SocketError();
        iptv_remote_stop();
        std::snprintf(hint, sizeof(hint), "Phone remote unavailable: %s failed (%d, 0x%08x)",
                      failed_stage, error, static_cast<unsigned>(result));
        return false;
    }
    char ip[INET_ADDRSTRLEN] = "<PS5-IP>";
    const int route = socket(AF_INET, SOCK_DGRAM, 0);
    if (route >= 0)
    {
        sockaddr_in destination{};
#ifdef __FreeBSD__
        destination.sin_len = sizeof(destination);
#endif
        destination.sin_family = AF_INET;
        destination.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &destination.sin_addr);
        socklen_t size = sizeof(address);
        if (connect(route, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == 0 &&
            getsockname(route, reinterpret_cast<sockaddr *>(&address), &size) == 0)
            inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
        close(route);
    }
    std::snprintf(hint, sizeof(hint), "Phone: http://%s:%u  |  Code: %s", ip, port, pin);
    return true;
}
void iptv_remote_stop(void)
{
    for (auto &client : clients)
        Close(client);
    if (listener >= 0)
        close(listener);
    listener = -1;
    read_at = write_at = 0;
    search_pending = search_enabled = false;
    next_pair_attempt = 0;
    std::memset(pin, 0, sizeof(pin));
    std::snprintf(hint, sizeof(hint), "Phone remote unavailable (port 8080)");
}
void iptv_remote_poll(void)
{
    if (listener < 0)
        return;
    const auto now = Now();
    for (auto &client : clients)
    {
        if (client.fd < 0)
        {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd < 0)
                continue;
            if (!Nonblocking(fd))
            {
                close(fd);
                continue;
            }
            client.fd = fd;
            client.deadline = now + 2000;
        }
        if (now >= client.deadline)
        {
            Close(client);
            continue;
        }
        if (client.output.empty())
        {
            char buffer[1024];
            const auto count = recv(client.fd, buffer, sizeof(buffer), 0);
            if (count == 0 || (count < 0 && !RetrySocket()))
            {
                Close(client);
                continue;
            }
            if (count > 0)
            {
                client.input.append(buffer, size_t(count));
                if (client.input.size() > max_request)
                    Reply(client, 413, "Request too large");
                else
                    Request(client);
            }
        }
        if (!client.output.empty())
        {
            const auto count = send(client.fd, client.output.data() + client.sent,
                                    client.output.size() - client.sent, MSG_NOSIGNAL);
            if (count > 0)
                client.sent += size_t(count);
            if (client.sent == client.output.size() || (count < 0 && !RetrySocket()))
                Close(client);
        }
    }
}
bool iptv_remote_next(iptv_input_event_t *event)
{
    if (!event || read_at == write_at)
        return false;
    *event = {actions[read_at], true};
    read_at = (read_at + 1) % actions.size();
    return true;
}
bool iptv_remote_search(char text[IPTV_IME_MAX_TEXT_BYTES])
{
    if (!search_pending || !text)
        return false;
    std::memcpy(text, search_text, sizeof(search_text));
    search_pending = false;
    return true;
}
void iptv_remote_enable_search(bool enabled)
{
    search_enabled = enabled;
    if (!enabled)
        search_pending = false;
}
const char *iptv_remote_hint(void)
{
    return hint;
}
