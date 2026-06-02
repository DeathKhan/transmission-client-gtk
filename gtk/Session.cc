// This file Copyright © Transmission authors and contributors.
// This file is licensed under the MIT (SPDX: MIT) license,
// A copy of this license can be found in licenses/ .

#include "Session.h"

#include "RemotePrefs.h"
#include "RpcClient.h"

#include "Actions.h"
#include "ListModelAdapter.h"
#include "Notify.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "SortListModel.hh"
#include "Torrent.h"
#include "TorrentSorter.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/crypto-utils.h>
#include <libtransmission/log.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/utils.h> // tr_time()
#include <libtransmission/variant.h>
#include <libtransmission/web-utils.h> // tr_urlIsValid()

#include <giomm/asyncresult.h>
#include <giomm/dbusconnection.h>
#include <giomm/fileinfo.h>
#include <giomm/filemonitor.h>
#include <giomm/liststore.h>
#include <glibmm/error.h>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/stringutils.h>
#include <glibmm/variant.h>

#if GTKMM_CHECK_VERSION(4, 0, 0)
#include <gtkmm/sortlistmodel.h>
#else
#include <gtkmm/treemodelsort.h>
#endif

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cinttypes> // PRId64
#include <cstring> // strstr
#include <functional>
#include <optional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace std::literals;
using namespace transmission::app;


class Session::Impl
{
public:
    Impl(Session& core, tr_session* session, bool force_remote = false);
    Impl& operator=(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl(Impl const&) = delete;
    ~Impl();

    tr_session* close();

    Glib::RefPtr<Gio::ListStore<Torrent>> get_raw_model() const;
    Glib::RefPtr<SortListModel<Torrent>> get_model();
    tr_session* get_session() const;

    std::pair<Glib::RefPtr<Torrent>, guint> find_torrent_by_id(tr_torrent_id_t torrent_id) const;

    size_t get_active_torrent_count() const;

    bool get_port_test_pending(PortTestIpProtocol ip_protocol);
    void set_port_test_pending(bool pending, PortTestIpProtocol ip_protocol);

    void update();
    void torrents_added();

    void add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify);
    void add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify);
    void add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify);
    bool add_from_url(Glib::ustring const& url);

    void remove_torrent(tr_torrent_id_t id, bool delete_files);

    void send_rpc_request(tr_quark method, tr_variant const& params, std::function<void(tr_variant&)> on_response);

    void commit_prefs_change(tr_quark key);

    auto& signal_add_error()
    {
        return signal_add_error_;
    }

    auto& signal_add_prompt()
    {
        return signal_add_prompt_;
    }

    auto& signal_blocklist_updated()
    {
        return signal_blocklist_updated_;
    }

    auto& signal_busy()
    {
        return signal_busy_;
    }

    auto& signal_prefs_changed()
    {
        return signal_prefs_changed_;
    }

    auto& signal_port_tested()
    {
        return signal_port_tested_;
    }

    auto& signal_torrents_changed()
    {
        return signal_torrents_changed_;
    }

    [[nodiscard]] constexpr auto& favicon_cache()
    {
        return favicon_cache_;
    }

    [[nodiscard]] bool is_remote() const noexcept
    {
        return is_remote_;
    }

    [[nodiscard]] bool get_remote_stats(tr_session_stats& current, tr_session_stats& cumulative) const noexcept;

    [[nodiscard]] int get_remote_blocklist_size() const noexcept;

    void init_remote();
    void refresh_remote_torrents();
    void merge_remote_torrents(std::vector<TorrentRpcSnapshot>&& rows);
    void refresh_remote_session_stats();
    void refresh_remote_session_prefs();
    void torrent_add_rpc(tr_variant::Map args, Glib::ustring const& display_name, bool do_notify);
    bool add_remote(Glib::ustring const& name, bool do_start, bool do_prompt, bool do_notify);
    void add_torrent_from_ctor(tr_ctor* ctor, bool do_start, bool delete_source, tr_priority_t priority);
    static tr_torrent_activity status_to_activity(int status);

    std::function<std::unordered_set<tr_torrent_id_t>()> get_selected_ids_;
    std::function<void(std::unordered_set<tr_torrent_id_t> const&)> restore_selected_ids_;

private:
    Glib::RefPtr<Session> get_core_ptr() const;

    bool is_busy() const;
    void add_to_busy(int addMe);
    void inc_busy();
    void dec_busy();

    bool add(Glib::ustring const& name_in, bool do_start, bool do_prompt, bool do_notify);
    void add_file_async_callback(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::AsyncResult>& result,
        tr_ctor* ctor,
        bool do_prompt,
        bool do_notify);

    Glib::RefPtr<Torrent> create_new_torrent(tr_ctor* ctor);

    void maybe_inhibit_hibernation();
    void set_hibernation_allowed(bool allowed);

    void watchdir_update();
    void watchdir_scan();
    void watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file);
    bool watchdir_idle();
    void on_file_changed_in_watchdir(
        Glib::RefPtr<Gio::File> const& file,
        Glib::RefPtr<Gio::File> const& other_type,
        IF_GLIBMM2_68(Gio::FileMonitor::Event, Gio::FileMonitorEvent) event_type);

    void on_pref_changed(tr_quark key);

    void on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running);
    void on_torrent_metadata_changed(tr_torrent* raw_torrent);

private:
    Session& core_;

    sigc::signal<void(ErrorCode, Glib::ustring const&)> signal_add_error_;
    sigc::signal<void(tr_ctor*)> signal_add_prompt_;
    sigc::signal<void(bool)> signal_blocklist_updated_;
    sigc::signal<void(bool)> signal_busy_;
    sigc::signal<void(tr_quark)> signal_prefs_changed_;
    sigc::signal<void(std::optional<bool>, PortTestIpProtocol)> signal_port_tested_;
    sigc::signal<void(std::unordered_set<tr_torrent_id_t> const&, Torrent::ChangeFlags)> signal_torrents_changed_;

    Glib::RefPtr<Gio::FileMonitor> monitor_;
    sigc::connection monitor_tag_;
    Glib::RefPtr<Gio::File> monitor_dir_;
    std::vector<Glib::RefPtr<Gio::File>> monitor_files_;
    sigc::connection monitor_idle_tag_;

    bool adding_from_watch_dir_ = false;
    bool inhibit_allowed_ = false;
    bool have_inhibit_cookie_ = false;
    bool dbus_error_ = false;
    std::array<bool, NUM_PORT_TEST_IP_PROTOCOL> port_test_pending_ = {};
    guint inhibit_cookie_ = 0;
    gint busy_count_ = 0;
    Glib::RefPtr<Gio::ListStore<Torrent>> raw_model_;
    Glib::RefPtr<SortListModel<Torrent>> sorted_model_;
    Glib::RefPtr<TorrentSorter> sorter_ = TorrentSorter::create();
    tr_session* session_ = nullptr;

    bool is_remote_ = false;
    bool applying_remote_prefs_ = false;
    int remote_blocklist_size_ = -1;
    std::unique_ptr<transmission::client::gtk::RpcClient> rpc_;
    sigc::connection remote_poll_tag_;
    tr_session_stats remote_stats_{};
    tr_session_stats remote_cumulative_stats_{};
    bool have_remote_stats_ = false;

    FaviconCache<Glib::RefPtr<Gdk::Pixbuf>> favicon_cache_;
};

Glib::RefPtr<Session> Session::Impl::get_core_ptr() const
{
    core_.reference();
    return Glib::make_refptr_for_instance(&core_);
}

/***
****
***/

Glib::RefPtr<Gio::ListStore<Torrent>> Session::Impl::get_raw_model() const
{
    return raw_model_;
}

Glib::RefPtr<Gio::ListModel> Session::get_model() const
{
    return impl_->get_raw_model();
}

Glib::RefPtr<Session::Model> Session::get_sorted_model() const
{
    return impl_->get_model();
}

Glib::RefPtr<SortListModel<Torrent>> Session::Impl::get_model()
{
    return sorted_model_;
}

tr_session* Session::get_session() const
{
    return impl_->get_session();
}

bool Session::is_remote() const noexcept
{
    return impl_->is_remote();
}

bool Session::Impl::get_remote_stats(tr_session_stats& current, tr_session_stats& cumulative) const noexcept
{
    if (!is_remote_ || !have_remote_stats_)
    {
        return false;
    }

    current = remote_stats_;
    cumulative = remote_cumulative_stats_;
    return true;
}

int Session::Impl::get_remote_blocklist_size() const noexcept
{
    return remote_blocklist_size_;
}

bool Session::get_remote_stats(tr_session_stats& current, tr_session_stats& cumulative) const noexcept
{
    return impl_->get_remote_stats(current, cumulative);
}

int Session::get_remote_blocklist_size() const noexcept
{
    return impl_->get_remote_blocklist_size();
}

void Session::get_remote_free_space(std::string const& path, std::function<void(int64_t)> callback)
{
    if (!impl_->is_remote() || !callback)
    {
        return;
    }

    auto params = tr_variant::Map{ 1U };
    params[TR_KEY_path] = path;

    impl_->send_rpc_request(
        TR_KEY_free_space,
        tr_variant{ std::move(params) },
        [cb = std::move(callback)](tr_variant& response) mutable
        {
            int64_t bytes = -1;
            if (auto const* const m = response.get_if<tr_variant::Map>())
            {
                bytes = m->value_if<int64_t>(TR_KEY_size_bytes).value_or(-1);
            }
            cb(bytes);
        });
}

void Session::refresh_remote_prefs()
{
    if (impl_->is_remote())
    {
        impl_->refresh_remote_session_prefs();
    }
}

