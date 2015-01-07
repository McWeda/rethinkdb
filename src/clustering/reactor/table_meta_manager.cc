// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/reactor/table_meta_manager.hpp"

#include "clustering/generic/raft_core.tcc"
#include "clustering/generic/raft_network.tcc"

RDB_IMPL_SERIALIZABLE_2(
    table_meta_manager_business_card_t::action_timestamp_t::epoch_t, timestamp, id);
RDB_IMPL_SERIALIZABLE_2(
    table_meta_manager_business_card_t::action_timestamp_t, epoch, log_index);
RDB_IMPL_SERIALIZABLE_2(
    table_meta_manager_business_card_t, action_mailbox, server_id);
RDB_IMPL_SERIALIZABLE_4(
    table_meta_business_card_t, epoch, raft_member_id, raft_business_card, server_id);

table_meta_manager_t::table_meta_manager_t(
        const server_id_t &_server_id,
        mailbox_manager_t *_mailbox_manager,
        watchable_map_t<peer_id_t, table_meta_manager_business_card_t>
            *_table_meta_manager_directory,
        watchable_map_t<std::pair<peer_id_t, namespace_id_t>, table_meta_business_card_t>
            *_table_meta_directory,
        table_meta_persistence_interface_t *_persistence_interface) :
    server_id(_server_id),
    mailbox_manager(_mailbox_manager),
    table_meta_manager_directory(_table_meta_manager_directory),
    table_meta_directory(_table_meta_directory),
    persistence_interface(_persistence_interface),
    /* Whenever a server connects, we need to sync all of our tables to it. */
    table_meta_manager_directory_subs(
        table_meta_manager_directory,
        [this](const peer_id_t &peer, const table_meta_manager_business_card_t *bcard) {
            if (peer != mailbox_manager->get_me() && bcard != nullptr) {
                mutex_assertion_t::acq_t mutex_acq(&mutex);
                for (auto &&pair : tables) {
                    schedule_sync(pair.first, pair.second.get(), peer);
                }
            }
        }, false),
    /* Whenever a server changes its entry for a table in the directory, we need to
    re-sync that table to that server. */
    table_meta_directory_subs(
        table_meta_directory,
        [this](const std::pair<peer_id_t, namespace_id_t> &key,
                const table_meta_business_card_t *) {
            if (key.first != mailbox_manager->get_me()) {
                mutex_assertion_t::acq_t mutex_acq(&mutex);
                auto it = tables.find(key.second);
                if (it != tables.end()) {
                    schedule_sync(key.second, it->second.get(), key.first);
                }
            }
        }, false),
    action_mailbox(mailbox_manager,
        std::bind(&table_meta_manager_t::on_action, this,
            ph::_1, ph::_2, ph::_3, ph::_4, ph::_5, ph::_6, ph::_7))
{
    /* Resurrect any tables that were sitting on disk from when we last shut down */
    cond_t non_interruptor;
    persistence_interface->read_all_tables(
        [&](const namespace_id_t &table_id, const table_meta_persistent_state_t &state,
                scoped_ptr_t<multistore_ptr_t> &&multistore_ptr) {
            mutex_assertion_t::acq_t global_mutex_acq(&mutex);
            guarantee(tables.count(table_id) == 0);
            table_t *table;
            tables[table_id].init(table = new table_t);
            new_mutex_acq_t table_mutex_acq(&table->mutex);
            global_mutex_acq.reset();
            table->multistore_ptr = std::move(multistore_ptr);
            table->active = make_scoped<active_table_t>(this, table_id, state.epoch,
                state.member_id, state.raft_state, table->multistore_ptr.get());
            raft_member_t<table_raft_state_t>::state_and_config_t raft_state =
                table->active->raft.get_raft()->get_committed_state()->get();
            guarantee(raft_state.state.member_ids.at(server_id) == state.member_id,
                "Somehow we persisted a state in which we were not a member of the "
                "cluster");
            table->timestamp.epoch = state.epoch;
            table->timestamp.log_index = raft_state.log_index;
            table->is_deleted = false;

            /* This probably won't do anything, since we probably won't be connected to
            any other servers when `table_meta_manager_t` is created; but just in case,
            sync to any other servers that are connected. */
            table_meta_manager_directory->read_all(
                [&](const peer_id_t &peer, const table_meta_manager_business_card_t *) {
                    if (peer != mailbox_manager->get_me()) {
                        schedule_sync(table_id, table, peer);
                    }
                });
        },
        &non_interruptor);
}

