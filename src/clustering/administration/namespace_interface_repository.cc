// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "clustering/administration/namespace_interface_repository.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>

#include "arch/timing.hpp"
#include "clustering/administration/namespace_metadata.hpp"
#include "clustering/reactor/namespace_interface.hpp"
#include "concurrency/cross_thread_signal.hpp"
#include "concurrency/cross_thread_watchable.hpp"
#include "rdb_protocol/wait_for_readiness.hpp"

#define NAMESPACE_INTERFACE_EXPIRATION_MS (60 * 1000)

struct namespace_repo_t::namespace_cache_t {
public:
    std::map<namespace_id_t, scoped_ptr_t<namespace_cache_entry_t> > entries;
    auto_drainer_t drainer;
};

struct namespace_repo_t::namespace_cache_entry_t :
    public namespace_interface_access_t::ref_tracker_t
{
public:
    void add_ref() {
        ref_count++;
        if (ref_count == 1) {
            if (pulse_when_ref_count_becomes_nonzero) {
                pulse_when_ref_count_becomes_nonzero->
                    pulse_if_not_already_pulsed();
            }
        }
    }
    void release() {
        ref_count--;
        if (ref_count == 0) {
            if (pulse_when_ref_count_becomes_zero) {
                pulse_when_ref_count_becomes_zero->
                    pulse_if_not_already_pulsed();
            }
        }
    }

    promise_t<namespace_interface_t *> namespace_interface;
    int ref_count;
    cond_t *pulse_when_ref_count_becomes_zero;
    cond_t *pulse_when_ref_count_becomes_nonzero;
};

namespace_repo_t::namespace_repo_t(mailbox_manager_t *_mailbox_manager,
                                   const boost::shared_ptr<semilattice_read_view_t<cow_ptr_t<namespaces_semilattice_metadata_t> > > &semilattice_view,
                                   clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> > > _namespaces_directory_metadata,
                                   rdb_context_t *_ctx)
    : mailbox_manager(_mailbox_manager),
      namespaces_view(semilattice_view),
      namespaces_directory_metadata(_namespaces_directory_metadata),
      ctx(_ctx),
      namespaces_subscription(boost::bind(&namespace_repo_t::on_namespaces_change, this, drainer.lock()))
{
    namespaces_subscription.reset(namespaces_view);
}

namespace_repo_t::~namespace_repo_t() { }

std::map<peer_id_t, cow_ptr_t<reactor_business_card_t> > get_reactor_business_cards(
        const change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> &ns_directory_metadata, const namespace_id_t &n_id) {
    std::map<peer_id_t, cow_ptr_t<reactor_business_card_t> > res;
    for (std::map<peer_id_t, namespaces_directory_metadata_t>::const_iterator it  = ns_directory_metadata.get_inner().begin();
         it != ns_directory_metadata.get_inner().end();
         ++it) {
        namespaces_directory_metadata_t::reactor_bcards_map_t::const_iterator jt =
            it->second.reactor_bcards.find(n_id);
        if (jt != it->second.reactor_bcards.end()) {
            res[it->first] = jt->second.internal;
        } else {
            res[it->first] = cow_ptr_t<reactor_business_card_t>();
        }
    }

    return res;
}

void copy_region_maps_to_thread(
        const std::map<namespace_id_t, std::map<key_range_t, machine_id_t> > &from,
        one_per_thread_t<std::map<namespace_id_t, std::map<key_range_t, machine_id_t> > > *to,
        int thread, UNUSED auto_drainer_t::lock_t keepalive) {
    on_thread_t th((threadnum_t(thread)));
    *to->get() = from;
}

void namespace_repo_t::on_namespaces_change(auto_drainer_t::lock_t keepalive) {
    ASSERT_NO_CORO_WAITING;
    std::map<namespace_id_t, std::map<key_range_t, machine_id_t> > new_reg_to_pri_maps;

    namespaces_semilattice_metadata_t::namespace_map_t::const_iterator it;
    const namespaces_semilattice_metadata_t::namespace_map_t &ns = namespaces_view.get()->get().get()->namespaces;
    for (it = ns.begin(); it != ns.end(); ++it) {
        if (it->second.is_deleted()) {
            continue;
        }
        if (it->second.get_ref().blueprint.in_conflict()) {
            /* The reactor won't do anything while the blueprint is in conflict, so the
            old mapping is probably still accurate, although there's no guarantee. */
            auto jt = region_to_primary_maps.get()->find(it->first);
            if (jt != region_to_primary_maps.get()->end()) {
                new_reg_to_pri_maps[it->first] = jt->second;
            }
            continue;
        }
        const persistable_blueprint_t &bp = it->second.get_ref().blueprint.get_ref();
        persistable_blueprint_t::role_map_t::const_iterator it2;
        for (it2 = bp.machines_roles.begin(); it2 != bp.machines_roles.end(); ++it2) {
            const persistable_blueprint_t::region_to_role_map_t &roles = it2->second;
            persistable_blueprint_t::region_to_role_map_t::const_iterator it3;
            for (it3 = roles.begin(); it3 != roles.end(); ++it3) {
                if (it3->second == blueprint_role_t::blueprint_role_primary) {
                    new_reg_to_pri_maps[it->first][it3->first.inner] = it2->first;
                }
            }
        }
    }

    for (int thread = 0; thread < get_num_threads(); ++thread) {
        coro_t::spawn_ordered(std::bind(&copy_region_maps_to_thread,
                                        new_reg_to_pri_maps,
                                        &region_to_primary_maps,
                                        thread,
                                        keepalive));
    }
}

