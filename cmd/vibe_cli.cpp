// Copyright (C) 2025 Category Labs, Inc.
// Licensed under GPL-3.0. See LICENSE for details.
//
// monad-vibe-cli — Headless JSON CLI for Monad parallel EVM execution.
//
// Reads JSON from stdin, executes transactions via the official C++ engine
// (execute_block<EvmTraits<EVMC_CANCUN>>), writes JSON to stdout.
//
// Four-phase pipeline: parse → init → execute → format
// Structured error output on stderr: {"error":"...", "phase":"..."}
//
// When compiled with VIBE_ROOM_INSTRUMENTATION (CMake target default):
// - Captures per-tx execution timing, incarnation count, R/W sets
// - Runs post-hoc conflict detection on R/W set intersections
// - Populates conflict_details and per_tx_exec_time_us in JSON output

#include <category/core/assert.h>
#include <category/core/basic_formatter.hpp>
#include <category/core/byte_string.hpp>
#include <category/core/bytes.hpp>
#include <category/core/config.hpp>
#include <category/core/fiber/priority_pool.hpp>
#include <category/core/hex.hpp>
#include <category/core/int.hpp>
#include <category/core/keccak.hpp>
#include <category/core/result.hpp>
#include <category/execution/ethereum/block_hash_buffer.hpp>
#include <category/execution/ethereum/chain/chain.hpp>
#include <category/execution/ethereum/chain/ethereum_mainnet.hpp>
#include <category/execution/ethereum/core/account.hpp>
#include <category/execution/ethereum/core/block.hpp>
#include <category/execution/ethereum/core/receipt.hpp>
#include <category/execution/ethereum/core/transaction.hpp>
#include <category/execution/ethereum/db/db_snapshot.h>
#include <category/execution/ethereum/db/trie_db.hpp>
#include <category/execution/ethereum/db/util.hpp>
#include <category/execution/ethereum/execute_block.hpp>
#include <category/execution/ethereum/metrics/block_metrics.hpp>
#include <category/execution/ethereum/state2/block_state.hpp>
#include <category/execution/ethereum/state2/state_deltas.hpp>
#include <category/execution/ethereum/trace/call_tracer.hpp>
#include <category/execution/ethereum/trace/state_tracer.hpp>
#include <category/mpt/db.hpp>
#include <category/vm/evm/traits.hpp>

#include <evmc/evmc.hpp>
#include <nlohmann/json.hpp>
#include <quill/Quill.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef VIBE_ROOM_INSTRUMENTATION
#include <category/execution/ethereum/metrics/vibe_instrumentation.hpp>
#endif

using namespace monad;
using json = nlohmann::json;

// ── Structured error output ─────────────────────────────────────────────────

static void emit_error(std::string const &msg, std::string const &phase)
{
    json err;
    err["error"] = msg;
    err["phase"] = phase;
    std::cerr << err.dump() << std::endl;
}

// ── Input types ─────────────────────────────────────────────────────────────

struct ParsedTx
{
    Address sender;
    std::optional<Address> to;
    byte_string data;
    uint256_t value;
    uint64_t gas_limit;
    uint64_t nonce;
    uint256_t gas_price;
};

struct ParsedInput
{
    std::vector<ParsedTx> parsed_txs;
    std::vector<Transaction> transactions;
    std::vector<Address> senders;
    BlockHeader header;
};

// ── Hex parsing helpers ─────────────────────────────────────────────────────

static Address parse_address(std::string const &s)
{
    auto hex = s;
    if (hex.substr(0, 2) == "0x" || hex.substr(0, 2) == "0X")
        hex = hex.substr(2);
    // Pad to 40 chars
    while (hex.size() < 40)
        hex = "0" + hex;
    auto bytes = from_hex(hex);
    if (!bytes || bytes->size() != 20) {
        return Address{};
    }
    Address addr{};
    std::copy(bytes->begin(), bytes->end(), addr.bytes);
    return addr;
}

static uint256_t parse_uint256(std::string const &s)
{
    if (s.empty())
        return 0;
    if (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X") {
        auto hex = s.substr(2);
        if (hex.empty())
            return 0;
        return intx::from_string<uint256_t>("0x" + hex);
    }
    return intx::from_string<uint256_t>(s);
}

static byte_string parse_bytes(std::string const &s)
{
    auto hex = s;
    if (hex.substr(0, 2) == "0x" || hex.substr(0, 2) == "0X")
        hex = hex.substr(2);
    if (hex.empty())
        return {};
    auto result = from_hex(hex);
    if (!result)
        return {};
    return *result;
}

// ── Phase 1: Parse input ────────────────────────────────────────────────────

