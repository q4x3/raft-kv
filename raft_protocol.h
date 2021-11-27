#ifndef raft_protocol_h
#define raft_protocol_h

#include "rpc.h"
#include "raft_state_machine.h"

enum raft_rpc_opcodes {
    op_request_vote = 0x1212,
    op_append_entries = 0x3434,
    op_install_snapshot = 0x5656
};

enum raft_rpc_status {
   OK,
   RETRY,
   RPCERR,
   NOENT,
   IOERR
};

class request_vote_args {
public:
    // Your code here
    int term;           // candidate's term
    int candidate_id;   // candidate requesting vote
    int last_log_index; // index of candidate's last log entry
    int last_log_term;  // term of candidate's last log entry
    request_vote_args() {}
    request_vote_args(int _term, int _id, int _last_idx, int _last_term):
                    term(_term), candidate_id(_id), last_log_index(_last_idx), last_log_term(_last_term) {}
};

marshall& operator<<(marshall &m, const request_vote_args& args);

unmarshall& operator>>(unmarshall &u, request_vote_args& args);


class request_vote_reply {
public:
    // Your code here
    int term;           // current term, for candidate to update itself
    bool vote_granted;  // true means candidate received vote
};

marshall& operator<<(marshall &m, const request_vote_reply& reply);

unmarshall& operator>>(unmarshall &u, request_vote_reply& reply);

template<typename command>
class log_entry {
public:
    // Your code here
    int term;
    command cmd;
    log_entry():term(-1) {}
    log_entry(int term, command cmd): term(term), cmd(cmd) {}
};

template<typename command>
marshall& operator<<(marshall &m, const log_entry<command>& entry) {
    // Your code here
    m << entry.term;
    m << entry.cmd;
    return m;
}

template<typename command>
unmarshall& operator>>(unmarshall &u, log_entry<command>& entry) {
    // Your code here
    u >> entry.term;
    u >> entry.cmd;
    return u;
}

template<typename command>
class append_entries_args {
public:
    // Your code here
    int term;                                   // leader's term
    int leader_id;                              // so that follower can redirect
    int prev_log_idx;
    int prev_log_term;
    std::vector<log_entry<command>> entries;
    int leader_commit;
    append_entries_args():entries(std::vector<log_entry<command>>()) {}
    append_entries_args(int _term, int _id, int _prev_idx, int _prev_term, std::vector<log_entry<command>> _entries, int _idx):
        term(_term), leader_id(_id), prev_log_idx(_prev_idx), prev_log_term(_prev_term), entries(_entries), leader_commit(_idx) {}
};

template<typename command>
marshall& operator<<(marshall &m, const append_entries_args<command>& args) {
    // Your code here
    m << args.term;
    m << args.leader_id;
    m << args.prev_log_idx;
    m << args.prev_log_term;
    m << args.entries;
    m << args.leader_commit;
    return m;
}

template<typename command>
unmarshall& operator>>(unmarshall &u, append_entries_args<command>& args) {
    // Your code here
    u >> args.term;
    u >> args.leader_id;
    u >> args.prev_log_idx;
    u >> args.prev_log_term;
    u >> args.entries;
    u >> args.leader_commit;
    return u;
}

class append_entries_reply {
public:
    // Your code here
    int term;                       // current term, for leader to update itself
    int success;                    // 1 if follower contained entry matching prev_log_index and prev_log_term, 2 if heartbeat, 0 if fail.
    int index;                      // index of last log after append, only available if success = 1
};

marshall& operator<<(marshall &m, const append_entries_reply& reply);

unmarshall& operator>>(unmarshall &u, append_entries_reply& reply);


class install_snapshot_args {
public:
    // Your code here
};

marshall& operator<<(marshall &m, const install_snapshot_args& args);
unmarshall& operator>>(unmarshall &m, install_snapshot_args& args);


class install_snapshot_reply {
public:
    // Your code here
};

marshall& operator<<(marshall &m, const install_snapshot_reply& reply);
unmarshall& operator>>(unmarshall &m, install_snapshot_reply& reply);


#endif // raft_protocol_h