void Session::fetch_magnet_link(tr_torrent_id_t const id, std::function<void(std::string const&)> callback)
{
    if (!impl_->is_remote() || !callback)
    {
        return;
    }

    auto fields = tr_variant::Vector{};
    fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(TR_KEY_magnet_link)));

    auto ids = tr_variant::Vector{};
    ids.emplace_back(id);

    auto args = tr_variant::Map{ 2U };
    args[TR_KEY_fields] = std::move(fields);
    args[TR_KEY_ids] = std::move(ids);

    impl_->send_rpc_request(
        TR_KEY_torrent_get,
        tr_variant{ std::move(args) },
        [cb = std::move(callback)](tr_variant& result) mutable
        {
            auto* const result_map = result.get_if<tr_variant::Map>();
            if (result_map == nullptr)
            {
                cb({});
                return;
            }

            auto* const torrents = result_map->find_if<tr_variant::Vector>(TR_KEY_torrents);
            if (torrents != nullptr && !std::empty(*torrents))
            {
                if (auto* const map = (*torrents)[0].get_if<tr_variant::Map>())
                {
                    if (auto const link = map->value_if<std::string_view>(TR_KEY_magnet_link))
                    {
                        cb(std::string{ *link });
                        return;
                    }
                }
            }

            cb({});
        });
}

tr_session* Session::Impl::get_session() const
{
    return session_;
}

/***
****  BUSY
***/

bool Session::Impl::is_busy() const
{
    return busy_count_ > 0;
}

void Session::Impl::add_to_busy(int addMe)
{
    bool const wasBusy = is_busy();

    busy_count_ += addMe;

    if (wasBusy != is_busy())
    {
        signal_busy_.emit(is_busy());
    }
}

void Session::Impl::inc_busy()
{
    add_to_busy(1);
}

void Session::Impl::dec_busy()
{
    add_to_busy(-1);
}

/***
****
****  WATCHDIR
****
***/

namespace
{

time_t get_file_mtime(Glib::RefPtr<Gio::File> const& file)
{
    try
    {
        return file->query_info(G_FILE_ATTRIBUTE_TIME_MODIFIED)->get_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED);
    }
    catch (Glib::Error const&)
    {
        return 0;
    }
}

void rename_torrent(Glib::RefPtr<Gio::File> const& file)
{
    auto info = Glib::RefPtr<Gio::FileInfo>();

    try
    {
        info = file->query_info(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
    }
    catch (Glib::Error const&)
    {
        return;
    }

    auto const old_name = info->get_attribute_as_string(G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);
    auto const new_name = fmt::format("{}.added", old_name);

    try
    {
        file->set_display_name(new_name);
    }
    catch (Glib::Error const& e)
    {
        gtr_message(
            fmt::format(
                fmt::runtime(_("Couldn't rename '{old_path}' as '{path}': {error} ({error_code})")),
                fmt::arg("old_path", old_name),
                fmt::arg("path", new_name),
                fmt::arg("error", e.what()),
                fmt::arg("error_code", e.code())));
    }
}

} // namespace

bool Session::Impl::watchdir_idle()
{
    std::vector<Glib::RefPtr<Gio::File>> changing;
    std::vector<Glib::RefPtr<Gio::File>> unchanging;
    time_t const now = tr_time();

    /* separate the files into two lists: changing and unchanging */
    for (auto const& file : monitor_files_)
    {
        time_t const mtime = get_file_mtime(file);

        if (mtime + 2 >= now)
        {
            changing.push_back(file);
        }
        else
        {
            unchanging.push_back(file);
        }
    }

    /* add the files that have stopped changing */
    if (!unchanging.empty())
    {
        bool const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
        bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);

        adding_from_watch_dir_ = true;
        add_files(unchanging, do_start, do_prompt, true);
        std::for_each(unchanging.begin(), unchanging.end(), rename_torrent);
        adding_from_watch_dir_ = false;
    }

    /* keep monitoring the ones that are still changing */
    monitor_files_ = changing;

    /* if monitor_files is nonempty, keep checking every second */
    if (!monitor_files_.empty())
    {
        return true;
    }

    monitor_idle_tag_.disconnect();
    return false;
}

/* If this file is a torrent, add it to our list */
void Session::Impl::watchdir_monitor_file(Glib::RefPtr<Gio::File> const& file)
{
    auto const filename = file->get_path();
    bool const is_torrent = Glib::str_has_suffix(filename, ".torrent");

    if (is_torrent)
    {
        /* if we're not already watching this file, start watching it now */
        bool const found = std::any_of(
            monitor_files_.begin(),
            monitor_files_.end(),
            [file](auto const& f) { return file->equal(f); });

        if (!found)
        {
            monitor_files_.push_back(file);

            if (!monitor_idle_tag_.connected())
            {
                monitor_idle_tag_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Impl::watchdir_idle), 1);
            }
        }
    }
}

/* GFileMonitor noticed a file was created */
void Session::Impl::on_file_changed_in_watchdir(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::File> const& /*other_type*/,
    IF_GLIBMM2_68(Gio::FileMonitor::Event, Gio::FileMonitorEvent) event_type)
{
    if (event_type == TR_GIO_FILE_MONITOR_EVENT(CREATED))
    {
        watchdir_monitor_file(file);
    }
}

/* walk through the pre-existing files in the watchdir */
void Session::Impl::watchdir_scan()
{
    auto const dirname = gtr_pref_string_get(TR_KEY_watch_dir);

    try
    {
        for (auto const& name : Glib::Dir(dirname))
        {
            watchdir_monitor_file(Gio::File::create_for_path(Glib::build_filename(dirname, name)));
        }
    }
    catch (Glib::FileError const&)
    {
    }
}

void Session::Impl::watchdir_update()
{
    bool const is_enabled = gtr_pref_flag_get(TR_KEY_watch_dir_enabled);
    auto const dir = Gio::File::create_for_path(gtr_pref_string_get(TR_KEY_watch_dir));

    if (monitor_ != nullptr && (!is_enabled || !dir->equal(monitor_dir_)))
    {
        monitor_tag_.disconnect();
        monitor_->cancel();

        monitor_dir_.reset();
        monitor_.reset();
    }

    if (!is_enabled || monitor_ != nullptr)
    {
        return;
    }

    auto monitor = Glib::RefPtr<Gio::FileMonitor>();

    try
    {
        monitor = dir->monitor_directory();
    }
    catch (Glib::Error const&)
    {
        return;
    }

    watchdir_scan();

    monitor_ = monitor;
    monitor_dir_ = dir;
    monitor_tag_ = monitor_->signal_changed().connect(sigc::mem_fun(*this, &Impl::on_file_changed_in_watchdir));
}

/***
****
***/

void Session::Impl::on_pref_changed(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_sort_mode:
        if (auto const sort_mode = gtr_pref_get<SortMode>(TR_KEY_sort_mode))
        {
            sorter_->set_mode(*sort_mode);
        }
        break;

    case TR_KEY_sort_reversed:
        sorter_->set_reversed(gtr_pref_flag_get(TR_KEY_sort_reversed));
        break;

    case TR_KEY_peer_limit_global:
        if (session_ != nullptr)
        {
            tr_sessionSetPeerLimit(session_, gtr_pref_int_get(key));
        }
        break;

    case TR_KEY_peer_limit_per_torrent:
        if (session_ != nullptr)
        {
            tr_sessionSetPeerLimitPerTorrent(session_, gtr_pref_int_get(key));
        }
        break;

    case TR_KEY_inhibit_desktop_hibernation:
        maybe_inhibit_hibernation();
        break;

    case TR_KEY_watch_dir:
    case TR_KEY_watch_dir_enabled:
        if (session_ != nullptr)
        {
            watchdir_update();
        }
        break;

    default:
        break;
    }
}

/**
***
**/

Glib::RefPtr<Session> Session::create(tr_session* session)
{
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return Glib::make_refptr_for_instance(new Session(session));
}

Glib::RefPtr<Session> Session::create_remote()
{
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto self = Glib::make_refptr_for_instance(new Session(true));
    self->impl_->init_remote();
    return self;
}

Session::Session(tr_session* session)
    : Glib::ObjectBase(typeid(Session))
    , impl_(std::make_unique<Impl>(*this, session))
{
}

Session::Session(bool const remote)
    : Glib::ObjectBase(typeid(Session))
    , impl_(std::make_unique<Impl>(*this, nullptr, remote))
{
}

Session::~Session() = default;

Session::Impl::Impl(Session& core, tr_session* session, bool const force_remote)
    : core_{ core }
    , session_{ session }
    , is_remote_{ force_remote }
{
    raw_model_ = Gio::ListStore<Torrent>::create();
    signal_torrents_changed_.connect(sigc::hide<0>(sigc::mem_fun(*sorter_, &TorrentSorter::update)));
    sorted_model_ = SortListModel<Torrent>::create(gtr_ptr_static_cast<Gio::ListModel>(raw_model_), sorter_);

    /* init from prefs & listen to pref changes */
    on_pref_changed(TR_KEY_sort_mode);
    on_pref_changed(TR_KEY_sort_reversed);
    on_pref_changed(TR_KEY_watch_dir_enabled);
    on_pref_changed(TR_KEY_peer_limit_global);
    on_pref_changed(TR_KEY_inhibit_desktop_hibernation);
    signal_prefs_changed_.connect([this](auto key) { on_pref_changed(key); });

    if (session != nullptr)
    {
        tr_sessionSetMetadataCallback(
            session,
            [](auto* /*session*/, auto* tor, gpointer impl) { static_cast<Impl*>(impl)->on_torrent_metadata_changed(tor); },
            this);

        tr_sessionSetCompletenessCallback(
            session,
            [](auto* tor, auto completeness, bool was_running, gpointer impl)
            { static_cast<Impl*>(impl)->on_torrent_completeness_changed(tor, completeness, was_running); },
            this);
    }
}

Session::Impl::~Impl()
{
    monitor_idle_tag_.disconnect();
}

tr_session* Session::close()
{
    return impl_->close();
}

tr_session* Session::Impl::close()
{
    auto* session = session_;

    if (session != nullptr)
    {
        session_ = nullptr;
        gtr_pref_save(session);
    }

    return session;
}

