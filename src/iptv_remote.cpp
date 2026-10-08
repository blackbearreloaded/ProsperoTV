/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_remote.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <algorithm>
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
constexpr size_t max_request = 16384;
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
char pin[7], hint[160] = "Phone remote unavailable", remote_url[80];
std::string pairing_store;
std::array<std::string, 8> paired_tokens;
uint64_t pairing_deadline = 0;
bool connected_pending = false;
unsigned volume = 100;
bool (*save_volume)(unsigned, void *) = nullptr;
void *volume_context = nullptr;
int (*sources_handler)(const char *, size_t, char *, size_t, void *) = nullptr;
void *sources_context = nullptr;
char search_text[IPTV_IME_MAX_TEXT_BYTES];
bool search_enabled = false, search_pending = false;
uint64_t next_pair_attempt = 0;
int (*playback_favorite)(void *) = nullptr;
void *playback_favorite_context = nullptr;
std::string icon;

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
void Reply(Client &client, int code, std::string_view body,
           const char *content_type = "text/plain; charset=utf-8", std::string_view headers = {})
{
    client.output =
        "HTTP/1.1 " + std::to_string(code) + " " + (code == 200 ? "OK" : "Error") +
        "\r\nContent-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'; "
        "base-uri 'none'; form-action 'self'\r\n";
    client.output.append(headers);
    client.output.append("\r\n");
    client.output.append(body);
}
bool Random(void *bytes, size_t size)
{
    FILE *file = std::fopen("/dev/urandom", "rb");
    if (!file)
        return false;
    const bool ok = std::fread(bytes, 1, size, file) == size;
    std::fclose(file);
    return ok;
}
bool SaveTokens(const std::array<std::string, 8> &tokens)
{
    if (pairing_store.empty())
        return false;
    const std::string temporary = pairing_store + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    for (const auto &token : tokens)
        if (!token.empty())
            std::fprintf(file, "%s\n", token.c_str());
    const bool written = std::fflush(file) == 0 && !std::ferror(file);
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || chmod(temporary.c_str(), 0600) != 0 ||
        std::rename(temporary.c_str(), pairing_store.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
bool ValidToken(std::string_view token)
{
    return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == token.npos;
}
std::string Cookie(std::string_view token)
{
    return "Set-Cookie: prosperotv_remote=" + std::string(token) +
           "; Path=/; Max-Age=" + (token.empty() ? "0" : "31536000") +
           "; HttpOnly; SameSite=Strict\r\n";
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
    bool has_length = false, remote_header = false, invalid = false;
    std::string session_token;
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
        else if (key == "x-prosperotv-remote")
            remote_header = value == "1";
        else if (key == "cookie")
        {
            while (!value.empty())
            {
                const auto separator = value.find(';');
                auto cookie = value.substr(0, separator);
                while (!cookie.empty() && cookie.front() == ' ')
                    cookie.remove_prefix(1);
                constexpr std::string_view name = "prosperotv_remote=";
                if (cookie.starts_with(name))
                    session_token = cookie.substr(name.size());
                if (separator == value.npos)
                    break;
                value.remove_prefix(separator + 1);
            }
        }
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
        Reply(client, 200, page, "text/html; charset=utf-8");
        return;
    }
    if (first == "GET /icon.png HTTP/1.1" && !icon.empty())
    {
        Reply(client, 200, icon, "image/png");
        return;
    }
    // Cookie authentication still requires a custom header: cross-origin forms
    // cannot issue commands, and preflight requests are never granted CORS.
    const auto body = request.substr(end + 4, length);
    if (!remote_header)
    {
        Reply(client, 403, "Open the ProsperoTV remote website to connect.");
        return;
    }
    if (first == "POST /api/pair HTTP/1.1")
    {
        if (Now() < next_pair_attempt)
        {
            Reply(client, 429, "Wait a second before trying the pairing code again.");
            return;
        }
        if (!iptv_remote_pairing_seconds() || body.size() != 6 || body != pin)
        {
            next_pair_attempt = Now() + 1000;
            Reply(client, 401, "Select Pair a phone in TV Settings and enter its current code.");
            return;
        }
        auto tokens = paired_tokens;
        auto slot = std::find(tokens.begin(), tokens.end(), std::string{});
        if (slot == tokens.end())
        {
            Reply(client, 409,
                  "Eight phones are paired. Forget paired phones in TV Settings first.");
            return;
        }
        unsigned char bytes[32];
        if (!Random(bytes, sizeof(bytes)))
        {
            Reply(client, 500, "Cannot create a pairing token. Try again.");
            return;
        }
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned char byte : bytes)
        {
            slot->push_back(hex[byte >> 4]);
            slot->push_back(hex[byte & 15]);
        }
        if (!SaveTokens(tokens))
        {
            Reply(client, 500, "Cannot save this phone on the TV. Try again.");
            return;
        }
        const std::string cookie = Cookie(*slot);
        paired_tokens = std::move(tokens);
        iptv_remote_cancel_pairing();
        Reply(client, 200, "Phone paired", "text/plain; charset=utf-8", cookie);
        return;
    }
    if (!ValidToken(session_token) ||
        std::find(paired_tokens.begin(), paired_tokens.end(), session_token) == paired_tokens.end())
    {
        Reply(client, 401, "Select Pair a phone in TV Settings to connect this browser.");
        return;
    }
    if (first == "GET /api/status HTTP/1.1")
    {
        connected_pending = true;
        Reply(client, 200,
              search_enabled ? "Connected · Ready to browse"
                             : "Connected · Controls ready (Back to browse)");
        return;
    }
    if (first == "GET /api/sources HTTP/1.1" || first == "POST /api/sources HTTP/1.1")
    {
        const bool listing = first.front() == 'G';
        if (!sources_handler)
            Reply(client, 409, "Return to the TV menu to manage sources.");
        else if (!listing && (!has_length || body.empty()))
            Reply(client, 400, "Source details are required.");
        else
        {
            std::string output(128u * 1024u, '\0');
            const int code = sources_handler(listing ? "" : body.data(), listing ? 0 : body.size(),
                                             output.data(), output.size(), sources_context);
            output.resize(std::strlen(output.c_str()));
            Reply(client, code, output,
                  listing && code == 200 ? "application/json" : "text/plain; charset=utf-8");
        }
        return;
    }
    if (first == "POST /api/disconnect HTTP/1.1")
    {
        auto tokens = paired_tokens;
        *std::find(tokens.begin(), tokens.end(), session_token) = {};
        if (!SaveTokens(tokens))
        {
            Reply(client, 500, "Could not forget this phone. Try again.");
            return;
        }
        paired_tokens = std::move(tokens);
        Reply(client, 200, "Phone forgotten", "text/plain; charset=utf-8", Cookie({}));
        return;
    }
    if (first == "GET /api/volume HTTP/1.1")
    {
        Reply(client, 200, std::to_string(volume));
        return;
    }
    if (first == "POST /api/volume HTTP/1.1")
    {
        unsigned requested = 0;
        if (body.empty() || body.size() > 3 || body.find_first_not_of("0123456789") != body.npos)
        {
            Reply(client, 400, "Volume must be between 0 and 100.");
            return;
        }
        for (char c : body)
            requested = requested * 10 + unsigned(c - '0');
        if (requested > 100)
            Reply(client, 400, "Volume must be between 0 and 100.");
        else if (!save_volume || !save_volume(requested, volume_context))
            Reply(client, 500, "Could not save volume. Try again.");
        else
        {
            volume = requested;
            Reply(client, 200, requested == 0 ? "Muted" : "Volume " + std::to_string(volume) + "%");
        }
        return;
    }
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
        if (body == "favorite" && playback_favorite)
        {
            const int result = playback_favorite(playback_favorite_context);
            Reply(client, result < 0 ? 500 : 200,
                  result < 0   ? "Could not save favorites. Try again."
                  : result > 0 ? "Added to favorites"
                               : "Removed from favorites");
            return;
        }
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
    std::snprintf(remote_url, sizeof(remote_url), "http://%s:%u", ip, port);
    std::snprintf(hint, sizeof(hint), "Phone remote: Settings > Pair a phone");
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
    connected_pending = false;
    next_pair_attempt = 0;
    iptv_remote_cancel_pairing();
    remote_url[0] = 0;
    iptv_remote_set_playback_favorite(nullptr, nullptr);
    iptv_remote_set_sources_handler(nullptr, nullptr);
    std::memset(pin, 0, sizeof(pin));
    std::snprintf(hint, sizeof(hint), "Phone remote unavailable (port 8888)");
}
void iptv_remote_set_sources_handler(int (*handle)(const char *, size_t, char *, size_t, void *),
                                     void *context)
{
    sources_handler = handle;
    sources_context = context;
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
void iptv_remote_set_playback_favorite(int (*toggle)(void *), void *context)
{
    playback_favorite = toggle;
    playback_favorite_context = context;
}
void iptv_remote_set_icon(const char *path)
{
    icon.clear();
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return;
    char chunk[4096];
    size_t bytes = 0;
    while ((bytes = std::fread(chunk, 1, sizeof(chunk), file)) != 0)
    {
        icon.append(chunk, bytes);
        if (icon.size() > 1024 * 1024)
            break;
    }
    if (std::ferror(file) || icon.size() > 1024 * 1024)
        icon.clear();
    std::fclose(file);
}
void iptv_remote_set_pairing_store(const char *path)
{
    pairing_store = path;
    paired_tokens = {};
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return;
    char line[68];
    unsigned count = 0;
    while (count < paired_tokens.size() && std::fgets(line, sizeof(line), file))
    {
        std::string token(line);
        if (!token.empty() && token.back() == '\n')
            token.pop_back();
        if (ValidToken(token))
            paired_tokens[count++] = std::move(token);
    }
    std::fclose(file);
}
bool iptv_remote_begin_pairing(void)
{
    connected_pending = false;
    iptv_remote_cancel_pairing();
    unsigned random;
    if (listener < 0 || !Random(&random, sizeof(random)))
        return false;
    std::snprintf(pin, sizeof(pin), "%06u", random % 1000000);
#ifndef IPTV_REMOTE_PAIRING_MS
#define IPTV_REMOTE_PAIRING_MS 120000
#endif
    pairing_deadline = Now() + IPTV_REMOTE_PAIRING_MS;
    return true;
}
void iptv_remote_cancel_pairing(void)
{
    pin[0] = 0;
    pairing_deadline = 0;
}
bool iptv_remote_take_connected(void)
{
    const bool connected = connected_pending;
    connected_pending = false;
    return connected;
}
const char *iptv_remote_url(void)
{
    return remote_url;
}
const char *iptv_remote_pairing_code(void)
{
    return iptv_remote_pairing_seconds() ? pin : "";
}
unsigned iptv_remote_pairing_seconds(void)
{
    const auto now = Now();
    return pairing_deadline > now ? unsigned((pairing_deadline - now + 999) / 1000) : 0;
}
unsigned iptv_remote_paired_count(void)
{
    return unsigned(std::count_if(paired_tokens.begin(), paired_tokens.end(),
                                  [](const std::string &token) { return !token.empty(); }));
}
bool iptv_remote_forget_phones(void)
{
    if (!SaveTokens({}))
        return false;
    paired_tokens = {};
    iptv_remote_cancel_pairing();
    read_at = write_at = 0;
    search_pending = false;
    return true;
}
void iptv_remote_set_volume(unsigned value)
{
    volume = std::min(value, 100u);
}
void iptv_remote_set_volume_handler(bool (*save)(unsigned, void *), void *context)
{
    save_volume = save;
    volume_context = context;
}
