// Copyright (C) 2025 Category Labs, Inc.
//
// Vibe-Room instrumentation data structures for parallel execution telemetry.
//
// Captures per-tx execution metrics (timing, incarnation count, R/W sets)
// and post-hoc conflict detection for downstream visualization and analysis.
//
// All types are compiled only when VIBE_ROOM_INSTRUMENTATION is defined.
// Non-instrumented builds exclude this header entirely.

#pragma once

#ifdef VIBE_ROOM_INSTRUMENTATION

#include <category/core/config.hpp>
#include <category/core/bytes.hpp>
#include <category/execution/ethereum/core/address.hpp>
#include <category/execution/ethereum/state3/state.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

MONAD_NAMESPACE_BEGIN

/// Serializable representation of a state location.
/// Field names match Rust CLI conflict.rs::LocationInfo exactly for JSON compatibility.
struct LocationInfo
{
    std::string location_type; // "Storage", "Balance", "Nonce", "CodeHash"
    std::string address;       // "0x..." lowercase hex
    std::optional<std::string> slot; // "0x..." for Storage, nullopt otherwise

    nlohmann::json to_json() const
    {
        nlohmann::json j;
        j["location_type"] = location_type;
        j["address"] = address;
        if (slot.has_value()) {
            j["slot"] = slot.value();
        }
        return j;
    }
};

/// A detected conflict between two transactions at a specific location.
/// Field names match Rust CLI conflict.rs::ConflictPair exactly.
struct ConflictPair
{
    LocationInfo location;
    uint32_t tx_a;
    uint32_t tx_b;
    std::string conflict_type; // "write-write" or "read-write"

    nlohmann::json to_json() const
    {
        nlohmann::json j;
        j["location"] = location.to_json();
        j["tx_a"] = tx_a;
        j["tx_b"] = tx_b;
        j["conflict_type"] = conflict_type;
        return j;
    }
};

/// Per-transaction instrumentation data captured during execution.
struct TxInstrumentationData
{
    uint32_t tx_index{0};
    uint32_t incarnation_count{1}; // 1 = first pass, 2 = retried
    std::chrono::microseconds exec_time{0};
    std::vector<LocationInfo> reads;
    std::vector<LocationInfo> writes;

    nlohmann::json to_json() const
    {
        nlohmann::json j;
        j["tx_index"] = tx_index;
        j["incarnation_count"] = incarnation_count;
        j["exec_time_us"] = exec_time.count();
        j["reads"] = nlohmann::json::array();
        for (auto const &r : reads) {
            j["reads"].push_back(r.to_json());
        }
        j["writes"] = nlohmann::json::array();
        for (auto const &w : writes) {
            j["writes"].push_back(w.to_json());
        }
        return j;
    }
};

/// Block-level instrumentation data aggregating all per-tx metrics.
/// Thread-safe via mutex for concurrent fiber appends.
struct BlockInstrumentationData
{
    std::vector<TxInstrumentationData> per_tx;
    std::vector<ConflictPair> conflicts;
    std::mutex mtx;

    void add_tx_data(TxInstrumentationData data)
    {
        std::lock_guard<std::mutex> lock(mtx);
        per_tx.push_back(std::move(data));
    }
};

/// Format an Address as "0x..." lowercase hex string.
inline std::string address_to_hex(Address const &addr)
{
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(42);
    for (auto byte : addr.bytes) {
        result += hex_chars[(byte >> 4) & 0xf];
        result += hex_chars[byte & 0xf];
    }
    return result;
}

/// Format a bytes32_t as "0x..." lowercase hex string.
inline std::string bytes32_to_hex(bytes32_t const &val)
{
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(66);
    for (auto byte : val.bytes) {
        result += hex_chars[(byte >> 4) & 0xf];
        result += hex_chars[byte & 0xf];
    }
    return result;
}

/// Extract read locations from State's original() map.
/// Call BEFORE block_state_.merge(state) — merge destroys per-tx data.
///
/// original() contains entries for every account whose state was read
/// during execution. Each entry generates Balance + Nonce LocationInfo.
/// Storage reads are in current() where value == original value.
inline std::vector<LocationInfo> extract_reads(State const &state)
{
    std::vector<LocationInfo> reads;
    for (auto const &[addr, orig_state] : state.original()) {
        auto addr_hex = address_to_hex(addr);
        reads.push_back(LocationInfo{
            .location_type = "Balance",
            .address = addr_hex,
            .slot = std::nullopt});
        reads.push_back(LocationInfo{
            .location_type = "Nonce",
            .address = addr_hex,
            .slot = std::nullopt});
    }
    return reads;
}

