// transmission-client-gtk — remote session preference helpers
#pragma once

#include "Prefs.h"
#include "Session.h"

#include <libtransmission/quark.h>
#include <libtransmission/transmission.h>

#include <fmt/format.h>

#include <string>

namespace transmission::client::gtk
{

enum class SessionMode
{
    Remote = 0,
    Host = 1,
    None = 2,
};

[[nodiscard]] inline SessionMode get_session_mode()
{
    if (gtr_pref_flag_get(TR_KEY_remote_session_enabled))
    {
        return SessionMode::Remote;
    }

    if (gtr_pref_flag_get(TR_KEY_rpc_enabled))
    {
        return SessionMode::Host;
    }

    return SessionMode::None;
}

[[nodiscard]] inline SessionMode get_runtime_session_mode(Session const& core)
{
    if (core.is_remote())
    {
        return SessionMode::Remote;
    }

    if (core.get_session() != nullptr && gtr_pref_flag_get(TR_KEY_rpc_enabled))
    {
        return SessionMode::Host;
    }

    return SessionMode::None;
}

inline void apply_session_mode_prefs(SessionMode const mode)
{
    switch (mode)
    {
    case SessionMode::Remote:
        gtr_pref_flag_set(TR_KEY_remote_session_enabled, true);
        break;
    case SessionMode::Host:
        gtr_pref_flag_set(TR_KEY_remote_session_enabled, false);
        gtr_pref_flag_set(TR_KEY_rpc_enabled, true);
        break;
    case SessionMode::None:
        gtr_pref_flag_set(TR_KEY_remote_session_enabled, false);
        gtr_pref_flag_set(TR_KEY_rpc_enabled, false);
        break;
    }
}

[[nodiscard]] inline bool is_remote_session_enabled()
{
    return get_session_mode() == SessionMode::Remote;
}

[[nodiscard]] inline std::string build_remote_rpc_url()
{
    auto host = gtr_pref_string_get(TR_KEY_remote_session_host);
    if (host.empty())
    {
        host = "127.0.0.1";
    }

    auto const port = static_cast<int>(gtr_pref_int_get(TR_KEY_remote_session_port));
    auto const use_https = gtr_pref_flag_get(TR_KEY_remote_session_https);
    auto path = gtr_pref_string_get(TR_KEY_remote_session_url_base_path);
    if (path.empty())
    {
        path = "/transmission/";
    }
    if (path.back() != '/')
    {
        path.push_back('/');
    }
    path.append("rpc");

    return fmt::format("{}://{}:{}{}", use_https ? "https" : "http", host, port > 0 ? port : 9091, path);
}

} // namespace transmission::client::gtk
