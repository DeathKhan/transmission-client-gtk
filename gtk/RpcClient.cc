// transmission-client-gtk — HTTP RPC client (see qt/RpcClient.cc)
#include "RpcClient.h"

#include <libtransmission/api-compat.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/serializer.h>
#include <libtransmission/variant.h>

#include <giomm/converterinputstream.h>
#include <giomm/memoryinputstream.h>
#include <giomm/zlibdecompressor.h>
#include <libsoup/soup.h>

#include <fmt/format.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace transmission::client::gtk
{

namespace
{

int64_t next_request_id()
{
    static std::atomic<int64_t> id{ 1 };
    return id++;
}

[[nodiscard]] bool is_gzip_body(void const* data, size_t const len) noexcept
{
    return len >= 2U && static_cast<uint8_t const*>(data)[0] == 0x1f && static_cast<uint8_t const*>(data)[1] == 0x8b;
}

// libsoup returns gzip-compressed bodies; Qt's QNetworkReply decompresses automatically.
[[nodiscard]] std::optional<std::string> decode_http_body(void const* data, size_t const len)
{
    if (data == nullptr || len == 0U)
    {
        return std::nullopt;
    }

    if (!is_gzip_body(data, len))
    {
        return std::string{ static_cast<char const*>(data), len };
    }

    try
    {
        auto const bytes = Glib::Bytes::create(static_cast<gconstpointer>(data), len);
        auto const in = Gio::MemoryInputStream::create();
        in->add_bytes(bytes);
        auto const decompressor = Gio::ZlibDecompressor::create(Gio::ZlibCompressorFormat::GZIP);
        auto const converter = Gio::ConverterInputStream::create(in, decompressor);

        auto out = std::string{};
        auto buffer = std::array<char, 8192>{};

        while (true)
        {
            auto const n = converter->read(buffer.data(), buffer.size());
            if (n == 0)
            {
                break;
            }

            if (n < 0)
            {
                return std::nullopt;
            }

            out.append(buffer.data(), static_cast<size_t>(n));
        }

        return out;
    }
    catch (Glib::Error const&)
    {
        return std::nullopt;
    }
}

// Mirrors qt/RpcClient::parseResponseData()
[[nodiscard]] RpcResponse parse_response_data(tr_variant& response)
{
    auto ret = RpcResponse{};
    ret.success = false;
    ret.errmsg = "unknown error";

    if (auto* const response_map = response.get_if<tr_variant::Map>())
    {
        if (auto* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result))
        {
            ret.success = true;
            ret.errmsg.clear();
            ret.args = std::make_unique<tr_variant>(std::move(*result));
            return ret;
        }

        if (auto* const error_map = response_map->find_if<tr_variant::Map>(TR_KEY_error))
        {
            if (auto const msg = error_map->value_if<std::string_view>(TR_KEY_message))
            {
                ret.errmsg = std::string{ *msg };
            }

            if (auto* const data = error_map->find_if<tr_variant::Map>(TR_KEY_data))
            {
                if (auto const errstr = data->value_if<std::string_view>(TR_KEY_error_string))
                {
                    ret.errmsg = std::string{ *errstr };
                }

                if (auto* const result = data->find_if<tr_variant::Map>(TR_KEY_result))
                {
                    ret.args = std::make_unique<tr_variant>(std::move(*result));
                }
            }
        }
        else if (auto const result = response_map->value_if<std::string_view>(TR_KEY_result))
        {
            ret.errmsg = std::string{ *result };
        }
    }

    return ret;
}

} // namespace

RpcClient::RpcClient() = default;

RpcClient::~RpcClient()
{
    stop();
}

void RpcClient::stop()
{
    if (soup_session_ != nullptr)
    {
        g_object_unref(soup_session_);
        soup_session_ = nullptr;
    }

    session_ = nullptr;
    url_.clear();
    username_.clear();
    password_.clear();
    session_id_.clear();
    network_style_ = libtransmission::api_compat::Style::Tr4;
}

void RpcClient::start_local(tr_session* session)
{
    stop();
    session_ = session;
}

void RpcClient::start_remote(std::string_view url, std::string_view username, std::string_view password)
{
    stop();
    url_ = url;
    username_ = username;
    password_ = password;
    soup_session_ = soup_session_new();
}

void RpcClient::exec_async(tr_quark const method, tr_variant const& params, Callback callback)
{
    auto reqmap = tr_variant::Map{ 4U };
    reqmap.try_emplace(TR_KEY_jsonrpc, tr_variant::unmanaged_string(JsonRpc::Version));
    reqmap.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(method));
    if (params.has_value())
    {
        reqmap.try_emplace(TR_KEY_params, params.clone());
    }

    auto req = tr_variant{ std::move(reqmap) };
    if (session_ != nullptr)
    {
        exec_local(req, std::move(callback));
    }
    else
    {
        exec_remote(req, std::move(callback));
    }
}