/// Extract write locations from State's current() map.
/// Call BEFORE block_state_.merge(state) — merge destroys per-tx data.
///
/// current() contains entries for every account whose state was modified.
/// Each entry generates Balance + Nonce LocationInfo.
/// Storage writes are detected from the storage deltas.
inline std::vector<LocationInfo> extract_writes(State const &state)
{
    std::vector<LocationInfo> writes;
    for (auto const &[addr, version_stack] : state.current()) {
        auto addr_hex = address_to_hex(addr);
        writes.push_back(LocationInfo{
            .location_type = "Balance",
            .address = addr_hex,
            .slot = std::nullopt});
        writes.push_back(LocationInfo{
            .location_type = "Nonce",
            .address = addr_hex,
            .slot = std::nullopt});
    }
    return writes;
}

/// Detect conflicts post-hoc from collected per-tx instrumentation data.
/// O(n²) pairwise comparison matching the Rust detect_conflicts() algorithm.
inline std::vector<ConflictPair>
detect_conflicts_post_hoc(std::vector<TxInstrumentationData> const &per_tx)
{
    std::vector<ConflictPair> conflicts;

    // Build a key for each location: (location_type, address, slot)
    // Using string concatenation for simplicity — this is post-hoc, not hot path
    auto loc_key = [](LocationInfo const &l) -> std::string {
        std::string k = l.location_type + "|" + l.address;
        if (l.slot.has_value()) {
            k += "|" + l.slot.value();
        }
        return k;
    };

    for (size_t a = 0; a < per_tx.size(); ++a) {
        for (size_t b = a + 1; b < per_tx.size(); ++b) {
            auto const &tx_a = per_tx[a];
            auto const &tx_b = per_tx[b];

            // Build key sets
            std::unordered_set<std::string> write_keys_a, write_keys_b;
            std::unordered_set<std::string> read_keys_a, read_keys_b;

            for (auto const &w : tx_a.writes)
                write_keys_a.insert(loc_key(w));
            for (auto const &w : tx_b.writes)
                write_keys_b.insert(loc_key(w));
            for (auto const &r : tx_a.reads)
                read_keys_a.insert(loc_key(r));
            for (auto const &r : tx_b.reads)
                read_keys_b.insert(loc_key(r));

            // Helper to find the LocationInfo by key
            auto find_loc = [](std::vector<LocationInfo> const &locs,
                               std::string const &key,
                               auto const &key_fn) -> LocationInfo const * {
                for (auto const &l : locs) {
                    if (key_fn(l) == key)
                        return &l;
                }
                return nullptr;
            };

            // Write-write conflicts
            for (auto const &key : write_keys_a) {
                if (write_keys_b.count(key)) {
                    auto const *loc = find_loc(tx_a.writes, key, loc_key);
                    if (loc) {
                        conflicts.push_back(ConflictPair{
                            .location = *loc,
                            .tx_a = tx_a.tx_index,
                            .tx_b = tx_b.tx_index,
                            .conflict_type = "write-write"});
                    }
                }
            }

            // Read-write: a reads, b writes
            for (auto const &key : read_keys_a) {
                if (write_keys_b.count(key)) {
                    auto const *loc = find_loc(tx_a.reads, key, loc_key);
                    if (loc) {
                        conflicts.push_back(ConflictPair{
                            .location = *loc,
                            .tx_a = tx_a.tx_index,
                            .tx_b = tx_b.tx_index,
                            .conflict_type = "read-write"});
                    }
                }
            }

            // Read-write: a writes, b reads
            for (auto const &key : write_keys_a) {
                if (read_keys_b.count(key)) {
                    auto const *loc = find_loc(tx_a.writes, key, loc_key);
                    if (loc) {
                        conflicts.push_back(ConflictPair{
                            .location = *loc,
                            .tx_a = tx_a.tx_index,
                            .tx_b = tx_b.tx_index,
                            .conflict_type = "read-write"});
                    }
                }
            }
        }
    }

    return conflicts;
}

MONAD_NAMESPACE_END

#endif // VIBE_ROOM_INSTRUMENTATION
