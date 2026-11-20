#pragma once

#include <cpr/cpr.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

namespace wiki_http {

/// Wikimedia enforces its User-Agent policy on dump downloads and may block generic clients such as "curl/8.x".
/// The policy asks for the tool's name and version plus contact information, such as the project page.
/// https://foundation.wikimedia.org/wiki/Policy:Wikimedia_Foundation_User-Agent_Policy
inline cpr::UserAgent user_agent() {
    return cpr::UserAgent{"WikiGraph-Explorer/1.0 (https://github.com/WeGoToMars/WikiGraph-Explorer) libcurl"};
}

/// Small requests (statistics, RSS feeds, HEAD) give up after this long.
inline constexpr std::chrono::milliseconds kRequestTimeout{20000};
/// Downloads: time to connect, and how long the transfer may stall below 1 byte/s before it is aborted.
inline constexpr std::chrono::milliseconds kConnectTimeout{20000};
inline constexpr std::chrono::seconds kStallTimeout{60};

/// Aborts a request (the callback returns false) once a stop is requested.
inline cpr::ProgressCallback cancel_callback(std::stop_token stop) {
    return cpr::ProgressCallback([token = std::move(stop)](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t,
                                                           intptr_t) { return !token.stop_requested(); });
}

/// The certificate authorities to trust on Linux: $SSL_CERT_FILE, else the first bundle found where the common
/// distributions keep it, else empty (curl's built-in default). The static Linux build carries its own libcurl,
/// whose built-in path is that of the system it was built on.
inline const std::string& ca_bundle() {
    static const std::string path = [] {
        if (const char* env = std::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0') return std::string(env);
        for (const char* candidate : {
                 "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Alpine, Arch, Gentoo
                 "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL, CentOS
                 "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL 9+
                 "/etc/ssl/ca-bundle.pem",                             // openSUSE
                 "/etc/ssl/cert.pem",                                  // Void, some minimal images
             }) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return std::string(candidate);
        }
        return std::string();
    }();
    return path;
}

/// cpr::Get, trusting ca_bundle() on Linux.
template <class... Options>
cpr::Response get(Options&&... options) {
#ifdef __linux__
    if (const std::string& ca = ca_bundle(); !ca.empty())
        return cpr::Get(std::forward<Options>(options)..., cpr::Ssl(cpr::ssl::CaInfo{std::string(ca)}));
#endif
    return cpr::Get(std::forward<Options>(options)...);
}

}  // namespace wiki_http