void namespace_repo_t::create_and_destroy_namespace_interface(
            namespace_cache_t *cache,
            const uuid_u &namespace_id,
            auto_drainer_t::lock_t keepalive)
            THROWS_NOTHING{
    keepalive.assert_is_holding(&cache->drainer);
    threadnum_t thread = get_thread_id();

    namespace_cache_entry_t *cache_entry = cache->entries.find(namespace_id)->second.get();
    guarantee(!cache_entry->namespace_interface.get_ready_signal()->is_pulsed());

    /* We need to switch to `home_thread()` to construct `cross_thread_watchable`, then
    switch back. In destruction we need to do the reverse. Fortunately RAII works really
    nicely here. */
    on_thread_t switch_to_home_thread(home_thread());
    clone_ptr_t<watchable_t<std::map<peer_id_t, cow_ptr_t<reactor_business_card_t> > > > subview =
        namespaces_directory_metadata->subview(boost::bind(&get_reactor_business_cards, _1, namespace_id));
    cross_thread_watchable_variable_t<std::map<peer_id_t, cow_ptr_t<reactor_business_card_t> > > cross_thread_watchable(subview, thread);
    on_thread_t switch_back(thread);

    cluster_namespace_interface_t namespace_interface(
        mailbox_manager,
        region_to_primary_maps.get(),
        cross_thread_watchable.get_watchable(),
        namespace_id,
        ctx);

    try {
        /* Wait for the table to become available for use */
        wait_interruptible(namespace_interface.get_initial_ready_signal(),
            keepalive.get_drain_signal());

        /* Give the outside world access to `namespace_interface` */
        cache_entry->namespace_interface.pulse(&namespace_interface);

        /* Wait until it's time to shut down */
        while (true) {
            while (cache_entry->ref_count != 0) {
                cond_t ref_count_is_zero;
                assignment_sentry_t<cond_t *> notify_if_ref_count_becomes_zero(
                    &cache_entry->pulse_when_ref_count_becomes_zero,
                    &ref_count_is_zero);
                wait_interruptible(&ref_count_is_zero, keepalive.get_drain_signal());
            }
            signal_timer_t expiration_timer;
            expiration_timer.start(NAMESPACE_INTERFACE_EXPIRATION_MS);
            cond_t ref_count_is_nonzero;
            assignment_sentry_t<cond_t *> notify_if_ref_count_becomes_nonzero(
                &cache_entry->pulse_when_ref_count_becomes_nonzero,
                &ref_count_is_nonzero);
            wait_any_t waiter(&expiration_timer, &ref_count_is_nonzero);
            wait_interruptible(&waiter, keepalive.get_drain_signal());
            if (!ref_count_is_nonzero.is_pulsed()) {
                guarantee(cache_entry->ref_count == 0);
                /* We waited a whole `NAMESPACE_INTERFACE_EXPIRATION_MS` and
                nothing used us. So let's destroy ourselves. */
                break;
            }
        }

    } catch (const interrupted_exc_t &) {
        /* We got here because we were interrupted in the startup process. That
        means the `namespace_repo_t` destructor was called, which means there
        mustn't exist any `access_t` objects. So ref_count must be 0. */
        guarantee(cache_entry->ref_count == 0);
    }

    ASSERT_NO_CORO_WAITING;
    cache->entries.erase(namespace_id);
}

namespace_interface_access_t namespace_repo_t::get_namespace_interface(
        const uuid_u &ns_id, signal_t *interruptor) {
    /* Find or create a cache entry for the table. When we find or create the cache, we
    need to wait until the `namespace_interface_t *` is actually ready before returning,
    but we want to be sure to hold a reference to the cache entry in the meantime. So we
    construct `temporary_holder`, which manages a reference to the `cache_entry`, but has
    its namespace interface set to `NULL`. Then when the real table is ready, we
    construct a real `namespace_interface_access_t` with a non-`NULL` namespace
    interface, and then delete `temporary_holder`. */
    namespace_interface_access_t temporary_holder;
    namespace_cache_entry_t *cache_entry;
    {
        ASSERT_FINITE_CORO_WAITING;
        namespace_cache_t *cache = namespace_caches.get();
        if (cache->entries.find(ns_id) == cache->entries.end()) {
            cache_entry = new namespace_cache_entry_t;
            cache_entry->ref_count = 0;
            cache_entry->pulse_when_ref_count_becomes_zero = NULL;
            cache_entry->pulse_when_ref_count_becomes_nonzero = NULL;

            namespace_id_t id(ns_id);
            cache->entries.insert(std::make_pair(id,
                scoped_ptr_t<namespace_cache_entry_t>(cache_entry)));

            coro_t::spawn_sometime(boost::bind(
                &namespace_repo_t::create_and_destroy_namespace_interface, this,
                cache, ns_id,
                auto_drainer_t::lock_t(&cache->drainer)));
        } else {
            cache_entry = cache->entries[ns_id].get();
        }
    }
    wait_interruptible(cache_entry->namespace_interface.get_ready_signal(), interruptor);
    return namespace_interface_access_t(
        cache_entry->namespace_interface.wait(),
        cache_entry,
        get_thread_id());
}

