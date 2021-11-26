#include "raft_protocol.h"

marshall& operator<<(marshall &m, const request_vote_args& args) {
    m << args.term;
    m << args.candidate_id;
    m << args.last_log_index;
    m << args.last_log_term;
    return m;
}
unmarshall& operator>>(unmarshall &u, request_vote_args& args) {
    u >> args.term;
    u >> args.candidate_id;
    u >> args.last_log_index;
    u >> args.last_log_term;
    return u;
}

marshall& operator<<(marshall &m, const request_vote_reply& reply) {
    m << reply.term;
    m << reply.vote_granted;
    return m;
}

unmarshall& operator>>(unmarshall &u, request_vote_reply& reply) {
    u >> reply.term;
    u >> reply.vote_granted;
    return u;
}

marshall& operator<<(marshall &m, const append_entries_reply& reply) {
    m << reply.term;
    m << reply.success;
    return m;
}
unmarshall& operator>>(unmarshall &u, append_entries_reply& reply) {
    u >> reply.term;
    u >> reply.success;
    return u;
}

marshall& operator<<(marshall &m, const install_snapshot_args& args) {
    // Your code here

    return m;
}

unmarshall& operator>>(unmarshall &u, install_snapshot_args& args) {
    // Your code here

    return u; 
}

marshall& operator<<(marshall &m, const install_snapshot_reply& reply) {
    // Your code here

    return m;
}

unmarshall& operator>>(unmarshall &u, install_snapshot_reply& reply) {
    // Your code here

    return u;
}