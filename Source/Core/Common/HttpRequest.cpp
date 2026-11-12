// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/HttpRequest.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#ifdef __SWITCH__
#include <array>
#include <ctime>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <fmt/format.h>
#endif

#include <curl/curl.h>

#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

namespace Common
{
class HttpRequest::Impl final
{
public:
  enum class Method
  {
    GET,
    POST,
  };

  explicit Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback);

  bool IsValid() const;
  std::string GetHeaderValue(std::string_view name) const;
  void SetCookies(const std::string& cookies);
  void UseIPv4();
#ifdef __SWITCH__
  void ResolveBlockedHostsPublicly();
#endif
  void FollowRedirects(long max);
  s32 GetLastResponseCode();
  Response Fetch(const std::string& url, Method method, const Headers& headers, const u8* payload,
                 size_t size, AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only,
                 std::span<Multiform> multiform = {});

  static int CurlProgressCallback(Impl* impl, curl_off_t dltotal, curl_off_t dlnow,
                                  curl_off_t ultotal, curl_off_t ulnow);
  std::string EscapeComponent(const std::string& string);

private:
#ifdef __SWITCH__
  void ResolveBlockedHost(const std::string& url);
#endif

  static inline std::once_flag s_curl_was_initialized;
  ProgressCallback m_callback;
  Headers m_response_headers;
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> m_curl{nullptr, curl_easy_cleanup};
  std::string m_error_string;
#ifdef __SWITCH__
  bool m_resolve_blocked_hosts = false;
  std::unordered_set<std::string> m_checked_hosts;
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> m_resolve{nullptr,
                                                                         curl_slist_free_all};
#endif
};

#ifdef __SWITCH__
namespace
{
// Atmosphere's DNS redirect answers blocked hosts with a loopback or null address.
bool IsBlockedBySystemResolver(const char* host)
{
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &result) != 0)
    return true;
  Common::ScopeGuard result_guard{[result] { freeaddrinfo(result); }};

  for (const addrinfo* entry = result; entry; entry = entry->ai_next)
  {
    const u32 address = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr);
    if (address != 0 && (address >> 24) != 127)
      return false;
  }
  return true;
}

bool SkipDnsName(std::span<const u8> packet, size_t* offset)
{
  while (*offset < packet.size())
  {
    const u8 length = packet[*offset];
    if ((length & 0xC0) == 0xC0)
    {
      *offset += 2;
      return *offset <= packet.size();
    }
    *offset += 1 + length;
    if (length == 0)
      return true;
  }
  return false;
}

// Sends a plain A query to a public resolver, which the system resolver redirect never sees.
std::optional<std::string> QueryDnsServer(const char* server, const std::string& host)
{
  static u16 s_query_id = static_cast<u16>(std::time(nullptr));
  const u16 id = ++s_query_id;
  std::vector<u8> query = {static_cast<u8>(id >> 8), static_cast<u8>(id), 0x01, 0x00, 0, 1, 0, 0,
                           0, 0, 0, 0};
  for (const std::string& label : SplitString(host, '.'))
  {
    if (label.empty() || label.size() > 63)
      return std::nullopt;
    query.push_back(static_cast<u8>(label.size()));
    query.insert(query.end(), label.begin(), label.end());
  }
  query.insert(query.end(), {0, 0, 1, 0, 1});

  const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0)
    return std::nullopt;
  Common::ScopeGuard socket_guard{[socket_fd] { close(socket_fd); }};
  timeval timeout{3, 0};
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_in server_address{};
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(53);
  if (inet_pton(AF_INET, server, &server_address.sin_addr) != 1 ||
      sendto(socket_fd, query.data(), query.size(), 0,
             reinterpret_cast<const sockaddr*>(&server_address),
             sizeof(server_address)) != static_cast<ssize_t>(query.size()))
  {
    return std::nullopt;
  }

  std::array<u8, 512> buffer{};
  const ssize_t received = recv(socket_fd, buffer.data(), buffer.size(), 0);
  if (received < 12)
    return std::nullopt;
  const std::span<const u8> response(buffer.data(), static_cast<size_t>(received));
  const bool is_reply = (response[2] & 0x80) != 0;
  if (response[0] != query[0] || response[1] != query[1] || !is_reply || (response[3] & 0x0F) != 0)
    return std::nullopt;

  const u16 question_count = static_cast<u16>((response[4] << 8) | response[5]);
  const u16 answer_count = static_cast<u16>((response[6] << 8) | response[7]);
  size_t offset = 12;
  for (u16 i = 0; i < question_count; ++i)
  {
    if (!SkipDnsName(response, &offset))
      return std::nullopt;
    offset += 4;
  }
  for (u16 i = 0; i < answer_count; ++i)
  {
    if (!SkipDnsName(response, &offset) || offset + 10 > response.size())
      return std::nullopt;
    const u16 type = static_cast<u16>((response[offset] << 8) | response[offset + 1]);
    const u16 record_class = static_cast<u16>((response[offset + 2] << 8) | response[offset + 3]);
    const u16 length = static_cast<u16>((response[offset + 8] << 8) | response[offset + 9]);
    offset += 10;
    if (offset + length > response.size())
      return std::nullopt;
    if (type == 1 && record_class == 1 && length == 4)
    {
      return fmt::format("{}.{}.{}.{}", response[offset], response[offset + 1],
                         response[offset + 2], response[offset + 3]);
    }
    offset += length;
  }
  return std::nullopt;
}