/***
****  COMPLETENESS CALLBACK
***/

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before calling notify's dbus code... */
void Session::Impl::on_torrent_completeness_changed(tr_torrent* tor, tr_completeness completeness, bool was_running)
{
    if (was_running && completeness != TR_LEECH && tr_torrentStat(tor)->sizeWhenDone != 0)
    {
        Glib::signal_idle().connect(
            [core = get_core_ptr(), torrent_id = tr_torrentId(tor)]()
            {
                gtr_notify_torrent_completed(core, torrent_id);
                return false;
            });
    }
}

/***
****  METADATA CALLBACK
***/

namespace
{

struct metadata_callback_data
{
    Session* core;
    tr_torrent_id_t torrent_id;
};

} // namespace

std::pair<Glib::RefPtr<Torrent>, guint> Session::Impl::find_torrent_by_id(tr_torrent_id_t torrent_id) const
{
    auto begin_position = 0U;
    auto end_position = raw_model_->get_n_items();

    while (begin_position < end_position)
    {
        auto const position = begin_position + (end_position - begin_position) / 2;
        auto const torrent = raw_model_->get_item(position);
        auto const current_torrent_id = torrent->get_id();

        if (current_torrent_id == torrent_id)
        {
            return { torrent, position };
        }

        if (current_torrent_id < torrent_id)
        {
            begin_position = position + 1;
        }
        else
        {
            end_position = position;
        }
    }

    return {};
}

/* this is called in the libtransmission thread, *NOT* the GTK+ thread,
   so delegate to the GTK+ thread before changing our list store... */
void Session::Impl::on_torrent_metadata_changed(tr_torrent* raw_torrent)
{
    Glib::signal_idle().connect(
        [this, core = get_core_ptr(), torrent_id = tr_torrentId(raw_torrent)]()
        {
            /* update the torrent's collated name */
            if (auto const& [torrent, position] = find_torrent_by_id(torrent_id); torrent)
            {
                torrent->update();
            }

            return false;
        });
}

/***
****
****  ADDING TORRENTS
****
***/

void Session::add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify)
{
    impl_->add_torrent(torrent, do_notify);
}

void Session::Impl::add_torrent(Glib::RefPtr<Torrent> const& torrent, bool do_notify)
{
    if (torrent != nullptr)
    {
        raw_model_->insert_sorted(torrent, &Torrent::compare_by_id);

        if (do_notify)
        {
            gtr_notify_torrent_added(get_core_ptr(), torrent->get_id());
        }
    }
}

Glib::RefPtr<Torrent> Session::Impl::create_new_torrent(tr_ctor* ctor)
{
    bool do_trash = false;

    /* let the gtk client handle the removal, since libT
     * doesn't have any concept of the glib trash API */
    tr_ctorGetDeleteSource(ctor, &do_trash);
    tr_ctorSetDeleteSource(ctor, false);
    tr_torrent* const tor = tr_torrentNew(ctor, nullptr);

    if (tor != nullptr && do_trash)
    {
        char const* config = tr_sessionGetConfigDir(session_);
        char const* source = tr_ctorGetSourceFile(ctor);

        if (source != nullptr)
        {
            /* #1294: don't delete the .torrent file if it's our internal copy */
            bool const is_internal = strstr(source, config) == source;

            if (!is_internal)
            {
                gtr_file_trash_or_remove(source, nullptr);
            }
        }
    }

    return Torrent::create(tor);
}

void Session::Impl::add_ctor(tr_ctor* ctor, bool do_prompt, bool do_notify)
{
    if (is_remote_)
    {
        if (do_prompt)
        {
            signal_add_prompt_.emit(ctor);
            return;
        }

        bool paused = false;
        (void)tr_ctorGetPaused(ctor, TR_FORCE, &paused);
        bool const do_start = !paused;

        if (auto const* const path = tr_ctorGetSourceFile(ctor))
        {
            (void)add_remote(path, do_start, false, do_notify);
        }

        tr_ctorFree(ctor);
        return;
    }

    auto const* metainfo = tr_ctorGetMetainfo(ctor);
    if (metainfo == nullptr)
    {
        return;
    }

    if (tr_torrentFindFromMetainfo(get_session(), metainfo) != nullptr)
    {
        /* don't complain about torrent files in the watch directory
         * that have already been added... that gets annoying and we
         * don't want to be nagging users to clean up their watch dirs */
        if (tr_ctorGetSourceFile(ctor) == nullptr || !adding_from_watch_dir_)
        {
            signal_add_error_.emit(ERR_ADD_TORRENT_DUP, metainfo->name().c_str());
        }

        tr_ctorFree(ctor);
        return;
    }

    if (!do_prompt)
    {
        add_torrent(create_new_torrent(ctor), do_notify);
        tr_ctorFree(ctor);
        return;
    }

    signal_add_prompt_.emit(ctor);
}