table_meta_manager_t::active_table_t::active_table_t(
        table_meta_manager_t *_parent,
        const namespace_id_t &_table_id,
        const table_meta_manager_business_card_t::action_timestamp_t::epoch_t &_epoch,
        const raft_member_id_t &_member_id,
        const raft_persistent_state_t<table_raft_state_t> &initial_state,
        UNUSED multistore_ptr_t *multistore_ptr) :
    parent(_parent),
    table_id(_table_id),
    epoch(_epoch),
    member_id(_member_id),
    raft(member_id, parent->mailbox_manager, &raft_directory, this, initial_state),
    /* We monitor the directory for updates related to this table, then extract the
    `raft_business_card_t`s and pass them on to the `raft_networked_member_t`. */
    table_directory_subs(
        parent->table_meta_directory,
        [this](const std::pair<peer_id_t, namespace_id_t> &key,
                const table_meta_business_card_t *bcard) {
            if (key.second == table_id) {
                boost::optional<raft_member_id_t> new_member_id;
                /* Note that if another server is in a different epoch from us, then we
                don't put the Raft members into contact with each other. */
                if (bcard != nullptr && bcard->epoch == epoch) {
                    new_member_id = bcard->raft_member_id;
                }
                auto it = old_peer_member_ids.find(key.first);
                if (it != old_peer_member_ids.end() &&
                        (!static_cast<bool>(new_member_id) ||
                            *new_member_id != it->second)) {
                    raft_directory.delete_key(it->second);
                    old_peer_member_ids.erase(it);
                }
                if (static_cast<bool>(new_member_id)) {
                    raft_directory.set_key_no_equals(
                        *new_member_id, bcard->raft_business_card);
                    old_peer_member_ids[key.first] = *new_member_id;
                }
            }
        }, true),
    /* Every time the Raft cluster commits a transaction, we re-sync to every other
    server in the cluster. This is because the transaction might consist of adding or
    removing a server, in which case we need to notify that server. */
    raft_commit_subs([this]() {
            mutex_assertion_t::acq_t mutex_acq(&parent->mutex);
            auto it = parent->tables.find(table_id);
            guarantee(it != parent->tables.end());
            parent->table_meta_manager_directory->read_all(
            [&](const peer_id_t &peer, const table_meta_manager_business_card_t *) {
                if (peer != parent->mailbox_manager->get_me()) {
                    parent->schedule_sync(table_id, it->second.get(), peer);
                }
            });
        })
{
    watchable_t<raft_member_t<table_raft_state_t>::state_and_config_t>::freeze_t
        freeze(raft.get_raft()->get_committed_state());
    raft_commit_subs.reset(raft.get_raft()->get_committed_state(), &freeze);

    table_meta_business_card_t bcard;
    bcard.epoch = _epoch;
    bcard.raft_member_id = member_id;
    bcard.raft_business_card = raft.get_business_card();
    bcard.server_id = parent->server_id;
    parent->table_meta_business_cards.set_key_no_equals(table_id, bcard);
}

table_meta_manager_t::active_table_t::~active_table_t() {
    parent->table_meta_business_cards.delete_key(table_id);
}

void table_meta_manager_t::active_table_t::write_persistent_state(
        const raft_persistent_state_t<table_raft_state_t> &inner_ps,
        signal_t *interruptor) {
    table_meta_persistent_state_t outer_ps;
    outer_ps.epoch = epoch;
    outer_ps.member_id = member_id;
    outer_ps.raft_state = inner_ps;
    parent->persistence_interface->update_table(table_id, outer_ps, interruptor);
}

