// transmission-client-gtk — HTTP RPC to a remote transmission-daemon
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <libtransmission/api-compat.h>
#include <libtransmission/quark.h>
#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>

struct tr_session;
typedef struct _SoupSession SoupSession;

namespace transmission::client::gtk
{

struct RpcResponse
{
    bool success = false;
    std::string errmsg;
    std::unique_ptr<tr_variant> args;
};

class RpcClient
{
public:
    RpcClient();
    ~RpcClient();

    RpcClient(RpcClient const&) = delete;
    RpcClient& operator=(RpcClient const&) = delete;

    [[nodiscard]] bool is_local() const noexcept
    {
        return session_ != nullptr;
    }

    void stop();
    void start_local(tr_session* session);
    void start_remote(std::string_view url, std::string_view username, std::string_view password);

    using Callback = std::function<void(RpcResponse&&)>;

    void exec_async(tr_quark method, tr_variant const& params, Callback callback);
    void exec_async(tr_quark method, Callback callback);

private:
    void exec_local(tr_variant& request, Callback callback);
    void exec_remote(tr_variant& request, Callback callback);
    bool post_remote(std::string const& body, RpcResponse& response);

    tr_session* session_ = nullptr;
    SoupSession* soup_session_ = nullptr;
    std::string url_;
    std::string username_;
    std::string password_;
    std::string session_id_;
    libtransmission::api_compat::Style network_style_ = libtransmission::api_compat::Style::Tr4;
    bool const verbose_ = tr_env_key_exists("TR_RPC_VERBOSE");
};

} // namespace transmission::client::gtk