namespace
{

void core_apply_defaults(tr_ctor* ctor)
{
    if (!tr_ctorGetPaused(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPaused(ctor, TR_FORCE, !gtr_pref_flag_get(TR_KEY_start_added_torrents));
    }

    if (!tr_ctorGetDeleteSource(ctor, nullptr))
    {
        tr_ctorSetDeleteSource(ctor, gtr_pref_flag_get(TR_KEY_trash_original_torrent_files));
    }

    if (!tr_ctorGetPeerLimit(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetPeerLimit(ctor, TR_FORCE, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));
    }

    if (!tr_ctorGetDownloadDir(ctor, TR_FORCE, nullptr))
    {
        tr_ctorSetDownloadDir(ctor, TR_FORCE, gtr_pref_string_get(TR_KEY_download_dir).c_str());
    }
}

namespace
{

[[nodiscard]] tr_torrent_activity status_to_activity(int const status)
{
    switch (status)
    {
    case TR_STATUS_CHECK:
        return TR_STATUS_CHECK;
    case TR_STATUS_CHECK_WAIT:
        return TR_STATUS_CHECK_WAIT;
    case TR_STATUS_DOWNLOAD:
        return TR_STATUS_DOWNLOAD;
    case TR_STATUS_DOWNLOAD_WAIT:
        return TR_STATUS_DOWNLOAD_WAIT;
    case TR_STATUS_SEED:
        return TR_STATUS_SEED;
    case TR_STATUS_SEED_WAIT:
        return TR_STATUS_SEED_WAIT;
    default:
        return TR_STATUS_STOPPED;
    }
}

} // namespace

namespace remote_rpc
{

[[nodiscard]] std::vector<std::string> tracker_sitenames_from_map(tr_variant::Map const& map)
{
    auto sites = std::vector<std::string>{};
    auto* const stats = map.find_if<tr_variant::Vector>(TR_KEY_tracker_stats);
    if (stats == nullptr)
    {
        return sites;
    }

    for (auto const& entry : *stats)
    {
        auto* const tracker = entry.get_if<tr_variant::Map>();
        if (tracker == nullptr)
        {
            continue;
        }

        if (auto const site = tracker->value_if<std::string_view>(TR_KEY_sitename); site && !site->empty())
        {
            if (std::find(sites.begin(), sites.end(), *site) == sites.end())
            {
                sites.emplace_back(*site);
            }
        }
    }

    return sites;
}

[[nodiscard]] TorrentRpcSnapshot snapshot_from_map(tr_variant::Map const& map)
{
    auto row = TorrentRpcSnapshot{};
    row.id = map.value_if<int64_t>(TR_KEY_id).value_or(-1);
    row.name = map.value_if<std::string_view>(TR_KEY_name).value_or(""sv);
    row.activity = status_to_activity(static_cast<int>(map.value_if<int64_t>(TR_KEY_status).value_or(TR_STATUS_STOPPED)));
    row.percent_done = static_cast<float>(map.value_if<double>(TR_KEY_percent_done).value_or(0.0));
    row.rate_download = map.value_if<int64_t>(TR_KEY_rate_download).value_or(0);
    row.rate_upload = map.value_if<int64_t>(TR_KEY_rate_upload).value_or(0);
    row.error_code = static_cast<int>(map.value_if<int64_t>(TR_KEY_error).value_or(0));
    row.error_message = map.value_if<std::string_view>(TR_KEY_error_string).value_or(""sv);
    row.total_size = map.value_if<int64_t>(TR_KEY_total_size).value_or(0);
    row.finished = map.value_if<bool>(TR_KEY_is_finished).value_or(false);
    row.download_dir = map.value_if<std::string_view>(TR_KEY_download_dir).value_or(""sv);
    row.file_count = static_cast<int>(map.value_if<int64_t>(TR_KEY_file_count).value_or(0));
    row.left_until_done = map.value_if<int64_t>(TR_KEY_left_until_done).value_or(0);
    row.peers_connected = static_cast<uint16_t>(map.value_if<int64_t>(TR_KEY_peers_connected).value_or(0));
    row.peers_sending_to_us = static_cast<uint16_t>(map.value_if<int64_t>(TR_KEY_peers_sending_to_us).value_or(0));
    row.peers_getting_from_us = static_cast<uint16_t>(map.value_if<int64_t>(TR_KEY_peers_getting_from_us).value_or(0));
    row.webseeds_sending_to_us = static_cast<uint16_t>(map.value_if<int64_t>(TR_KEY_webseeds_sending_to_us).value_or(0));
    row.downloaded_ever = map.value_if<int64_t>(TR_KEY_downloaded_ever).value_or(0);
    row.uploaded_ever = map.value_if<int64_t>(TR_KEY_uploaded_ever).value_or(0);
    row.have_valid = map.value_if<int64_t>(TR_KEY_have_valid).value_or(0);
    row.have_unchecked = map.value_if<int64_t>(TR_KEY_have_unchecked).value_or(0);
    row.size_when_done = map.value_if<int64_t>(TR_KEY_size_when_done).value_or(0);
    row.eta = static_cast<time_t>(map.value_if<int64_t>(TR_KEY_eta).value_or(0));
    row.queue_position = static_cast<size_t>(map.value_if<int64_t>(TR_KEY_queue_position).value_or(0));
    row.recheck_progress = static_cast<float>(map.value_if<double>(TR_KEY_recheck_progress).value_or(0.0));
    row.metadata_percent_complete = static_cast<float>(map.value_if<double>(TR_KEY_metadata_percent_complete).value_or(1.0));
    row.seed_ratio_mode = static_cast<int>(map.value_if<int64_t>(TR_KEY_seed_ratio_mode).value_or(0));
    row.seed_ratio_limit = static_cast<float>(map.value_if<double>(TR_KEY_seed_ratio_limit).value_or(0.0));
    row.is_stalled = map.value_if<bool>(TR_KEY_is_stalled).value_or(false);
    row.tracker_sitenames = tracker_sitenames_from_map(map);
    return row;
}

[[nodiscard]] bool is_remote_session_pref(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_alt_speed_down:
    case TR_KEY_alt_speed_enabled:
    case TR_KEY_alt_speed_time_begin:
    case TR_KEY_alt_speed_time_day:
    case TR_KEY_alt_speed_time_enabled:
    case TR_KEY_alt_speed_time_end:
    case TR_KEY_alt_speed_up:
    case TR_KEY_blocklist_enabled:
    case TR_KEY_blocklist_url:
    case TR_KEY_default_trackers:
    case TR_KEY_dht_enabled:
    case TR_KEY_download_dir:
    case TR_KEY_download_queue_enabled:
    case TR_KEY_download_queue_size:
    case TR_KEY_encryption:
    case TR_KEY_idle_seeding_limit:
    case TR_KEY_idle_seeding_limit_enabled:
    case TR_KEY_incomplete_dir:
    case TR_KEY_incomplete_dir_enabled:
    case TR_KEY_lpd_enabled:
    case TR_KEY_peer_limit_global:
    case TR_KEY_peer_limit_per_torrent:
    case TR_KEY_peer_port:
    case TR_KEY_peer_port_random_on_start:
    case TR_KEY_pex_enabled:
    case TR_KEY_port_forwarding_enabled:
    case TR_KEY_queue_stalled_enabled:
    case TR_KEY_queue_stalled_minutes:
    case TR_KEY_ratio_limit:
    case TR_KEY_ratio_limit_enabled:
    case TR_KEY_rename_partial_files:
    case TR_KEY_script_torrent_done_enabled:
    case TR_KEY_script_torrent_done_filename:
    case TR_KEY_script_torrent_done_seeding_enabled:
    case TR_KEY_script_torrent_done_seeding_filename:
    case TR_KEY_seed_queue_enabled:
    case TR_KEY_seed_queue_size:
    case TR_KEY_speed_limit_down:
    case TR_KEY_speed_limit_down_enabled:
    case TR_KEY_speed_limit_up:
    case TR_KEY_speed_limit_up_enabled:
    case TR_KEY_start_added_torrents:
    case TR_KEY_trash_original_torrent_files:
    case TR_KEY_utp_enabled:
        return true;
    default:
        return false;
    }
}

void variant_to_pref(tr_quark const key, tr_variant const& val)
{
    if (auto const b = val.value_if<bool>())
    {
        if (key == TR_KEY_seed_ratio_limited)
        {
            gtr_pref_flag_set(TR_KEY_ratio_limit_enabled, *b);
        }
        else
        {
            gtr_pref_flag_set(key, *b);
        }
    }
    else if (auto const i = val.value_if<int64_t>())
    {
        if (key == TR_KEY_encryption)
        {
            gtr_pref_int_set(key, static_cast<int>(*i));
        }
        else
        {
            gtr_pref_int_set(key, *i);
        }
    }
    else if (auto const d = val.value_if<double>())
    {
        if (key == TR_KEY_seed_ratio_limit)
        {
            gtr_pref_double_set(TR_KEY_ratio_limit, *d);
        }
        else
        {
            gtr_pref_double_set(key, *d);
        }
    }
    else if (auto const s = val.value_if<std::string_view>())
    {
        if (key == TR_KEY_encryption)
        {
            if (*s == "required"sv)
            {
                gtr_pref_int_set(key, TR_ENCRYPTION_REQUIRED);
            }
            else if (*s == "tolerated"sv || *s == "allowed"sv)
            {
                gtr_pref_int_set(key, TR_ENCRYPTION_PREFERRED);
            }
            else
            {
                gtr_pref_int_set(key, TR_CLEAR_PREFERRED);
            }
        }
        else
        {
            gtr_pref_string_set(key, *s);
        }
    }
}

void apply_session_dict_to_prefs(tr_variant::Map const& dict)
{
    for (auto const& [key, val] : dict)
    {
        if (key == TR_KEY_seed_ratio_limited)
        {
            variant_to_pref(TR_KEY_seed_ratio_limited, val);
        }
        else if (key == TR_KEY_seed_ratio_limit)
        {
            variant_to_pref(TR_KEY_seed_ratio_limit, val);
        }
        else if (is_remote_session_pref(key))
        {
            variant_to_pref(key, val);
        }
    }
}

[[nodiscard]] std::optional<tr_variant> pref_to_variant(tr_quark const key)
{
    switch (key)
    {
    case TR_KEY_ratio_limit_enabled:
        return gtr_pref_flag_get(TR_KEY_ratio_limit_enabled);

    case TR_KEY_ratio_limit:
        return gtr_pref_double_get(TR_KEY_ratio_limit);

    case TR_KEY_encryption:
        switch (static_cast<tr_encryption_mode>(gtr_pref_int_get(key)))
        {
        case TR_ENCRYPTION_REQUIRED:
            return "required"sv;
        case TR_ENCRYPTION_PREFERRED:
            return "allowed"sv;
        default:
            return "preferred"sv;
        }

    case TR_KEY_alt_speed_enabled:
    case TR_KEY_alt_speed_time_enabled:
    case TR_KEY_blocklist_enabled:
    case TR_KEY_blocklist_updates_enabled:
    case TR_KEY_dht_enabled:
    case TR_KEY_download_queue_enabled:
    case TR_KEY_idle_seeding_limit_enabled:
    case TR_KEY_incomplete_dir_enabled:
    case TR_KEY_lpd_enabled:
    case TR_KEY_peer_port_random_on_start:
    case TR_KEY_pex_enabled:
    case TR_KEY_port_forwarding_enabled:
    case TR_KEY_queue_stalled_enabled:
    case TR_KEY_rename_partial_files:
    case TR_KEY_script_torrent_done_enabled:
    case TR_KEY_script_torrent_done_seeding_enabled:
    case TR_KEY_seed_queue_enabled:
    case TR_KEY_speed_limit_down_enabled:
    case TR_KEY_speed_limit_up_enabled:
    case TR_KEY_start_added_torrents:
    case TR_KEY_trash_original_torrent_files:
    case TR_KEY_utp_enabled:
        return gtr_pref_flag_get(key);

    case TR_KEY_alt_speed_down:
    case TR_KEY_alt_speed_time_begin:
    case TR_KEY_alt_speed_time_day:
    case TR_KEY_alt_speed_time_end:
    case TR_KEY_alt_speed_up:
    case TR_KEY_download_queue_size:
    case TR_KEY_idle_seeding_limit:
    case TR_KEY_peer_limit_global:
    case TR_KEY_peer_limit_per_torrent:
    case TR_KEY_peer_port:
    case TR_KEY_queue_stalled_minutes:
    case TR_KEY_seed_queue_size:
    case TR_KEY_speed_limit_down:
    case TR_KEY_speed_limit_up:
        return static_cast<int64_t>(gtr_pref_int_get(key));

    case TR_KEY_download_dir:
    case TR_KEY_incomplete_dir:
    case TR_KEY_default_trackers:
    case TR_KEY_blocklist_url:
    case TR_KEY_script_torrent_done_filename:
    case TR_KEY_script_torrent_done_seeding_filename:
        return gtr_pref_string_get(key);

    default:
        return {};
    }
}

[[nodiscard]] bool refreshes_torrent_list(tr_quark const method)
{
    switch (method)
    {
    case TR_KEY_torrent_get:
    case TR_KEY_session_get:
    case TR_KEY_session_stats:
    case TR_KEY_port_test:
    case TR_KEY_blocklist_update:
        return false;
    default:
        return true;
    }
}

void update_stats_from_dict(tr_variant::Map const& dict, tr_session_stats* stats)
{
    if (stats == nullptr)
    {
        return;
    }

    if (auto const v = dict.value_if<int64_t>(TR_KEY_uploaded_bytes))
    {
        stats->uploadedBytes = static_cast<uint64_t>(*v);
    }

    if (auto const v = dict.value_if<int64_t>(TR_KEY_downloaded_bytes))
    {
        stats->downloadedBytes = static_cast<uint64_t>(*v);
    }

    if (auto const v = dict.value_if<int64_t>(TR_KEY_files_added))
    {
        stats->filesAdded = static_cast<uint64_t>(*v);
    }

    if (auto const v = dict.value_if<int64_t>(TR_KEY_session_count))
    {
        stats->sessionCount = static_cast<uint64_t>(*v);
    }

    if (auto const v = dict.value_if<int64_t>(TR_KEY_seconds_active))
    {
        stats->secondsActive = static_cast<uint64_t>(*v);
    }

    stats->ratio = static_cast<float>(tr_getRatio(stats->uploadedBytes, stats->downloadedBytes));
}

} // namespace remote_rpc

} // namespace

void Session::add_ctor(tr_ctor* ctor)
{
    bool const do_notify = false;
    bool const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    core_apply_defaults(ctor);
    impl_->add_ctor(ctor, do_prompt, do_notify);
}

void Session::add_torrent_from_ctor(tr_ctor* const ctor, bool const do_start, bool const delete_source, tr_priority_t const priority)
{
    impl_->add_torrent_from_ctor(ctor, do_start, delete_source, priority);
}

