#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>

namespace cache {
namespace cluster {

// Raft node states
enum class RaftState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

// Raft log entry
struct LogEntry {
    uint64_t term;
    uint64_t index;
    std::string command;
    std::chrono::system_clock::time_point timestamp;
    
    LogEntry() : term(0), index(0) {}
    LogEntry(uint64_t t, uint64_t i, const std::string& cmd)
        : term(t), index(i), command(cmd), 
          timestamp(std::chrono::system_clock::now()) {}
    
    std::string serialize() const;
    static Result<LogEntry> deserialize(const std::string& data);
};

// Vote request message
struct VoteRequest {
    uint64_t term;
    NodeId candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
};

// Vote response message
struct VoteResponse {
    uint64_t term;
    bool vote_granted;
};

// Append entries request message
struct AppendEntriesRequest {
    uint64_t term;
    NodeId leader_id;
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    std::vector<LogEntry> entries;
    uint64_t leader_commit;
};

// Append entries response message
struct AppendEntriesResponse {
    uint64_t term;
    bool success;
    uint64_t match_index;
};

// Raft configuration
struct RaftConfig {
    NodeId node_id;
    std::vector<NodeId> cluster_members;
    uint32_t election_timeout_min_ms;
    uint32_t election_timeout_max_ms;
    uint32_t heartbeat_interval_ms;
    std::string log_directory;
    size_t max_log_entries;
    
    RaftConfig() 
        : election_timeout_min_ms(150), election_timeout_max_ms(300),
          heartbeat_interval_ms(50), max_log_entries(10000) {}
};

// Raft callbacks
class RaftCallbacks {
public:
    virtual ~RaftCallbacks() = default;
    
    // Called when this node becomes leader
    virtual void on_become_leader() = 0;
    
    // Called when this node loses leadership
    virtual void on_lose_leadership() = 0;
    
    // Called when a log entry is committed
    virtual void on_log_committed(const LogEntry& entry) = 0;
    
    // Called when cluster membership changes
    virtual void on_membership_change(const std::vector<NodeId>& new_members) = 0;
};

// Raft network interface
class RaftNetwork {
public:
    virtual ~RaftNetwork() = default;
    
    virtual Result<VoteResponse> send_vote_request(const NodeId& target, const VoteRequest& request) = 0;
    virtual Result<AppendEntriesResponse> send_append_entries(const NodeId& target, const AppendEntriesRequest& request) = 0;
    virtual Result<void> broadcast_append_entries(const AppendEntriesRequest& request, 
                                                  std::vector<AppendEntriesResponse>& responses) = 0;
};

// Raft consensus implementation
class Raft {
public:
    Raft(const RaftConfig& config, 
         std::unique_ptr<RaftNetwork> network,
         std::shared_ptr<RaftCallbacks> callbacks);
    ~Raft();
    
    // Lifecycle
    Result<void> start();
    Result<void> stop();
    bool is_running() const { return is_running_; }
    
    // State queries
    RaftState get_state() const { return state_; }
    bool is_leader() const { return state_ == RaftState::LEADER; }
    NodeId get_leader_id() const { return leader_id_; }
    uint64_t get_current_term() const { return current_term_; }
    
    // Log operations
    Result<uint64_t> append_entry(const std::string& command);
    Result<void> wait_for_commit(uint64_t index, uint32_t timeout_ms = 5000);
    
    // Cluster membership
    Result<void> add_member(const NodeId& node_id);
    Result<void> remove_member(const NodeId& node_id);
    std::vector<NodeId> get_cluster_members() const;
    
    // Message handling (called by network layer)
    VoteResponse handle_vote_request(const VoteRequest& request);
    AppendEntriesResponse handle_append_entries(const AppendEntriesRequest& request);
    
    // Statistics
    uint64_t get_log_size() const;
    uint64_t get_commit_index() const { return commit_index_; }
    uint64_t get_last_applied() const { return last_applied_; }
    
private:
    RaftConfig config_;
    std::unique_ptr<RaftNetwork> network_;
    std::shared_ptr<RaftCallbacks> callbacks_;
    
    // Persistent state
    std::atomic<uint64_t> current_term_;
    NodeId voted_for_;
    std::vector<LogEntry> log_;
    
    // Volatile state
    std::atomic<RaftState> state_;
    NodeId leader_id_;
    std::atomic<uint64_t> commit_index_;
    std::atomic<uint64_t> last_applied_;
    
    // Leader state
    std::unordered_map<NodeId, uint64_t> next_index_;
    std::unordered_map<NodeId, uint64_t> match_index_;
    
    // Threading
    mutable std::shared_mutex mutex_;
    std::condition_variable_any state_cv_;
    std::thread election_thread_;
    std::thread heartbeat_thread_;
    std::thread apply_thread_;
    std::atomic<bool> stop_threads_;
    std::atomic<bool> is_running_;
    
    // Election timing
    std::chrono::steady_clock::time_point last_heartbeat_;
    std::chrono::milliseconds election_timeout_;
    
    // Private methods
    void election_loop();
    void heartbeat_loop();
    void apply_loop();
    
    void start_election();
    void become_leader();
    void become_follower(uint64_t term, const NodeId& leader = "");
    void become_candidate();
    
    void send_heartbeats();
    void replicate_to_followers();
    void apply_committed_entries();
    
    bool log_is_up_to_date(uint64_t last_log_index, uint64_t last_log_term) const;
    void update_commit_index();
    void reset_election_timeout();
    
    // Persistence
    Result<void> persist_state();
    Result<void> load_state();
    Result<void> persist_log();
    Result<void> load_log();
    
    std::string get_state_file_path() const;
    std::string get_log_file_path() const;
};

} // namespace cluster
} // namespace cache