std::optional<std::string> QueryPublicDns(const std::string& host)
{
  for (const char* server : {"1.1.1.1", "8.8.8.8"})
  {
    if (std::optional<std::string> address = QueryDnsServer(server, host))
      return address;
  }
  return std::nullopt;
}
}  // namespace
#endif

HttpRequest::HttpRequest(std::chrono::milliseconds timeout_ms, ProgressCallback callback)
    : m_impl(std::make_unique<Impl>(timeout_ms, std::move(callback)))
{
}

HttpRequest::~HttpRequest() = default;

bool HttpRequest::IsValid() const
{
  return m_impl->IsValid();
}

void HttpRequest::SetCookies(const std::string& cookies)
{
  m_impl->SetCookies(cookies);
}

void HttpRequest::UseIPv4()
{
  m_impl->UseIPv4();
}

#ifdef __SWITCH__
void HttpRequest::ResolveBlockedHostsPublicly()
{
  m_impl->ResolveBlockedHostsPublicly();
}
#endif

void HttpRequest::FollowRedirects(long max)
{
  m_impl->FollowRedirects(max);
}

std::string HttpRequest::EscapeComponent(const std::string& string)
{
  return m_impl->EscapeComponent(string);
}

s32 HttpRequest::GetLastResponseCode() const
{
  return m_impl->GetLastResponseCode();
}

std::string HttpRequest::GetHeaderValue(std::string_view name) const
{
  return m_impl->GetHeaderValue(name);
}

HttpRequest::Response HttpRequest::Get(const std::string& url, const Headers& headers,
                                       AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::GET, headers, nullptr, 0, codes);
}

HttpRequest::Response HttpRequest::Post(const std::string& url, std::span<const u8> payload,
                                        const Headers& headers, AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::POST, headers, payload.data(), payload.size(), codes);
}

HttpRequest::Response HttpRequest::Post(const std::string& url, std::string_view payload,
                                        const Headers& headers, AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::POST, headers,
                       reinterpret_cast<const u8*>(payload.data()), payload.size(), codes);
}

int HttpRequest::Impl::CurlProgressCallback(Impl* impl, curl_off_t dltotal, curl_off_t dlnow,
                                            curl_off_t ultotal, curl_off_t ulnow)
{
  // Abort if callback isn't true
  return !impl->m_callback(static_cast<s64>(dltotal), static_cast<s64>(dlnow),
                           static_cast<s64>(ultotal), static_cast<s64>(ulnow));
}