static std::optional<ParsedInput> parse_input(std::string const &input_str)
{
    json j;
    try {
        j = json::parse(input_str);
    } catch (json::exception const &e) {
        emit_error(std::string("JSON parse error: ") + e.what(), "parse");
        return std::nullopt;
    }

    ParsedInput input;

    // Parse block_env
    auto const &be = j["block_env"];
    input.header.number = be.value("number", uint64_t{1});
    input.header.beneficiary = parse_address(be.value("coinbase", std::string("0x0")));
    input.header.timestamp = be.value("timestamp", uint64_t{0});
    input.header.gas_limit = be.value("gas_limit", uint64_t{30000000});
    auto base_fee_str = be.value("base_fee", std::string("0"));
    input.header.base_fee_per_gas = parse_uint256(base_fee_str);
    auto diff_str = be.value("difficulty", std::string("0"));
    input.header.difficulty = parse_uint256(diff_str);
    // Cancun blob fields — required even for non-blob txs
    input.header.blob_gas_used = 0;
    input.header.excess_blob_gas = 0;
    input.header.parent_beacon_block_root = bytes32_t{};

    // Parse transactions
    if (j.contains("transactions") && j["transactions"].is_array()) {
        for (auto const &tx_json : j["transactions"]) {
            ParsedTx ptx;
            ptx.sender = parse_address(tx_json.value("sender", std::string("0x0")));
            if (tx_json.contains("to") && !tx_json["to"].is_null()) {
                ptx.to = parse_address(tx_json["to"].get<std::string>());
            }
            ptx.data = parse_bytes(tx_json.value("data", std::string("0x")));
            ptx.value = parse_uint256(tx_json.value("value", std::string("0")));
            ptx.gas_limit = tx_json.value("gas_limit", uint64_t{21000});
            ptx.nonce = tx_json.value("nonce", uint64_t{0});
            ptx.gas_price = parse_uint256(tx_json.value("gas_price", std::string("1000000000")));

            // Build Transaction for execute_block
            Transaction tx;
            tx.nonce = ptx.nonce;
            tx.max_fee_per_gas = ptx.gas_price;
            tx.gas_limit = ptx.gas_limit;
            tx.value = ptx.value;
            tx.to = ptx.to;
            tx.data = ptx.data;
            // Dummy signature to pass validation — senders provided directly
            tx.sc.r = 1;
            tx.sc.s = 1;

            input.parsed_txs.push_back(std::move(ptx));
            input.transactions.push_back(std::move(tx));
            input.senders.push_back(input.parsed_txs.back().sender);
        }
    }

    return input;
}

// ── Phase 2: Init state ─────────────────────────────────────────────────────

struct ExecutionContext
{
    InMemoryMachine machine;
    mpt::Db db;
    TrieDb tdb;
    vm::VM vm;
    std::unique_ptr<BlockState> block_state;

    ExecutionContext()
        : machine()
        , db(machine)
        , tdb(db)
        , vm()
        , block_state(nullptr)
    {
    }
};

static std::unique_ptr<ExecutionContext>
init_state(ParsedInput const &input)
{
    auto ctx = std::make_unique<ExecutionContext>();

    // Pre-fund unique senders with 1000 ETH at genesis
    // Inline commit_sequential: commit StateDeltas + empty Code at block 0
    StateDeltas genesis_deltas;
    auto const prefund_balance =
        intx::from_string<uint256_t>("1000000000000000000000"); // 1000 ETH

    // Collect unique senders
    std::vector<Address> unique_senders;
    for (auto const &ptx : input.parsed_txs) {
        bool found = false;
        for (auto const &s : unique_senders) {
            if (s == ptx.sender) {
                found = true;
                break;
            }
        }
        if (!found) {
            unique_senders.push_back(ptx.sender);
        }
    }

    for (auto const &addr : unique_senders) {
        StateDeltas::accessor acc;
        genesis_deltas.insert(acc, addr);
        acc->second = StateDelta{
            .account = AccountDelta{
                std::nullopt,
                Account{
                    .balance = prefund_balance,
                    .code_hash = NULL_HASH,
                    .nonce = 0}},
            .storage = {}};
    }

    Code empty_code;
    ctx->tdb.commit(
        genesis_deltas, empty_code, bytes32_t{},
        BlockHeader{.number = 0});
    ctx->tdb.finalize(0, bytes32_t{});

    ctx->block_state = std::make_unique<BlockState>(ctx->tdb, ctx->vm);

    return ctx;
}

// ── Phase 3: Execute ────────────────────────────────────────────────────────

struct ExecutionResult
{
    std::vector<Receipt> receipts;
    BlockMetrics metrics;
#ifdef VIBE_ROOM_INSTRUMENTATION
    std::unique_ptr<BlockInstrumentationData> instrumentation;
#endif
};

