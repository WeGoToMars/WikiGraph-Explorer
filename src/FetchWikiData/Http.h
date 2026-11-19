#pragma once

#include <cpr/cpr.h>

#include <chrono>
#include <stop_token>

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

}  // namespace wiki_http