HttpRequest::Impl::Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback)
    : m_callback(std::move(callback))
{
  std::call_once(s_curl_was_initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  m_curl.reset(curl_easy_init());
  if (!m_curl)
    return;

  curl_easy_setopt(m_curl.get(), CURLOPT_NOPROGRESS, m_callback == nullptr);

  if (m_callback)
  {
    curl_easy_setopt(m_curl.get(), CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
  }

  // Set up error buffer
  m_error_string.resize(CURL_ERROR_SIZE);
  curl_easy_setopt(m_curl.get(), CURLOPT_ERRORBUFFER, m_error_string.data());

  // libcurl may not have been built with async DNS support, so we disable
  // signal handlers to avoid a possible and likely crash if a resolve times out.
  curl_easy_setopt(m_curl.get(), CURLOPT_NOSIGNAL, true);
  curl_easy_setopt(m_curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms.count()));
  // Sadly CURLOPT_LOW_SPEED_TIME doesn't have a millisecond variant so we have to use seconds
  curl_easy_setopt(
      m_curl.get(), CURLOPT_LOW_SPEED_TIME,
      static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(timeout_ms).count()));
  curl_easy_setopt(m_curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1);
}

bool HttpRequest::Impl::IsValid() const
{
  return m_curl != nullptr;
}

s32 HttpRequest::Impl::GetLastResponseCode()
{
  long response_code{};
  curl_easy_getinfo(m_curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
  return static_cast<s32>(response_code);
}

void HttpRequest::Impl::SetCookies(const std::string& cookies)
{
  curl_easy_setopt(m_curl.get(), CURLOPT_COOKIE, cookies.c_str());
}

void HttpRequest::Impl::UseIPv4()
{
  curl_easy_setopt(m_curl.get(), CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
}

#ifdef __SWITCH__
void HttpRequest::Impl::ResolveBlockedHostsPublicly()
{
  m_resolve_blocked_hosts = true;
}

void HttpRequest::Impl::ResolveBlockedHost(const std::string& url)
{
  CURLU* const parsed = curl_url();
  if (!parsed)
    return;
  Common::ScopeGuard parsed_guard{[parsed] { curl_url_cleanup(parsed); }};
  char* host = nullptr;
  char* port = nullptr;
  if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK ||
      curl_url_get(parsed, CURLUPART_HOST, &host, 0) != CURLUE_OK)
  {
    return;
  }
  Common::ScopeGuard host_guard{[host] { curl_free(host); }};
  if (curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) != CURLUE_OK)
    return;
  Common::ScopeGuard port_guard{[port] { curl_free(port); }};

  const std::string key = fmt::format("{}:{}", host, port);
  if (!m_checked_hosts.insert(key).second || !IsBlockedBySystemResolver(host))
    return;

  const std::optional<std::string> address = QueryPublicDns(host);
  if (!address)
  {
    ERROR_LOG_FMT(COMMON, "Failed to resolve {} through public DNS", host);
    return;
  }

  curl_slist* const list =
      curl_slist_append(m_resolve.get(), fmt::format("{}:{}", key, *address).c_str());
  if (!list)
    return;
  (void)m_resolve.release();
  m_resolve.reset(list);
  curl_easy_setopt(m_curl.get(), CURLOPT_RESOLVE, m_resolve.get());
}
#endif

HttpRequest::Response HttpRequest::PostMultiform(const std::string& url,
                                                 std::span<Multiform> multiform,
                                                 const Headers& headers, AllowedReturnCodes codes)
{
  return m_impl->Fetch(url, Impl::Method::POST, headers, nullptr, 0, codes, multiform);
}