void table_meta_manager_t::on_action(
        signal_t *interruptor,
        const namespace_id_t &table_id,
        const table_meta_manager_business_card_t::action_timestamp_t &timestamp,
        bool is_deletion,
        const boost::optional<raft_member_id_t> &member_id,
        const boost::optional<raft_persistent_state_t<table_raft_state_t> >
            &initial_state,
        const mailbox_t<void()>::address_t &ack_addr) {
    /* Validate the incoming message */
    guarantee(static_cast<bool>(member_id) == static_cast<bool>(initial_state));
    guarantee(!(is_deletion && static_cast<bool>(member_id)));
    guarantee(is_deletion == timestamp.epoch.id.is_nil());

    /* Find or create the table record for this table */
    mutex_assertion_t::acq_t global_mutex_acq(&mutex);
    bool is_new = (tables.count(table_id) == 0);
    table_t *table;
    scoped_ptr_t<table_t> *table_ptr = &tables[table_id];
    if (!table_ptr->has()) {
        table_ptr->init(table = new table_t);
    } else {
        table = table_ptr->get();
    }
    new_mutex_in_line_t table_mutex_in_line(&table->mutex);
    global_mutex_acq.reset();
    wait_interruptible(table_mutex_in_line.acq_signal(), interruptor);

    /* Validate existing record */
    if (!is_new) {
        guarantee(!(table->is_deleted && !is_deletion), "Can't un-delete a table");
        guarantee(table->is_deleted == table->timestamp.epoch.id.is_nil());
        guarantee(table->multistore_ptr.has() == table->active.has());
    }

    /* Reject outdated or redundant messages */
    if (!is_new && !timestamp.supersedes(table->timestamp)) {
        return;
    }

    /* Bring record up to date */
    if (static_cast<bool>(member_id) && !table->active.has()) {
        /* The table is being created, or we are joining it */
        table_meta_persistent_state_t st;
        st.epoch = timestamp.epoch;
        st.member_id = *member_id;
        st.raft_state = *initial_state;
        persistence_interface->add_table(
            table_id, st, &table->multistore_ptr, interruptor);
        table->active = make_scoped<active_table_t>(this, table_id, timestamp.epoch,
            *member_id, *initial_state, table->multistore_ptr.get());
    } else if (!static_cast<bool>(member_id) && table->active.has()) {
        /* The table is being dropped, or we are leaving it */
        table->active.reset();
        table->multistore_ptr.reset();
        persistence_interface->remove_table(table_id, interruptor);
    } else if (static_cast<bool>(member_id) && table->active.has() &&
            (table->timestamp.epoch != timestamp.epoch ||
                table->active->member_id != *member_id)) {
        /* If the epoch changed, then the table's configuration was manually overridden.
        If the member ID changed, then we were removed and then re-added, but we never
        processed the removal message. In either case, we have to destroy and re-create
        `table->active`. */
        table->active.reset();
        table_meta_persistent_state_t st;
        st.epoch = timestamp.epoch;
        st.member_id = *member_id;
        st.raft_state = *initial_state;
        persistence_interface->update_table(table_id, st, interruptor);
        table->active = make_scoped<active_table_t>(this, table_id, timestamp.epoch,
            *member_id, *initial_state, table->multistore_ptr.get());
    }

    table->timestamp = timestamp;
    table->is_deleted = is_deletion;

    /* Sync this table to all other peers. This is usually redundant, but if we encounter
    non-transitive connectivity it can prevent a stall. For example, suppose that server
    A creates a new table, which is hosted on B, C, and D; but the messages from A to
    C and D are lost. This code path will cause B to forward the messages to C and D
    instead of being stuck in a limbo state. */
    table_meta_manager_directory->read_all(
    [&](const peer_id_t &peer, const table_meta_manager_business_card_t *) {
        if (peer != mailbox_manager->get_me()) {
            schedule_sync(table_id, table, peer);
        }
    });

    if (!ack_addr.is_nil()) {
        send(mailbox_manager, ack_addr);
    }
}