static std::optional<ExecutionResult>
execute(ExecutionContext &ctx, ParsedInput const &input)
{
    auto const num_txs = input.transactions.size();

    // Build Block
    Block block;
    block.header = input.header;
    block.transactions = input.transactions;

    // Block hash buffer (empty — no parent hash lookup needed for simple execution)
    BlockHashBufferFinalized block_hash_buffer;
    if (input.header.number > 0) {
        block_hash_buffer.set(input.header.number - 1, bytes32_t{});
    }

    // Empty authorities
    std::vector<std::vector<std::optional<Address>>> authorities(num_txs);

    // Create tracers
    std::vector<std::vector<CallFrame>> call_frames(num_txs);
    std::vector<std::unique_ptr<CallTracerBase>> call_tracers;
    std::vector<std::unique_ptr<trace::StateTracer>> state_tracers;
    for (size_t i = 0; i < num_txs; ++i) {
        call_tracers.emplace_back(std::make_unique<NoopCallTracer>());
        state_tracers.emplace_back(
            std::make_unique<trace::StateTracer>(std::monostate{}));
    }

    // Fiber pool for parallel execution
    fiber::PriorityPool pool{4, 8};

    ExecutionResult result;

#ifdef VIBE_ROOM_INSTRUMENTATION
    result.instrumentation = std::make_unique<BlockInstrumentationData>();
    result.metrics.instrumentation = result.instrumentation.get();
#endif

    auto chain_ctx = ChainContext<EvmTraits<EVMC_CANCUN>>::debug_empty();

    auto receipts_result = execute_block<EvmTraits<EVMC_CANCUN>>(
        EthereumMainnet{},
        block,
        input.senders,
        authorities,
        *ctx.block_state,
        block_hash_buffer,
        pool.fiber_group(),
        result.metrics,
        call_tracers,
        state_tracers,
        chain_ctx);

    if (receipts_result.has_error()) {
        emit_error("execute_block failed", "execute");
        return std::nullopt;
    }

    result.receipts = std::move(receipts_result.value());

#ifdef VIBE_ROOM_INSTRUMENTATION
    // Detach pointer — ownership stays with result.instrumentation unique_ptr
    result.metrics.instrumentation = nullptr;

    // Sort per_tx by tx_index for deterministic output
    std::sort(result.instrumentation->per_tx.begin(),
              result.instrumentation->per_tx.end(),
              [](auto const &a, auto const &b) {
                  return a.tx_index < b.tx_index;
              });

    // Run post-hoc conflict detection
    result.instrumentation->conflicts =
        detect_conflicts_post_hoc(result.instrumentation->per_tx);
#endif

    return result;
}

// ── Phase 4: Format output ──────────────────────────────────────────────────