void RpcClient::exec_async(tr_quark const method, Callback callback)
{
    exec_async(method, tr_variant{}, std::move(callback));
}

void RpcClient::exec_local(tr_variant& request, Callback callback)
{
    auto const id = next_request_id();
    if (auto* const map = request.get_if<tr_variant::Map>())
    {
        map->try_emplace(TR_KEY_id, id);
    }

    tr_rpc_request_exec(
        session_,
        request,
        [cb = std::move(callback)](tr_session* /*session*/, tr_variant&& response) mutable
        {
            auto owned = tr_variant{ std::move(response) };
            libtransmission::api_compat::convert_incoming_data(owned);
            cb(parse_response_data(owned));
        });
}

void RpcClient::exec_remote(tr_variant& request, Callback callback)
{
    auto const id = next_request_id();
    if (auto* const map = request.get_if<tr_variant::Map>())
    {
        map->try_emplace(TR_KEY_id, id);
    }

    libtransmission::api_compat::convert(request, network_style_);
    auto const body = tr_variant_serde::json().compact().to_string(request);

    auto response = RpcResponse{};
    if (!post_remote(body, response))
    {
        callback(std::move(response));
        return;
    }

    callback(std::move(response));
}

bool RpcClient::post_remote(std::string const& body, RpcResponse& response)
{
    if (url_.empty() || soup_session_ == nullptr)
    {
        response.errmsg = "no remote URL configured";
        return false;
    }

    auto* msg = soup_message_new(SOUP_METHOD_POST, url_.c_str());

    soup_message_headers_append(
        soup_message_get_request_headers(msg),
        "Content-Type",
        "application/json; charset=UTF-8");
    soup_message_headers_append(soup_message_get_request_headers(msg), "User-Agent", "transmission-client-gtk");

    if (!session_id_.empty())
    {
        soup_message_headers_append(soup_message_get_request_headers(msg), TR_RPC_SESSION_ID_HEADER, session_id_.c_str());
    }

    if (!username_.empty())
    {
        auto const cred = fmt::format("{}:{}", username_, password_);
        auto* const encoded = g_base64_encode(reinterpret_cast<guchar const*>(cred.data()), cred.size());
        auto const header = fmt::format("Basic {}", encoded);
        soup_message_headers_append(soup_message_get_request_headers(msg), "Authorization", header.c_str());
        g_free(encoded);
    }

    soup_message_set_request_body_from_bytes(msg, "application/json", g_bytes_new(body.data(), body.size()));

    GError* error = nullptr;
    GBytes* bytes = soup_session_send_and_read(soup_session_, msg, nullptr, &error);
    auto const status = soup_message_get_status(msg);

    if (error != nullptr)
    {
        response.errmsg = error->message;
        g_error_free(error);
        g_object_unref(msg);
        return false;
    }

    if (status == SOUP_STATUS_CONFLICT && soup_message_headers_get_one(soup_message_get_response_headers(msg), TR_RPC_SESSION_ID_HEADER))
    {
        session_id_ = soup_message_headers_get_one(soup_message_get_response_headers(msg), TR_RPC_SESSION_ID_HEADER);

        if (soup_message_headers_get_one(soup_message_get_response_headers(msg), TR_RPC_RPC_VERSION_HEADER))
        {
            network_style_ = libtransmission::api_compat::Style::Tr5;
        }
        else
        {
            network_style_ = libtransmission::api_compat::Style::Tr4;
        }

        if (bytes != nullptr)
        {
            g_bytes_unref(bytes);
        }

        g_object_unref(msg);
        return post_remote(body, response);
    }

    g_object_unref(msg);

    if (bytes == nullptr)
    {
        response.errmsg = "empty RPC response";
        return false;
    }

    if (status != SOUP_STATUS_OK)
    {
        response.errmsg = fmt::format("HTTP {}", static_cast<int>(status));
        g_bytes_unref(bytes);
        return false;
    }

    auto const data_len = g_bytes_get_size(bytes);
    auto const* data = static_cast<char const*>(g_bytes_get_data(bytes, nullptr));

    auto const decoded = decode_http_body(data, data_len);
    g_bytes_unref(bytes);

    if (!decoded)
    {
        response.errmsg = "failed to decode RPC response";
        return false;
    }

    auto const json = std::string_view{ *decoded };

    if (verbose_)
    {
        fmt::print(stderr, "RPC response:\n{}\n", json);
    }

    if (auto var = tr_variant_serde::json().parse(json))
    {
        libtransmission::api_compat::convert_incoming_data(*var);
        response = parse_response_data(*var);
        return true;
    }

    response.errmsg = "invalid JSON response";
    return false;
}

} // namespace transmission::client::gtk