void table_meta_manager_t::do_sync(
        const namespace_id_t &table_id,
        const table_t &table,
        const server_id_t &other_server_id,
        const boost::optional<table_meta_business_card_t> &table_bcard,
        const table_meta_manager_business_card_t &table_manager_bcard) {
    if (table.is_deleted && static_cast<bool>(table_bcard)) {
        send(mailbox_manager, table_manager_bcard.action_mailbox,
            table_id, table.timestamp, true, boost::optional<raft_member_id_t>(),
            boost::optional<raft_persistent_state_t<table_raft_state_t> >(),
            mailbox_t<void()>::address_t());
    } else if (table.active.has()) {
        raft_log_index_t log_index;
        boost::optional<raft_member_id_t> member_id;
        table.active->raft.get_raft()->get_committed_state()->apply_read(
            [&](const raft_member_t<table_raft_state_t>::state_and_config_t *st) {
                log_index = st->log_index;
                auto it = st->state.member_ids.find(other_server_id);
                if (it != st->state.member_ids.end()) {
                    member_id = it->second;
                }
            });
        if (static_cast<bool>(member_id) != static_cast<bool>(table_bcard) ||
                (static_cast<bool>(member_id) && static_cast<bool>(table_bcard) &&
                    (table.timestamp.epoch.supersedes(table_bcard->epoch) ||
                        *member_id != table_bcard->raft_member_id))) {
            boost::optional<raft_persistent_state_t<table_raft_state_t> > initial_state;
            if (static_cast<bool>(member_id)) {
                initial_state = table.active->raft.get_raft()->get_state_for_init();
            }
            table_meta_manager_business_card_t::action_timestamp_t timestamp;
            timestamp.epoch = table.timestamp.epoch;
            timestamp.log_index = log_index;
            send(mailbox_manager, table_manager_bcard.action_mailbox,
                table_id, timestamp, false, member_id, initial_state,
                mailbox_t<void()>::address_t());
        }
    }
}

void table_meta_manager_t::schedule_sync(
        const namespace_id_t &table_id,
        table_t *table,
        const peer_id_t &peer_id) {
    ASSERT_FINITE_CORO_WAITING;
    table->to_sync_set.insert(peer_id);
    if (table->sync_coro_running) {
        /* The sync coro will pick up our change when it finishes the current batch */
        return;
    }
    guarantee(table->to_sync_set.size() == 1, "If there were other changes queued up, "
        "the sync coro should have already been running");
    table->sync_coro_running = true;
    auto_drainer_t::lock_t keepalive(&drainer);
    /* Spawn the sync coroutine. Note that we have at most one instance of this coroutine
    per table running at any given time. The naive approach would be to spawn one
    instance every time that `schedule_sync()` is called. The problem is that
    `schedule_sync()` may be called redundantly many times; this approach means that a
    lot of the redundant calls will be collapsed. */
    coro_t::spawn_sometime(
    [this, keepalive /* important to capture */, table_id, table]() {
        try {
            mutex_assertion_t::acq_t global_mutex_acq(&mutex);
            while (!table->to_sync_set.empty()) {
                std::set<peer_id_t> to_sync_set;
                std::swap(table->to_sync_set, to_sync_set);
                new_mutex_in_line_t table_mutex_in_line(&table->mutex);
                global_mutex_acq.reset();
                wait_interruptible(table_mutex_in_line.acq_signal(),
                    keepalive.get_drain_signal());
                for (const peer_id_t &peer : to_sync_set) {
                    guarantee(peer != mailbox_manager->get_me());
                    /* Removing `peer` from `table->to_sync_set` isn't strictly
                    necessary, but it sometimes reduces redundant traffic */
                    table->to_sync_set.erase(peer);
                    boost::optional<table_meta_manager_business_card_t>
                        table_meta_manager_bcard =
                            table_meta_manager_directory->get_key(peer);
                    if (!static_cast<bool>(table_meta_manager_bcard)) {
                        /* Peer is not connected. */
                        continue;
                    }
                    do_sync(
                        table_id,
                        *table,
                        table_meta_manager_bcard->server_id,
                        table_meta_directory->get_key(std::make_pair(peer, table_id)),
                        *table_meta_manager_bcard);
                }
                global_mutex_acq.reset(&mutex);
            }
            table->sync_coro_running = false;
        } catch (const interrupted_exc_t &) {
            /* We're shutting down. Don't continue syncing. `sync_coro_running` will be
            left in the `true` state, but that doesn't matter. */
        }
    });
}