static json format_output(
    ExecutionResult const &exec_result,
    ParsedInput const &input,
    [[maybe_unused]] BlockInstrumentationData const *instrumentation)
{
    auto const &receipts = exec_result.receipts;
    auto const num_txs = input.transactions.size();

    json output;

    // results array
    json results_arr = json::array();
    uint64_t total_gas = 0;
    uint64_t prev_cumulative = 0;
    for (size_t i = 0; i < receipts.size(); ++i) {
        auto const &receipt = receipts[i];
        uint64_t gas_used = receipt.gas_used - prev_cumulative;
        prev_cumulative = receipt.gas_used;
        total_gas += gas_used;

        json tx_result;
        tx_result["success"] = (receipt.status == 1);
        tx_result["gas_used"] = gas_used;
        tx_result["output"] = "0x"; // output not easily extractable from Receipt
        tx_result["error"] = (receipt.status == 1) ? json(nullptr) : json("execution failed");
        tx_result["logs_count"] = receipt.logs_bloom.size() > 0 ? receipt.logs.size() : 0;
        results_arr.push_back(tx_result);
    }
    output["results"] = results_arr;

    // incarnations array
    json incarnations_arr = json::array();
#ifdef VIBE_ROOM_INSTRUMENTATION
    if (instrumentation) {
        for (auto const &tx_data : instrumentation->per_tx) {
            incarnations_arr.push_back(
                tx_data.incarnation_count > 1 ? tx_data.incarnation_count - 1 : 0);
        }
    } else
#endif
    {
        for (size_t i = 0; i < num_txs; ++i) {
            incarnations_arr.push_back(0);
        }
    }
    output["incarnations"] = incarnations_arr;

    // stats
    uint32_t num_conflicts = 0;
    uint32_t num_re_executions = 0;
#ifdef VIBE_ROOM_INSTRUMENTATION
    if (instrumentation) {
        for (auto const &tx_data : instrumentation->per_tx) {
            if (tx_data.incarnation_count > 1) {
                num_conflicts++;
                num_re_executions += (tx_data.incarnation_count - 1);
            }
        }
    } else
#endif
    {
        num_re_executions = exec_result.metrics.num_retries;
        num_conflicts = exec_result.metrics.num_retries > 0 ? 1 : 0;
    }

    json stats;
    stats["total_gas"] = total_gas;
    stats["num_transactions"] = num_txs;
    stats["num_conflicts"] = num_conflicts;
    stats["num_re_executions"] = num_re_executions;

#ifdef VIBE_ROOM_INSTRUMENTATION
    if (instrumentation) {
        json exec_times = json::array();
        for (auto const &tx_data : instrumentation->per_tx) {
            exec_times.push_back(tx_data.exec_time.count());
        }
        stats["per_tx_exec_time_us"] = exec_times;
    }
#endif

    output["stats"] = stats;

    // conflict_details
    json conflict_details;
#ifdef VIBE_ROOM_INSTRUMENTATION
    if (instrumentation) {
        json per_tx_arr = json::array();
        for (auto const &tx_data : instrumentation->per_tx) {
            per_tx_arr.push_back(tx_data.to_json());
        }
        conflict_details["per_tx"] = per_tx_arr;

        json conflicts_arr = json::array();
        for (auto const &conflict : instrumentation->conflicts) {
            conflicts_arr.push_back(conflict.to_json());
        }
        conflict_details["conflicts"] = conflicts_arr;
    } else
#endif
    {
        conflict_details["per_tx"] = json::array();
        conflict_details["conflicts"] = json::array();
    }
    output["conflict_details"] = conflict_details;

    return output;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // Handle --help and --version
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout <<
                "monad-vibe-cli — Headless JSON CLI for Monad parallel EVM execution\n\n"
                "Usage:\n"
                "  echo '{\"transactions\":[...],\"block_env\":{...}}' | monad-vibe-cli\n\n"
                "Input format (JSON on stdin):\n"
                "  {\n"
                "    \"transactions\": [{\n"
                "      \"sender\": \"0x...\",\n"
                "      \"to\": \"0x...\",       // null for contract creation\n"
                "      \"data\": \"0x...\",\n"
                "      \"value\": \"0\",\n"
                "      \"gas_limit\": 21000,\n"
                "      \"nonce\": 0,\n"
                "      \"gas_price\": \"1000000000\"\n"
                "    }],\n"
                "    \"block_env\": {\n"
                "      \"number\": 1,\n"
                "      \"coinbase\": \"0x...\",\n"
                "      \"timestamp\": 1700000000,\n"
                "      \"gas_limit\": 30000000,\n"
                "      \"base_fee\": \"0\",\n"
                "      \"difficulty\": \"0\"\n"
                "    }\n"
                "  }\n\n"
                "Output format (JSON on stdout):\n"
                "  { \"results\": [...], \"incarnations\": [...], \"stats\": {...}, \"conflict_details\": {...} }\n\n"
                "Options:\n"
                "  --help, -h       Show this help\n"
                "  --version, -v    Show version\n";
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
#ifdef GIT_COMMIT_HASH
            std::cout << "monad-vibe-cli " << GIT_COMMIT_HASH << std::endl;
#else
            std::cout << "monad-vibe-cli unknown" << std::endl;
#endif
            return 0;
        }
    }

    // Initialize quill logger — required before execute_block (LOG_ERROR dereferences global logger)
    auto stdout_handler = quill::stdout_handler();
    quill::Config cfg;
    cfg.default_handlers.emplace_back(stdout_handler);
    quill::configure(cfg);
    quill::start(true);
    quill::get_root_logger()->set_log_level(quill::LogLevel::Error);

    // Phase 1: Read and parse stdin
    std::string input_str;
    {
        std::ostringstream oss;
        oss << std::cin.rdbuf();
        input_str = oss.str();
    }

    if (input_str.empty()) {
        emit_error("empty input", "parse");
        return 1;
    }

    auto parsed = parse_input(input_str);
    if (!parsed) {
        return 1;
    }

    // Phase 2: Initialize state
    std::unique_ptr<ExecutionContext> ctx;
    try {
        ctx = init_state(*parsed);
    } catch (std::exception const &e) {
        emit_error(std::string("init failed: ") + e.what(), "init");
        return 1;
    }

    if (!ctx || !ctx->block_state) {
        emit_error("init_state returned null", "init");
        return 1;
    }

    // Phase 3: Execute
    auto exec_result_opt = execute(*ctx, *parsed);
    if (!exec_result_opt) {
        return 1;
    }

    // Phase 4: Format and output
#ifdef VIBE_ROOM_INSTRUMENTATION
    json output = format_output(*exec_result_opt, *parsed, exec_result_opt->instrumentation.get());
#else
    json output = format_output(*exec_result_opt, *parsed, nullptr);
#endif

    try {
        std::cout << output.dump() << std::endl;
    } catch (json::exception const &e) {
        emit_error(std::string("serialize error: ") + e.what(), "serialize");
        return 1;
    }

    return 0;
}