/***
****
***/

void Session::Impl::add_file_async_callback(
    Glib::RefPtr<Gio::File> const& file,
    Glib::RefPtr<Gio::AsyncResult>& result,
    tr_ctor* ctor,
    bool do_prompt,
    bool do_notify)
{
    try
    {
        gsize length = 0;
        char* contents = nullptr;

        if (!file->load_contents_finish(result, contents, length))
        {
            gtr_message(fmt::format(fmt::runtime(_("Couldn't read '{path}'")), fmt::arg("path", file->get_parse_name())));
        }
        else if (is_remote_)
        {
            bool paused = false;
            if (ctor != nullptr)
            {
                (void)tr_ctorGetPaused(ctor, TR_FORCE, &paused);
                tr_ctorFree(ctor);
            }

            auto args = tr_variant::Map{ 2U };
            args[TR_KEY_paused] = paused;
            args[TR_KEY_metainfo] = tr_base64_encode({ contents, length });

            torrent_add_rpc(std::move(args), file->get_parse_name(), do_notify);
            g_free(contents);
        }
        else if (tr_ctorSetMetainfo(ctor, contents, length, nullptr))
        {
            add_ctor(ctor, do_prompt, do_notify);
        }
        else
        {
            tr_ctorFree(ctor);
        }
    }
    catch (Glib::Error const& e)
    {
        gtr_message(
            fmt::format(
                fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
                fmt::arg("path", file->get_parse_name()),
                fmt::arg("error", e.what()),
                fmt::arg("error_code", e.code())));
    }

    dec_busy();
}

// Add `name,` which might be a local filename, a magnet link, or a URI.
bool Session::Impl::add(Glib::ustring const& name_in, bool const do_start, bool const do_prompt, bool const do_notify)
{
    auto name = name_in;

    // `gio::File` doesn't seem to know how to stringify magnet links correctly.
    // Unfortunately there are some code paths that unavoidably use `gio::File`
    // e.g. Gtk::Application::on_open() so we have to do this:
    if (auto constexpr BrokenMagnetLinkPrefix = "magnet:///?"sv; tr_strv_starts_with(name.raw(), BrokenMagnetLinkPrefix))
    {
        name.replace(0, std::size(BrokenMagnetLinkPrefix), "magnet:?");
    }

    if (is_remote_)
    {
        return add_remote(name, do_start, do_prompt, do_notify);
    }

    auto* const session = get_session();
    if (session == nullptr)
    {
        return false;
    }

    bool handled = false;
    auto* ctor = tr_ctorNew(session);
    core_apply_defaults(ctor);
    tr_ctorSetPaused(ctor, TR_FORCE, !do_start);

    bool loaded = false;
    auto file = Gio::File::create_for_parse_name(name);
    if (auto const path = file->get_path(); !std::empty(path))
    {
        // try to treat it as a file...
        loaded = tr_ctorSetMetainfoFromFile(ctor, path.c_str(), nullptr);
    }

    if (!loaded)
    {
        // try to treat it as a magnet link...
        loaded = tr_ctorSetMetainfoFromMagnetLink(ctor, name.raw().c_str(), nullptr);
    }

    // if we could make sense of it, add it
    if (loaded)
    {
        handled = true;
        add_ctor(ctor, do_prompt, do_notify);
    }
    else if (tr_urlIsValid(file->get_uri()))
    {
        handled = true;
        inc_busy();
        file->load_contents_async([this, file, ctor, do_prompt, do_notify](auto& result)
                                  { add_file_async_callback(file, result, ctor, do_prompt, do_notify); });
    }
    else
    {
        tr_ctorFree(ctor);
        std::cerr << fmt::format(
                         fmt::runtime(_("Couldn't add torrent file '{path}'")),
                         fmt::arg("path", file->get_parse_name()))
                  << '\n';
    }

    return handled;
}

bool Session::add_from_url(Glib::ustring const& url)
{
    return impl_->add_from_url(url);
}

bool Session::Impl::add_from_url(Glib::ustring const& url)
{
    auto const do_start = gtr_pref_flag_get(TR_KEY_start_added_torrents);
    auto const do_prompt = gtr_pref_flag_get(TR_KEY_show_options_window);
    auto const do_notify = false;

    auto const handled = add(url, do_start, do_prompt, do_notify);
    torrents_added();
    return handled;
}

void Session::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    impl_->add_files(files, do_start, do_prompt, do_notify);
}

void Session::Impl::add_files(std::vector<Glib::RefPtr<Gio::File>> const& files, bool do_start, bool do_prompt, bool do_notify)
{
    for (auto const& file : files)
    {
        add(file->get_parse_name(), do_start, do_prompt, do_notify);
    }

    torrents_added();
}

void Session::torrents_added()
{
    impl_->torrents_added();
}

void Session::Impl::torrents_added()
{
    update();
    signal_add_error_.emit(ERR_NO_MORE_TORRENTS, {});
}

void Session::torrent_changed(tr_torrent_id_t id)
{
    if (auto const& [torrent, position] = impl_->find_torrent_by_id(id); torrent)
    {
        torrent->update();
    }
}

void Session::remove_torrent(tr_torrent_id_t id, bool delete_files)
{
    impl_->remove_torrent(id, delete_files);
}

void Session::Impl::remove_torrent(tr_torrent_id_t id, bool delete_files)
{
    if (auto const& [torrent, position] = find_torrent_by_id(id); torrent)
    {
        get_raw_model()->remove(position);

        if (is_remote())
        {
            auto ids = tr_variant::Vector{};
            ids.emplace_back(id);
            auto args = tr_variant::Map{ 2U };
            args[TR_KEY_ids] = std::move(ids);
            args[TR_KEY_delete_local_data] = delete_files;
            send_rpc_request(TR_KEY_torrent_remove, tr_variant{ std::move(args) }, {});
            return;
        }

        tr_torrentRemove(
            &torrent->get_underlying(),
            delete_files,
            [](char const* filename, void* /*user_data*/, tr_error* error)
            { return gtr_file_trash_or_remove(filename, error); },
            nullptr);
    }
}

void Session::load(bool force_paused)
{
    if (impl_->is_remote())
    {
        impl_->refresh_remote_torrents();
        return;
    }

    auto* const ctor = tr_ctorNew(impl_->get_session());

    if (force_paused)
    {
        tr_ctorSetPaused(ctor, TR_FORCE, true);
    }

    tr_ctorSetPeerLimit(ctor, TR_FALLBACK, gtr_pref_int_get(TR_KEY_peer_limit_per_torrent));

    auto* session = impl_->get_session();
    auto const n_torrents = tr_sessionLoadTorrents(session, ctor);
    tr_ctorFree(ctor);

    auto raw_torrents = std::vector<tr_torrent*>{};
    raw_torrents.resize(n_torrents);
    tr_sessionGetAllTorrents(session, std::data(raw_torrents), std::size(raw_torrents));

    auto torrents = std::vector<Glib::RefPtr<Torrent>>();
    torrents.reserve(raw_torrents.size());
    std::transform(raw_torrents.begin(), raw_torrents.end(), std::back_inserter(torrents), &Torrent::create);
    std::sort(torrents.begin(), torrents.end(), &Torrent::less_by_id);

    auto const model = impl_->get_raw_model();
    model->splice(0, model->get_n_items(), torrents);
}

void Session::clear()
{
    impl_->get_raw_model()->remove_all();
}

/***
****
***/

void Session::update()
{
    impl_->update();
}

void Session::start_now(tr_torrent_id_t const id)
{
    auto ids = tr_variant::Vector{};
    ids.emplace_back(id);
    auto params = tr_variant::Map{ 1U };
    params[TR_KEY_ids] = std::move(ids);
    exec(TR_KEY_torrent_start_now, tr_variant{ std::move(params) });
}

void Session::Impl::update()
{
    auto torrent_ids = std::unordered_set<tr_torrent_id_t>();
    auto changes = Torrent::ChangeFlags();

    /* update the model */
    for (auto i = 0U, count = raw_model_->get_n_items(); i < count; ++i)
    {
        auto const torrent = raw_model_->get_item(i);
        if (auto const torrent_changes = torrent->update(); torrent_changes.any())
        {
            torrent_ids.insert(torrent->get_id());
            changes |= torrent_changes;
        }
    }

    /* update hibernation */
    maybe_inhibit_hibernation();

    if (changes.any())
    {
        signal_torrents_changed_.emit(torrent_ids, changes);
    }
}

/**
***  Hibernate
**/

namespace
{

auto const SessionManagerServiceName = "org.gnome.SessionManager"sv; // TODO(C++20): Use ""s
auto const SessionManagerInterface = "org.gnome.SessionManager"sv; // TODO(C++20): Use ""s
auto const SessionManagerObjectPath = "/org/gnome/SessionManager"sv; // TODO(C++20): Use ""s

bool gtr_inhibit_hibernation(guint32& cookie)
{
    bool success = false;
    char const* application = "Transmission BitTorrent Client";
    char const* reason = "BitTorrent Activity";
    int const toplevel_xid = 0;
    int const flags = 4; /* Inhibit suspending the session or computer */

    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(TR_GIO_DBUS_BUS_TYPE(SESSION));

        auto response = connection->call_sync(
            std::string(SessionManagerObjectPath),
            std::string(SessionManagerInterface),
            "Inhibit",
            Glib::VariantContainerBase::create_tuple(
                {
                    Glib::Variant<Glib::ustring>::create(application),
                    Glib::Variant<guint32>::create(toplevel_xid),
                    Glib::Variant<Glib::ustring>::create(reason),
                    Glib::Variant<guint32>::create(flags),
                }),
            std::string(SessionManagerServiceName),
            1000);

        cookie = Glib::VariantBase::cast_dynamic<Glib::Variant<guint32>>(response.get_child(0)).get();

        /* logging */
        tr_logAddInfo(_("Inhibiting desktop hibernation"));

        success = true;
    }
    catch (Glib::Error const& e)
    {
        tr_logAddError(
            fmt::format(fmt::runtime(_("Couldn't inhibit desktop hibernation: {error}")), fmt::arg("error", e.what())));
    }