void HttpRequest::Impl::FollowRedirects(long max)
{
  curl_easy_setopt(m_curl.get(), CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(m_curl.get(), CURLOPT_MAXREDIRS, max);
}

std::string HttpRequest::Impl::GetHeaderValue(std::string_view name) const
{
  for (const auto& [key, value] : m_response_headers)
  {
    if (key == name)
      return value.value();
  }

  return {};
}

std::string HttpRequest::Impl::EscapeComponent(const std::string& string)
{
  char* escaped = curl_easy_escape(m_curl.get(), string.c_str(), static_cast<int>(string.size()));
  std::string escaped_str(escaped);
  curl_free(escaped);

  return escaped_str;
}

static size_t CurlWriteCallback(char* data, size_t size, size_t nmemb, void* userdata)
{
  auto* buffer = static_cast<std::vector<u8>*>(userdata);
  const size_t actual_size = size * nmemb;
  buffer->insert(buffer->end(), data, data + actual_size);
  return actual_size;
}

static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* headers = static_cast<HttpRequest::Headers*>(userdata);
  std::string_view full_buffer = std::string_view{buffer, nitems};
  const size_t colon_pos = full_buffer.find(':');
  if (colon_pos == std::string::npos)
    return nitems * size;

  const std::string_view key = full_buffer.substr(0, colon_pos);
  const std::string_view value = StripWhitespace(full_buffer.substr(colon_pos + 1));

  headers->emplace(std::string{key}, std::string{value});
  return nitems * size;
}

HttpRequest::Response HttpRequest::Impl::Fetch(const std::string& url, Method method,
                                               const Headers& headers, const u8* payload,
                                               size_t size, AllowedReturnCodes codes,
                                               std::span<Multiform> multiform)
{
  m_response_headers.clear();
#ifdef __SWITCH__
  if (m_resolve_blocked_hosts)
    ResolveBlockedHost(url);
#endif
  curl_easy_setopt(m_curl.get(), CURLOPT_POST, method == Method::POST);
  curl_easy_setopt(m_curl.get(), CURLOPT_URL, url.c_str());
  if (method == Method::POST && multiform.empty())
  {
    curl_easy_setopt(m_curl.get(), CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(m_curl.get(), CURLOPT_POSTFIELDSIZE, size);
  }

  curl_mime* form = nullptr;
  Common::ScopeGuard multiform_guard{[&form] { curl_mime_free(form); }};
  if (!multiform.empty())
  {
    form = curl_mime_init(m_curl.get());
    for (const auto& value : multiform)
    {
      curl_mimepart* part = curl_mime_addpart(form);
      curl_mime_name(part, value.name.c_str());
      curl_mime_data(part, value.data.c_str(), value.data.size());
    }

    curl_easy_setopt(m_curl.get(), CURLOPT_MIMEPOST, form);
  }

  curl_slist* list = nullptr;
  Common::ScopeGuard list_guard{[&list] { curl_slist_free_all(list); }};
  for (const auto& [name, value] : headers)
  {
    if (!value)
      list = curl_slist_append(list, (name + ':').c_str());
    else if (value->empty())
      list = curl_slist_append(list, (name + ';').c_str());
    else
      list = curl_slist_append(list, (name + ": " + *value).c_str());
  }
  curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, list);

  curl_easy_setopt(m_curl.get(), CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(m_curl.get(), CURLOPT_HEADERDATA, static_cast<void*>(&m_response_headers));

  std::vector<u8> buffer;
  curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, &buffer);

  const char* type = method == Method::POST ? "POST" : "GET";
  const CURLcode res = curl_easy_perform(m_curl.get());
  if (res != CURLE_OK)
  {
    ERROR_LOG_FMT(COMMON, "Failed to {} {}: {}", type, url, m_error_string);
    return {};
  }

  if (codes == AllowedReturnCodes::All)
    return buffer;

  long response_code = 0;
  curl_easy_getinfo(m_curl.get(), CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code != 200)
  {
    if (buffer.empty())
    {
      ERROR_LOG_FMT(COMMON, "Failed to {} {}: server replied with code {}", type, url,
                    response_code);
    }
    else
    {
      ERROR_LOG_FMT(COMMON, "Failed to {} {}: server replied with code {} and body\n\x1b[0m{:.{}}",
                    type, url, response_code, reinterpret_cast<char*>(buffer.data()),
                    static_cast<int>(buffer.size()));
    }
    return {};
  }

  return buffer;
}
}  // namespace Common
