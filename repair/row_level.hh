/*
 * Copyright (C) 2018-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <vector>
#include "gms/inet_address.hh"
#include "repair/repair.hh"
#include <seastar/core/distributed.hh>
#include <seastar/util/bool_class.hh>

using namespace seastar;

class row_level_repair_gossip_helper;

namespace service {
class migration_manager;
class storage_proxy;
}

namespace db {

class system_keyspace;
class system_distributed_keyspace;
class batchlog_manager;

}

namespace gms {
    class gossiper;
}

class repair_meta;

using repair_meta_ptr = shared_ptr<repair_meta>;

struct shard_config {
    unsigned shard;
    unsigned shard_count;
    unsigned ignore_msb;
};

class repair_history {
public:
    // The key for the map is the table_id
    std::unordered_map<utils::UUID, std::unordered_map<dht::token_range, size_t>> finished_ranges;
    gc_clock::time_point repair_time = gc_clock::time_point::max();
};

class node_ops_metrics {
    tracker& _tracker;
public:
    node_ops_metrics(tracker& tracker);

    uint64_t bootstrap_total_ranges{0};
    uint64_t bootstrap_finished_ranges{0};
    uint64_t replace_total_ranges{0};
    uint64_t replace_finished_ranges{0};
    uint64_t rebuild_total_ranges{0};
    uint64_t rebuild_finished_ranges{0};
    uint64_t decommission_total_ranges{0};
    uint64_t decommission_finished_ranges{0};
    uint64_t removenode_total_ranges{0};
    uint64_t removenode_finished_ranges{0};
    uint64_t repair_total_ranges_sum{0};
    uint64_t repair_finished_ranges_sum{0};
private:
    seastar::metrics::metric_groups _metrics;
public:
    float bootstrap_finished_percentage();
    float replace_finished_percentage();
    float rebuild_finished_percentage();
    float decommission_finished_percentage();
    float removenode_finished_percentage();
    float repair_finished_percentage();
};

class repair_service : public seastar::peering_sharded_service<repair_service> {
    distributed<gms::gossiper>& _gossiper;
    netw::messaging_service& _messaging;
    sharded<replica::database>& _db;
    sharded<service::storage_proxy>& _sp;
    sharded<db::batchlog_manager>& _bm;
    sharded<db::system_distributed_keyspace>& _sys_dist_ks;
    sharded<db::system_keyspace>& _sys_ks;
    sharded<db::view::view_update_generator>& _view_update_generator;
    service::migration_manager& _mm;
    tracker _tracker;
    node_ops_metrics _node_ops_metrics;
    std::unordered_map<node_repair_meta_id, repair_meta_ptr> _repair_metas;
    uint32_t _next_repair_meta_id = 0;  // used only on shard 0

    std::unordered_map<utils::UUID, repair_history> _finished_ranges_history;

    shared_ptr<row_level_repair_gossip_helper> _gossip_helper;
    bool _stopped = false;

    size_t _max_repair_memory;
    seastar::semaphore _memory_sem;

    future<> init_ms_handlers();
    future<> uninit_ms_handlers();

public:
    repair_service(distributed<gms::gossiper>& gossiper,
            netw::messaging_service& ms,
            sharded<replica::database>& db,
            sharded<service::storage_proxy>& sp,
            sharded<db::batchlog_manager>& bm,
            sharded<db::system_distributed_keyspace>& sys_dist_ks,
            sharded<db::system_keyspace>& sys_ks,
            sharded<db::view::view_update_generator>& vug,
            service::migration_manager& mm, size_t max_repair_memory);
    ~repair_service();
    future<> start();
    future<> stop();

    // shutdown() stops all ongoing repairs started on this node (and
    // prevents any further repairs from being started). It returns a future
    // saying when all repairs have stopped, and attempts to stop them as
    // quickly as possible (we do not wait for repairs to finish but rather
    // stop them abruptly).
    future<> shutdown();

    future<std::optional<gc_clock::time_point>> update_history(utils::UUID repair_id, utils::UUID table_id, dht::token_range range, gc_clock::time_point repair_time);
    future<> cleanup_history(utils::UUID repair_id);
    future<> load_history();

    int do_repair_start(sstring keyspace, std::unordered_map<sstring, sstring> options_map);

    // The tokens are the tokens assigned to the bootstrap node.
    future<> bootstrap_with_repair(locator::token_metadata_ptr tmptr, std::unordered_set<dht::token> bootstrap_tokens);
    future<> decommission_with_repair(locator::token_metadata_ptr tmptr);
    future<> removenode_with_repair(locator::token_metadata_ptr tmptr, gms::inet_address leaving_node, shared_ptr<node_ops_info> ops);
    future<> rebuild_with_repair(locator::token_metadata_ptr tmptr, sstring source_dc);
    future<> replace_with_repair(locator::token_metadata_ptr tmptr, std::unordered_set<dht::token> replacing_tokens, std::list<gms::inet_address> ignore_nodes);
private:
    future<> do_decommission_removenode_with_repair(locator::token_metadata_ptr tmptr, gms::inet_address leaving_node, shared_ptr<node_ops_info> ops);
    future<> do_rebuild_replace_with_repair(locator::token_metadata_ptr tmptr, sstring op, sstring source_dc, streaming::stream_reason reason, std::list<gms::inet_address> ignore_nodes);

    future<> sync_data_using_repair(sstring keyspace,
            dht::token_range_vector ranges,
            std::unordered_map<dht::token_range, repair_neighbors> neighbors,
            streaming::stream_reason reason,
            std::optional<utils::UUID> ops_uuid);

    future<> do_sync_data_using_repair(sstring keyspace,
            dht::token_range_vector ranges,
            std::unordered_map<dht::token_range, repair_neighbors> neighbors,
            streaming::stream_reason reason,
            std::optional<utils::UUID> ops_uuid);

    future<repair_update_system_table_response> repair_update_system_table_handler(
            gms::inet_address from,
            repair_update_system_table_request req);

    future<repair_flush_hints_batchlog_response> repair_flush_hints_batchlog_handler(
            gms::inet_address from,
            repair_flush_hints_batchlog_request req);

public:
    netw::messaging_service& get_messaging() noexcept { return _messaging; }
    sharded<replica::database>& get_db() noexcept { return _db; }
    service::migration_manager& get_migration_manager() noexcept { return _mm; }
    sharded<db::system_distributed_keyspace>& get_sys_dist_ks() noexcept { return _sys_dist_ks; }
    sharded<db::view::view_update_generator>& get_view_update_generator() noexcept { return _view_update_generator; }
    gms::gossiper& get_gossiper() noexcept { return _gossiper.local(); }
    size_t max_repair_memory() const { return _max_repair_memory; }
    seastar::semaphore& memory_sem() { return _memory_sem; }
    tracker& repair_tracker() {
        return _tracker;
    }
    const tracker& repair_tracker() const {
        return _tracker;
    }

    const node_ops_metrics& get_metrics() const noexcept {
        return _node_ops_metrics;
    };
    node_ops_metrics& get_metrics() noexcept {
        return _node_ops_metrics;
    };

    // returns a vector with the ids of the active repairs
    future<std::vector<int>> get_active_repairs();

    // returns the status of repair task `id`
    future<repair_status> get_status(int id);

    // If the repair job is finished (SUCCESSFUL or FAILED), it returns immediately.
    // It blocks if the repair job is still RUNNING until timeout.
    future<repair_status> await_completion(int id, std::chrono::steady_clock::time_point timeout);

    // Abort all the repairs
    future<> abort_all();

    future<> abort_repair_node_ops(utils::UUID ops_uuid);

    std::unordered_map<node_repair_meta_id, repair_meta_ptr>& repair_meta_map() noexcept {
        return _repair_metas;
    }

    repair_meta_ptr get_repair_meta(gms::inet_address from, uint32_t repair_meta_id);

    future<>
    insert_repair_meta(
            const gms::inet_address& from,
            uint32_t src_cpu_id,
            uint32_t repair_meta_id,
            dht::token_range range,
            row_level_diff_detect_algorithm algo,
            uint64_t max_row_buf_size,
            uint64_t seed,
            shard_config master_node_shard_config,
            table_schema_version schema_version,
            streaming::stream_reason reason);

    future<>
    remove_repair_meta(const gms::inet_address& from,
            uint32_t repair_meta_id,
            sstring ks_name,
            sstring cf_name,
            dht::token_range range);

    future<> remove_repair_meta(gms::inet_address from);

    future<> remove_repair_meta();

    future<uint32_t> get_next_repair_meta_id();
};

class repair_info;
using repair_master = bool_class<class repair_master_tag>;
class partition_key_and_mutation_fragments;
using repair_rows_on_wire = std::list<partition_key_and_mutation_fragments>;
class repair_row;
class repair_hasher;
class repair_writer;

future<> repair_cf_range_row_level(repair_info& ri,
        sstring cf_name, utils::UUID table_id, dht::token_range range,
        const std::vector<gms::inet_address>& all_peer_nodes);
future<std::list<repair_row>> to_repair_rows_list(repair_rows_on_wire rows,
        schema_ptr s, uint64_t seed, repair_master is_master,
        reader_permit permit, repair_hasher hasher);
void flush_rows(schema_ptr s, std::list<repair_row>& rows, lw_shared_ptr<repair_writer>& writer);