    return success;
}

void gtr_uninhibit_hibernation(guint inhibit_cookie)
{
    try
    {
        auto const connection = Gio::DBus::Connection::get_sync(TR_GIO_DBUS_BUS_TYPE(SESSION));

        connection->call_sync(
            std::string(SessionManagerObjectPath),
            std::string(SessionManagerInterface),
            "Uninhibit",
            Glib::VariantContainerBase::create_tuple({ Glib::Variant<guint32>::create(inhibit_cookie) }),
            std::string(SessionManagerServiceName),
            1000);

        /* logging */
        tr_logAddInfo(_("Allowing desktop hibernation"));
    }
    catch (Glib::Error const& e)
    {
        tr_logAddError(
            fmt::format(fmt::runtime(_("Couldn't inhibit desktop hibernation: {error}")), fmt::arg("error", e.what())));
    }
}

} // namespace

void Session::Impl::set_hibernation_allowed(bool allowed)
{
    inhibit_allowed_ = allowed;

    if (allowed && have_inhibit_cookie_)
    {
        gtr_uninhibit_hibernation(inhibit_cookie_);
        have_inhibit_cookie_ = false;
    }

    if (!allowed && !have_inhibit_cookie_ && !dbus_error_)
    {
        if (gtr_inhibit_hibernation(inhibit_cookie_))
        {
            have_inhibit_cookie_ = true;
        }
        else
        {
            dbus_error_ = true;
        }
    }
}

void Session::Impl::maybe_inhibit_hibernation()
{
    /* hibernation is allowed if EITHER
     * (a) the "inhibit" pref is turned off OR
     * (b) there aren't any active torrents */
    bool const hibernation_allowed = !gtr_pref_flag_get(TR_KEY_inhibit_desktop_hibernation) || get_active_torrent_count() == 0;
    set_hibernation_allowed(hibernation_allowed);
}

/**
***  Prefs
**/

void Session::Impl::commit_prefs_change(tr_quark const key)
{
    signal_prefs_changed_.emit(key);
    if (is_remote())
    {
        gtr_pref_save_client_only();

        if (!applying_remote_prefs_ && rpc_ && remote_rpc::is_remote_session_pref(key))
        {
            auto args = tr_variant::Map{ 1U };
            if (key == TR_KEY_ratio_limit_enabled)
            {
                args[TR_KEY_seed_ratio_limited] = gtr_pref_flag_get(key);
            }
            else if (key == TR_KEY_ratio_limit)
            {
                args[TR_KEY_seed_ratio_limit] = gtr_pref_double_get(key);
            }
            else if (auto val = remote_rpc::pref_to_variant(key); val.has_value())
            {
                args.try_emplace(key, std::move(*val));
            }

            if (!args.empty())
            {
                send_rpc_request(TR_KEY_session_set, tr_variant{ std::move(args) }, {});
            }
        }

        return;
    }

    gtr_pref_save(session_);
}