bool table_create(
        mailbox_manager_t *mailbox_manager,
        watchable_map_t<peer_id_t, table_meta_manager_business_card_t>
            *table_meta_manager_directory,
        const table_config_t &config,
        signal_t *interruptor,
        namespace_id_t *table_id_out) {
    *table_id_out = generate_uuid();

    table_meta_manager_business_card_t::action_timestamp_t timestamp;
    timestamp.epoch.timestamp = current_microtime();
    timestamp.epoch.id = generate_uuid();
    timestamp.log_index = 0;

    std::set<server_id_t> servers;
    for (const table_config_t::shard_t &shard : config.shards) {
        servers.insert(shard.replicas.begin(), shard.replicas.end());
    }

    table_raft_state_t raft_state;
    raft_state.config = config;

    raft_config_t raft_config;
    for (const server_id_t &server_id : servers) {
        raft_member_id_t member_id = generate_uuid();
        raft_state.member_ids[server_id] = member_id;
        raft_config.voting_members.insert(member_id);
    }

    raft_persistent_state_t<table_raft_state_t> raft_ps =
        raft_persistent_state_t<table_raft_state_t>::make_initial(
            raft_state, raft_config);

    std::map<server_id_t, table_meta_manager_business_card_t> bcards;
    table_meta_manager_directory->read_all(
        [&](const peer_id_t &, const table_meta_manager_business_card_t *bc) {
            if (servers.count(bc->server_id) == 1) {
                bcards[bc->server_id] = *bc;
            }
        });

    size_t acks = 0;
    pmap(bcards.begin(), bcards.end(),
    [&](const std::pair<server_id_t, table_meta_manager_business_card_t> &pair) {
        try {
            disconnect_watcher_t dw(mailbox_manager,
                pair.second.action_mailbox.get_peer());
            cond_t got_ack;
            mailbox_t<void()> ack_mailbox(mailbox_manager,
                [&](signal_t *) { got_ack.pulse(); });
            wait_any_t interruptor2(&dw, interruptor);
            send(mailbox_manager, pair.second.action_mailbox,
                *table_id_out,
                timestamp,
                false,
                boost::optional<raft_member_id_t>(raft_state.member_ids.at(pair.first)),
                boost::optional<raft_persistent_state_t<table_raft_state_t> >(raft_ps),
                ack_mailbox.get_address());
            wait_interruptible(&got_ack, &interruptor2);
            ++acks;
        } catch (const interrupted_exc_t &) {
            /* do nothing */
        }
    });

    return (acks > 0);
}

bool table_drop(
        mailbox_manager_t *mailbox_manager,
        watchable_map_t<peer_id_t, table_meta_manager_business_card_t>
            *table_meta_manager_directory,
        watchable_map_t<std::pair<peer_id_t, namespace_id_t>, table_meta_business_card_t>
            *table_meta_directory,
        const namespace_id_t &table_id,
        signal_t *interruptor) {
    table_meta_manager_business_card_t::action_timestamp_t drop_timestamp;
    drop_timestamp.epoch.timestamp = std::numeric_limits<microtime_t>::max();
    drop_timestamp.epoch.id = nil_uuid();
    drop_timestamp.log_index = std::numeric_limits<raft_log_index_t>::max();

    std::map<server_id_t, table_meta_manager_business_card_t> bcards;
    table_meta_directory->read_all(
    [&](const std::pair<peer_id_t, namespace_id_t> &key,
            const table_meta_business_card_t *) {
        if (key.second == table_id) {
            table_meta_manager_directory->read_key(key.first,
            [&](const table_meta_manager_business_card_t *bc) {
                if (bc != nullptr) {
                    bcards[bc->server_id] = *bc;
                }
            });
        }
    });

    size_t acks = 0;
    pmap(bcards.begin(), bcards.end(),
    [&](const std::pair<server_id_t, table_meta_manager_business_card_t> &pair) {
        try {
            disconnect_watcher_t dw(mailbox_manager,
                pair.second.action_mailbox.get_peer());
            cond_t got_ack;
            mailbox_t<void()> ack_mailbox(mailbox_manager,
                [&](signal_t *) { got_ack.pulse(); });
            wait_any_t interruptor2(&dw, interruptor);
            send(mailbox_manager, pair.second.action_mailbox,
                table_id, drop_timestamp, true, boost::optional<raft_member_id_t>(),
                boost::optional<raft_persistent_state_t<table_raft_state_t> >(),
                ack_mailbox.get_address());
            wait_interruptible(&got_ack, &interruptor2);
            ++acks;
        } catch (const interrupted_exc_t &) {
            /* do nothing */
        }
    });

    return (acks > 0);
}