void Session::set_pref(tr_quark const key, std::string const& newval)
{
    if (newval != gtr_pref_string_get(key))
    {
        gtr_pref_string_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, bool newval)
{
    if (newval != gtr_pref_flag_get(key))
    {
        gtr_pref_flag_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, int newval)
{
    if (newval != gtr_pref_int_get(key))
    {
        gtr_pref_int_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

void Session::set_pref(tr_quark const key, double newval)
{
    if (std::fabs(newval - gtr_pref_double_get(key)) >= 0.0001)
    {
        gtr_pref_double_set(key, newval);
        impl_->commit_prefs_change(key);
    }
}

/***
****
****  RPC Interface
****
***/

namespace
{

int64_t nextId = 1;

bool const verbose_ = tr_env_key_exists("TR_RPC_VERBOSE");

std::map<int64_t, std::function<void(tr_variant&)>> pendingRequests;

bool core_read_rpc_response_idle(tr_variant& response)
{
    if (verbose_)
    {
        fmt::print("{:s}:{:d} got response:\n{:s}\n", __FILE__, __LINE__, tr_variant_serde::json().to_string(response));
    }

    if (auto const* resmap = response.get_if<tr_variant::Map>())
    {
        if (auto const id = resmap->value_if<int64_t>(TR_KEY_id))
        {
            if (auto const nh = pendingRequests.extract(*id))
            {
                nh.mapped()(response);
            }
            else
            {
                gtr_warning(fmt::format(fmt::runtime(_("Couldn't find pending RPC request for id {id}")), fmt::arg("id", *id)));
            }
        }
    }

    return false;
}

void core_read_rpc_response(tr_session* /*session*/, tr_variant&& response)
{
    auto owned_response = std::make_shared<tr_variant>(std::move(response));
    Glib::signal_idle().connect([owned_response]() mutable { return core_read_rpc_response_idle(*owned_response); });
}

} // namespace

void Session::Impl::init_remote()
{
    rpc_ = std::make_unique<transmission::client::gtk::RpcClient>();

    auto const url = transmission::client::gtk::build_remote_rpc_url();
    auto user = gtr_pref_string_get(TR_KEY_remote_session_username);
    auto pass = gtr_pref_string_get(TR_KEY_remote_session_password);

    rpc_->start_remote(url, user, pass);
    refresh_remote_torrents();

    refresh_remote_session_stats();
    refresh_remote_session_prefs();

    remote_poll_tag_ = Glib::signal_timeout().connect_seconds(
        [this]()
        {
            refresh_remote_torrents();
            refresh_remote_session_stats();
            return true;
        },
        3);
}

tr_torrent_activity Session::Impl::status_to_activity(int const status)
{
    return ::status_to_activity(status);
}

void Session::Impl::torrent_add_rpc(tr_variant::Map args, Glib::ustring const& display_name, bool const do_notify)
{
    if (!is_remote() || !rpc_)
    {
        return;
    }

    rpc_->exec_async(
        TR_KEY_torrent_add,
        tr_variant{ std::move(args) },
        [this, display_name, do_notify](transmission::client::gtk::RpcResponse&& response)
        {
            if (!response.success || !response.args)
            {
                if (!response.errmsg.empty())
                {
                    signal_add_error_.emit(ERR_ADD_TORRENT_ERR, response.errmsg);
                }
                return;
            }

            auto* const result = response.args->get_if<tr_variant::Map>();
            if (result == nullptr)
            {
                return;
            }

            if (result->contains(TR_KEY_torrent_added))
            {
                refresh_remote_torrents();

                if (do_notify && result->contains(TR_KEY_torrent_added))
                {
                    if (auto const* const added = result->find_if<tr_variant::Map>(TR_KEY_torrent_added))
                    {
                        if (auto const id = added->value_if<int64_t>(TR_KEY_id))
                        {
                            gtr_notify_torrent_added(get_core_ptr(), static_cast<tr_torrent_id_t>(*id));
                        }
                    }
                }
            }
            else if (result->contains(TR_KEY_torrent_duplicate))
            {
                auto const name = display_name.empty() ? Glib::ustring{ _("Torrent") } : display_name;
                signal_add_error_.emit(ERR_ADD_TORRENT_DUP, name);
                refresh_remote_torrents();
            }
        });
}

bool Session::Impl::add_remote(Glib::ustring const& name, bool const do_start, bool /*do_prompt*/, bool const do_notify)
{
    if (!is_remote() || !rpc_)
    {
        return false;
    }

    auto args = tr_variant::Map{ 2U };
    args[TR_KEY_paused] = !do_start;

    auto const name_std = name.raw();

    if (tr_magnet_metainfo{}.parseMagnet(name_std))
    {
        args[TR_KEY_filename] = name_std;
        torrent_add_rpc(std::move(args), name, do_notify);
        return true;
    }

    auto file = Gio::File::create_for_parse_name(name);
    if (auto const path = file->get_path(); !std::empty(path))
    {
        try
        {
            char* contents = nullptr;
            gsize len = 0;

            if (!file->load_contents(contents, len))
            {
                gtr_message(fmt::format(fmt::runtime(_("Couldn't read '{path}'")), fmt::arg("path", path)));
                return false;
            }

            args[TR_KEY_metainfo] = tr_base64_encode({ contents, len });
            g_free(contents);
            torrent_add_rpc(std::move(args), name, do_notify);
            return true;
        }
        catch (Glib::Error const& e)
        {
            gtr_message(
                fmt::format(
                    fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
                    fmt::arg("path", path),
                    fmt::arg("error", e.what()),
                    fmt::arg("error_code", e.code())));
            return false;
        }
    }

    if (tr_urlIsValid(file->get_uri()))
    {
        inc_busy();
        auto args_holder = std::make_shared<tr_variant::Map>(std::move(args));
        file->load_contents_async(
            [this, file, args_holder, do_notify](Glib::RefPtr<Gio::AsyncResult>& result)
            {
                try
                {
                    gsize len = 0;
                    char* data = nullptr;
                    if (file->load_contents_finish(result, data, len))
                    {
                        (*args_holder)[TR_KEY_metainfo] = tr_base64_encode({ data, len });
                        torrent_add_rpc(std::move(*args_holder), file->get_parse_name(), do_notify);
                    }

                    g_free(data);
                }
                catch (Glib::Error const& e)
                {
                    gtr_message(
                        fmt::format(
                            fmt::runtime(_("Couldn't read '{path}': {error}")),
                            fmt::arg("path", file->get_parse_name()),
                            fmt::arg("error", e.what())));
                }

                dec_busy();
            });
        return true;
    }

    return false;
}

void Session::Impl::add_torrent_from_ctor(tr_ctor* const ctor, bool const do_start, bool const delete_source, tr_priority_t const priority)
{
    if (!is_remote() || !rpc_ || ctor == nullptr)
    {
        return;
    }

    auto args = tr_variant::Map{ 4U };
    args[TR_KEY_paused] = !do_start;
    args[TR_KEY_bandwidth_priority] = priority;

    char const* download_dir = nullptr;
    if (tr_ctorGetDownloadDir(ctor, TR_FORCE, &download_dir) && download_dir != nullptr && download_dir[0] != '\0')
    {
        args[TR_KEY_download_dir] = std::string{ download_dir };
    }

    uint16_t peer_limit = 0;
    if (tr_ctorGetPeerLimit(ctor, TR_FORCE, &peer_limit) && peer_limit > 0)
    {
        args[TR_KEY_peer_limit] = static_cast<int64_t>(peer_limit);
    }

    Glib::ustring display_name;

    if (auto const* const path = tr_ctorGetSourceFile(ctor); path != nullptr && path[0] != '\0')
    {
        display_name = path;
        auto file = Gio::File::create_for_path(path);
        try
        {
            char* contents = nullptr;
            gsize len = 0;
            if (file->load_contents(contents, len))
            {
                args[TR_KEY_metainfo] = tr_base64_encode({ contents, len });
                g_free(contents);
            }
        }
        catch (Glib::Error const& e)
        {
            gtr_message(fmt::format(fmt::runtime(_("Couldn't read '{path}': {error}")), fmt::arg("path", path), fmt::arg("error", e.what())));
            return;
        }
    }
    if (!args.contains(TR_KEY_metainfo))
    {
        if (auto const* const metainfo = tr_ctorGetMetainfo(ctor); metainfo != nullptr)
        {
            display_name = metainfo->name();
        }
        return;
    }

    torrent_add_rpc(std::move(args), display_name, true);

    if (delete_source)
    {
        if (auto const* const path = tr_ctorGetSourceFile(ctor); path != nullptr)
        {
            gtr_file_trash_or_remove(path, nullptr);
        }
    }
}

void Session::Impl::refresh_remote_session_prefs()
{
    if (!is_remote() || !rpc_)
    {
        return;
    }

    rpc_->exec_async(
        TR_KEY_session_get,
        tr_variant{},
        [this](transmission::client::gtk::RpcResponse&& response)
        {
            if (!response.success || !response.args)
            {
                return;
            }

            auto* const dict = response.args->get_if<tr_variant::Map>();
            if (dict == nullptr)
            {
                return;
            }

            applying_remote_prefs_ = true;
            remote_rpc::apply_session_dict_to_prefs(*dict);

            if (auto const size = dict->value_if<int64_t>(TR_KEY_blocklist_size))
            {
                remote_blocklist_size_ = static_cast<int>(*size);
            }

            applying_remote_prefs_ = false;
            signal_prefs_changed_.emit(TR_KEY_NONE);
            signal_blocklist_updated_.emit(remote_blocklist_size_ >= 0);
        });
}

void Session::Impl::refresh_remote_session_stats()
{
    if (!is_remote() || !rpc_)
    {
        return;
    }

    rpc_->exec_async(
        TR_KEY_session_stats,
        tr_variant{},
        [this](transmission::client::gtk::RpcResponse&& response)
        {
            if (!response.success || !response.args)
            {
                return;
            }

            auto* const dict = response.args->get_if<tr_variant::Map>();
            if (dict == nullptr)
            {
                return;
            }

            if (auto* const current = dict->find_if<tr_variant::Map>(TR_KEY_current_stats))
            {
                remote_rpc::update_stats_from_dict(*current, &remote_stats_);
            }

            if (auto* const cumulative = dict->find_if<tr_variant::Map>(TR_KEY_cumulative_stats))
            {
                remote_rpc::update_stats_from_dict(*cumulative, &remote_cumulative_stats_);
            }

            have_remote_stats_ = true;
            signal_torrents_changed_.emit({}, Torrent::ChangeFlags{});
        });
}

void Session::Impl::merge_remote_torrents(std::vector<TorrentRpcSnapshot>&& rows)
{
    std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.id < b.id; });

    auto const selected_before = get_selected_ids_ ? get_selected_ids_() : std::unordered_set<tr_torrent_id_t>{};

    auto existing_by_id = std::unordered_map<tr_torrent_id_t, Glib::RefPtr<Torrent>>{};
    for (guint i = 0, n = raw_model_->get_n_items(); i < n; ++i)
    {
        auto const torrent = raw_model_->get_item(i);
        existing_by_id.emplace(torrent->get_id(), torrent);
    }

    auto new_ids = std::unordered_set<tr_torrent_id_t>{};
    new_ids.reserve(rows.size());
    for (auto const& row : rows)
    {
        new_ids.insert(row.id);
    }

    bool structure_changed = false;

    for (int i = static_cast<int>(raw_model_->get_n_items()) - 1; i >= 0; --i)
    {
        if (new_ids.find(raw_model_->get_item(static_cast<guint>(i))->get_id()) == new_ids.end())
        {
            raw_model_->splice(static_cast<guint>(i), 1, {});
            structure_changed = true;
        }
    }

    auto torrent_ids = std::unordered_set<tr_torrent_id_t>{};
    auto changes = Torrent::ChangeFlags{};

    for (auto const& row : rows)
    {
        if (auto it = existing_by_id.find(row.id); it != existing_by_id.end())
        {
            auto const row_changes = it->second->update_from_rpc(row);

            if (row_changes.any())
            {
                torrent_ids.insert(row.id);
                changes |= row_changes;
            }
        }
        else
        {
            auto const torrent = Torrent::create_from_rpc(row);

            guint insert_pos = 0;
            for (guint i = 0, n = raw_model_->get_n_items(); i < n; ++i)
            {
                if (raw_model_->get_item(i)->get_id() < row.id)
                {
                    insert_pos = i + 1;
                }
                else
                {
                    break;
                }
            }

            raw_model_->splice(insert_pos, 0, { torrent });
            structure_changed = true;
        }
    }

    if (structure_changed && restore_selected_ids_ && !selected_before.empty())
    {
        restore_selected_ids_(selected_before);
    }

    if (changes.any())
    {
        signal_torrents_changed_.emit(torrent_ids, changes);
    }
    else if (structure_changed)
    {
        signal_torrents_changed_.emit({}, Torrent::ChangeFlags{});
    }
}

void Session::Impl::refresh_remote_torrents()
{
    if (!is_remote() || !rpc_)
    {
        return;
    }

    auto fields = tr_variant::Vector{};
    fields.reserve(28);
    for (auto const key :
         { TR_KEY_id,
           TR_KEY_name,
           TR_KEY_status,
           TR_KEY_percent_done,
           TR_KEY_rate_download,
           TR_KEY_rate_upload,
           TR_KEY_error,
           TR_KEY_error_string,
           TR_KEY_total_size,
           TR_KEY_is_finished,
           TR_KEY_download_dir,
           TR_KEY_file_count,
           TR_KEY_left_until_done,
           TR_KEY_peers_connected,
           TR_KEY_peers_sending_to_us,
           TR_KEY_peers_getting_from_us,
           TR_KEY_webseeds_sending_to_us,
           TR_KEY_downloaded_ever,
           TR_KEY_uploaded_ever,
           TR_KEY_have_valid,
           TR_KEY_have_unchecked,
           TR_KEY_size_when_done,
           TR_KEY_eta,
           TR_KEY_queue_position,
           TR_KEY_recheck_progress,
           TR_KEY_metadata_percent_complete,
           TR_KEY_seed_ratio_mode,
           TR_KEY_seed_ratio_limit,
           TR_KEY_is_stalled,
           TR_KEY_tracker_stats })
    {
        fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
    }

    auto args = tr_variant::Map{ 1U };
    args[TR_KEY_fields] = std::move(fields);

    rpc_->exec_async(
        TR_KEY_torrent_get,
        tr_variant{ std::move(args) },
        [this](transmission::client::gtk::RpcResponse&& response)
        {
            if (!response.success || !response.args)
            {
                if (!response.errmsg.empty())
                {
                    gtr_warning(response.errmsg);
                }
                return;
            }

            auto* const result = response.args->get_if<tr_variant::Map>();
            if (result == nullptr)
            {
                return;
            }

            auto* const torrents = result->find_if<tr_variant::Vector>(TR_KEY_torrents);
            if (torrents == nullptr)
            {
                return;
            }

            auto rows = std::vector<TorrentRpcSnapshot>{};
            rows.reserve(torrents->size());

            for (auto const& entry : *torrents)
            {
                auto* const map = entry.get_if<tr_variant::Map>();
                if (map == nullptr)
                {
                    continue;
                }

                rows.push_back(remote_rpc::snapshot_from_map(*map));
            }

            Glib::signal_idle().connect_once(
                [this, rows = std::move(rows)]() mutable
                {
                    merge_remote_torrents(std::move(rows));
                });
        });
}

void Session::set_selection_helpers(
    std::function<std::unordered_set<tr_torrent_id_t>()> get_selected,
    std::function<void(std::unordered_set<tr_torrent_id_t> const&)> restore_selected)
{
    impl_->get_selected_ids_ = std::move(get_selected);
    impl_->restore_selected_ids_ = std::move(restore_selected);
}

void Session::Impl::send_rpc_request(
    tr_quark const method,
    tr_variant const& params,
    std::function<void(tr_variant&)> on_response)
{
    if (is_remote() && rpc_)
    {
        rpc_->exec_async(
            method,
            params,
            [this, method, cb = std::move(on_response)](transmission::client::gtk::RpcResponse&& response) mutable
            {
                if (response.success && response.args)
                {
                    if (cb)
                    {
                        cb(*response.args);
                    }

                    if (remote_rpc::refreshes_torrent_list(method))
                    {
                        refresh_remote_torrents();
                    }
                }
                else if (!response.errmsg.empty())
                {
                    gtr_warning(response.errmsg);
                }
            });
        return;
    }

    if (session_ == nullptr)
    {
        gtr_warning("No Transmission session available.");
        return;
    }

    // build the jsonrpc request
    auto reqmap = tr_variant::Map{ 4U };
    reqmap.try_emplace(TR_KEY_jsonrpc, tr_variant::unmanaged_string(JsonRpc::Version));
    reqmap.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(method));

    // add params if there are any
    if (params.has_value())
    {
        reqmap.try_emplace(TR_KEY_params, params.clone());
    }

    // add id if we want a response
    auto callback = std::function<void(tr_session*, tr_variant&&)>{};
    if (on_response)
    {
        auto const id = nextId++;
        pendingRequests.try_emplace(id, std::move(on_response));
        reqmap.try_emplace(TR_KEY_id, id);
        callback = core_read_rpc_response;
    }

    auto req = tr_variant{ std::move(reqmap) };

    if (verbose_)
    {
        fmt::print("{:s}:{:d} sending req:\n{:s}\n", __FILE__, __LINE__, tr_variant_serde::json().to_string(req));
    }

    tr_rpc_request_exec(session_, req, std::move(callback));
}

/***
****  Sending a test-port request via RPC
***/

void Session::port_test(PortTestIpProtocol const ip_protocol)
{
    static auto constexpr IpStr = std::array{ "ipv4"sv, "ipv6"sv };

    if (port_test_pending(ip_protocol))
    {
        return;
    }
    impl_->set_port_test_pending(true, ip_protocol);

    auto params = tr_variant::Map{ 1U };
    params.try_emplace(TR_KEY_ip_protocol, tr_variant::unmanaged_string(IpStr[ip_protocol]));

    impl_->send_rpc_request(
        TR_KEY_port_test,
        std::move(params),
        [this, ip_protocol](tr_variant& response)
        {
            impl_->set_port_test_pending(false, ip_protocol);

            auto is_open = std::optional<bool>();

            if (auto const* const resmap = response.get_if<tr_variant::Map>())
            {
                is_open = resmap->value_if<bool>(TR_KEY_port_is_open);
            }

            // If for whatever reason the status optional is empty here,
            // then something must have gone wrong with the port test,
            // so the UI should show the "error" state
            impl_->signal_port_tested().emit(is_open, ip_protocol);
        });
}

bool Session::port_test_pending(Session::PortTestIpProtocol ip_protocol) const noexcept
{
    return impl_->get_port_test_pending(ip_protocol);
}

bool Session::Impl::get_port_test_pending(Session::PortTestIpProtocol ip_protocol)
{
    return ip_protocol < NUM_PORT_TEST_IP_PROTOCOL && port_test_pending_[ip_protocol];
}

void Session::Impl::set_port_test_pending(bool pending, Session::PortTestIpProtocol ip_protocol)
{
    if (ip_protocol < NUM_PORT_TEST_IP_PROTOCOL)
    {
        port_test_pending_[ip_protocol] = pending;
    }
}

/***
****  Updating a blocklist via RPC
***/

void Session::blocklist_update()
{
    impl_->send_rpc_request(
        TR_KEY_blocklist_update,
        tr_variant{}, // no params
        [this](tr_variant& response)
        {
            std::optional<int64_t> n_rules;

            if (auto const* const resmap = response.get_if<tr_variant::Map>())
            {
                n_rules = resmap->value_if<int64_t>(TR_KEY_blocklist_size);
            }

            if (n_rules.has_value())
            {
                gtr_pref_int_set(TR_KEY_blocklist_date, tr_time());
            }

            impl_->signal_blocklist_updated().emit(n_rules >= 0);
        });
}

// ---

void Session::exec(tr_quark method, tr_variant const& params)
{
    impl_->send_rpc_request(method, params, {});
}

/***
****
***/

size_t Session::get_torrent_count() const
{
    return impl_->get_raw_model()->get_n_items();
}

size_t Session::get_active_torrent_count() const
{
    return impl_->get_active_torrent_count();
}

size_t Session::Impl::get_active_torrent_count() const
{
    size_t activeCount = 0;

    for (auto i = 0U, count = raw_model_->get_n_items(); i < count; ++i)
    {
        if (raw_model_->get_item(i)->get_activity() != TR_STATUS_STOPPED)
        {
            ++activeCount;
        }
    }

    return activeCount;
}

tr_torrent* Session::find_torrent(tr_torrent_id_t id) const
{
    tr_torrent* tor = nullptr;

    if (auto* const session = impl_->get_session(); session != nullptr)
    {
        tor = tr_torrentFindFromId(session, id);
    }

    return tor;
}

Glib::RefPtr<Torrent> Session::find_torrent_ref(tr_torrent_id_t const id) const
{
    auto const& [torrent, position] = impl_->find_torrent_by_id(id);
    (void)position;
    return torrent;
}

namespace
{

[[nodiscard]] auto remote_details_field_keys()
{
    return std::array{
        TR_KEY_id,
        TR_KEY_name,
        TR_KEY_status,
        TR_KEY_hash_string,
        TR_KEY_added_date,
        TR_KEY_download_dir,
        TR_KEY_total_size,
        TR_KEY_size_when_done,
        TR_KEY_left_until_done,
        TR_KEY_have_valid,
        TR_KEY_have_unchecked,
        TR_KEY_downloaded_ever,
        TR_KEY_uploaded_ever,
        TR_KEY_corrupt_ever,
        TR_KEY_error,
        TR_KEY_error_string,
        TR_KEY_eta,
        TR_KEY_activity_date,
        TR_KEY_start_date,
        TR_KEY_percent_done,
        TR_KEY_is_private,
        TR_KEY_comment,
        TR_KEY_creator,
        TR_KEY_date_created,
        TR_KEY_honors_session_limits,
        TR_KEY_download_limited,
        TR_KEY_download_limit,
        TR_KEY_upload_limited,
        TR_KEY_upload_limit,
        TR_KEY_bandwidth_priority,
        TR_KEY_seed_ratio_mode,
        TR_KEY_seed_ratio_limit,
        TR_KEY_seed_idle_mode,
        TR_KEY_seed_idle_limit,
        TR_KEY_peer_limit,
        TR_KEY_peers,
        TR_KEY_tracker_stats,
        TR_KEY_tracker_list,
        TR_KEY_files,
        TR_KEY_file_stats,
        TR_KEY_file_count,
    };
}

} // namespace

void Session::fetch_torrent_properties(
    std::vector<tr_torrent_id_t> const& ids,
    std::function<void(tr_variant&&)> callback) const
{
    if (!impl_->is_remote() || ids.empty() || !callback)
    {
        return;
    }

    auto fields = tr_variant::Vector{};
    for (auto const key : remote_details_field_keys())
    {
        fields.emplace_back(tr_variant::unmanaged_string(tr_quark_get_string_view(key)));
    }

    auto args = tr_variant::Map{ 2U };
    args[TR_KEY_fields] = std::move(fields);
    args[TR_KEY_ids] = to_variant(ids);

    impl_->send_rpc_request(
        TR_KEY_torrent_get,
        tr_variant{ std::move(args) },
        [cb = std::move(callback)](tr_variant& result) mutable { cb(std::move(result)); });
}

void Session::torrent_set_location(std::vector<tr_torrent_id_t> const& ids, std::string_view const path, bool const move)
{
    if (ids.empty())
    {
        return;
    }

    auto args = tr_variant::Map{ 3U };
    args[TR_KEY_ids] = to_variant(ids);
    args[TR_KEY_location] = std::string{ path };
    args[TR_KEY_move] = move;

    if (impl_->is_remote())
    {
        impl_->send_rpc_request(TR_KEY_torrent_set_location, tr_variant{ std::move(args) }, {});
        return;
    }

    for (auto const id : ids)
    {
        if (auto* const tor = find_torrent(id); tor != nullptr)
        {
            int local_done = 0;
            tr_torrentSetLocation(tor, std::string{ path }.c_str(), move, &local_done);
        }
    }
}

FaviconCache<Glib::RefPtr<Gdk::Pixbuf>>& Session::favicon_cache() const
{
    return impl_->favicon_cache();
}

void Session::open_folder(tr_torrent_id_t torrent_id) const
{
    if (impl_->is_remote())
    {
        auto const torrent = find_torrent_ref(torrent_id);
        if (torrent == nullptr)
        {
            return;
        }

        auto path = torrent->get_download_dir();
        if (path.empty())
        {
            return;
        }

        if (torrent->get_file_count() > 1)
        {
            path = Glib::build_filename(path, torrent->get_name());
        }

        gtr_open_file(path);
        return;
    }

    auto const* tor = find_torrent(torrent_id);

    if (tor != nullptr)
    {
        bool const single = tr_torrentFileCount(tor) == 1;
        char const* currentDir = tr_torrentGetCurrentDir(tor);

        if (single)
        {
            gtr_open_file(currentDir);
        }
        else
        {
            gtr_open_file(Glib::build_filename(currentDir, tr_torrentName(tor)));
        }
    }
}

sigc::signal<void(Session::ErrorCode, Glib::ustring const&)>& Session::signal_add_error()
{
    return impl_->signal_add_error();
}

sigc::signal<void(tr_ctor*)>& Session::signal_add_prompt()
{
    return impl_->signal_add_prompt();
}

sigc::signal<void(bool)>& Session::signal_blocklist_updated()
{
    return impl_->signal_blocklist_updated();
}

sigc::signal<void(bool)>& Session::signal_busy()
{
    return impl_->signal_busy();
}

sigc::signal<void(tr_quark)>& Session::signal_prefs_changed()
{
    return impl_->signal_prefs_changed();
}

sigc::signal<void(std::optional<bool>, Session::PortTestIpProtocol)>& Session::signal_port_tested()
{
    return impl_->signal_port_tested();
}

sigc::signal<void(std::unordered_set<tr_torrent_id_t> const&, Torrent::ChangeFlags)>& Session::signal_torrents_changed()
{
    return impl_->signal_torrents_changed();
}
