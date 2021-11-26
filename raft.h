#ifndef raft_h
#define raft_h

#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <ctime>
#include <algorithm>
#include <thread>
#include <stdarg.h>

#include "rpc.h"
#include "raft_storage.h"
#include "raft_protocol.h"
#include "raft_state_machine.h"

inline long long get_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

template<typename state_machine, typename command>
class raft {

static_assert(std::is_base_of<raft_state_machine, state_machine>(), "state_machine must inherit from raft_state_machine");
static_assert(std::is_base_of<raft_command, command>(), "command must inherit from raft_command");


friend class thread_pool;

#define RAFT_LOG(fmt, args...) \
    do { \
        auto now = \
        std::chrono::duration_cast<std::chrono::milliseconds>(\
            std::chrono::system_clock::now().time_since_epoch()\
        ).count();\
        printf("[%ld][%s:%d][node %d term %d] " fmt "\n", now, __FILE__, __LINE__, my_id, current_term, ##args); \
    } while(0);

public:
    raft(
        rpcs* rpc_server,
        std::vector<rpcc*> rpc_clients,
        int idx, 
        raft_storage<command>* storage,
        state_machine* state    
    );
    ~raft();

    // start the raft node.
    // Please make sure all of the rpc request handlers have been registered before this method.
    void start();

    // stop the raft node. 
    // Please make sure all of the background threads are joined in this method.
    // Notice: you should check whether is server should be stopped by calling is_stopped(). 
    //         Once it returns true, you should break all of your long-running loops in the background threads.
    void stop();

    // send a new command to the raft nodes.
    // This method returns true if this raft node is the leader that successfully appends the log.
    // If this node is not the leader, returns false. 
    bool new_command(command cmd, int &term, int &index);

    // returns whether this node is the leader, you should also set the current term;
    bool is_leader(int &term);

    // save a snapshot of the state machine and compact the log.
    bool save_snapshot();

private:
    std::mutex mtx;                         // A big lock to protect the whole data structure
    std::mutex time_mtx;                    // A lock to protect last received rpc time
    ThrPool* thread_pool;
    raft_storage<command>* storage;         // To persist the raft log
    state_machine* state;                   // The state machine that applies the raft log, e.g. a kv store

    rpcs* rpc_server;                       // RPC server to recieve and handle the RPC requests
    std::vector<rpcc*> rpc_clients;         // RPC clients of all raft nodes including this node
    int my_id;                              // The index of this node in rpc_clients, start from 0

    std::atomic_bool stopped;

    enum raft_role {
        follower,
        candidate,
        leader
    };
    raft_role role;

    std::thread* background_election;
    std::thread* background_ping;
    std::thread* background_commit;
    std::thread* background_apply;

    // Your code here:

    int vote_num;                           // how much vote granted received
    long long last_received_rpc_time;       // when receive last rpc
    int election_timeout;                   // wait for this long, then timeout, range in [300, 500]
    int leader_id;                          // current leader id, -1 if none;
    // persistent state on all servers
    int current_term;                       // latest term server has seen
    int voted_for;                          // candidate id that received vote in current term (-1 if none)
    std::vector<log_entry<command>> log;    // log entries

    // volatile state on all servers
    int commit_idx;                         // index of highest log entry known to be commited
    int last_applied;                       // index of highest log entry applied to state machine

    // volatile state on leaders
    /** for each server, index of the next log entry to send to that server
     * init to leader last log index + 1
     **/
    std::vector<int> next_idx;    
    /** for each server, index of highest log entry known to be replicated on server
     * init to 0, increases monotonically
     **/          
    std::vector<int> match_idx;             


private:
    // RPC handlers
    int request_vote(request_vote_args arg, request_vote_reply& reply);

    int append_entries(append_entries_args<command> arg, append_entries_reply& reply);

    int install_snapshot(install_snapshot_args arg, install_snapshot_reply& reply);

    // RPC helpers
    void send_request_vote(int target, request_vote_args arg);
    void handle_request_vote_reply(int target, const request_vote_args& arg, const request_vote_reply& reply);

    void send_append_entries(int target, append_entries_args<command> arg);
    void handle_append_entries_reply(int target, const append_entries_args<command>& arg, const append_entries_reply& reply);

    void send_install_snapshot(int target, install_snapshot_args arg);
    void handle_install_snapshot_reply(int target, const install_snapshot_args& arg, const install_snapshot_reply& reply);


private:
    bool is_stopped();
    int num_nodes() {return rpc_clients.size();}

    // background workers    
    void run_background_ping();
    void run_background_election();
    void run_background_commit();
    void run_background_apply();

    // Your code here:
    inline void update_time();                      // 更新last received rpc time
    inline void reset_timeout();                    // 重置election timeout
    inline bool is_timeout();                       // 判断是否timeout
    void start_election();                          // 启动选举
    void heartbeat();                               // 向其他节点发送心跳或上任claim

};

template<typename state_machine, typename command>
raft<state_machine, command>::raft(rpcs* server, std::vector<rpcc*> clients, int idx, raft_storage<command> *storage, state_machine *state) :
    storage(storage),
    state(state),   
    rpc_server(server),
    rpc_clients(clients),
    my_id(idx),
    stopped(false),
    role(follower),
    current_term(0),
    background_election(nullptr),
    background_ping(nullptr),
    background_commit(nullptr),
    background_apply(nullptr),
    voted_for(-1),
    commit_idx(0),
    last_applied(0),
    vote_num(0),
    leader_id(-1)
{
    thread_pool = new ThrPool(32);

    // Register the rpcs.
    rpc_server->reg(raft_rpc_opcodes::op_request_vote, this, &raft::request_vote);
    rpc_server->reg(raft_rpc_opcodes::op_append_entries, this, &raft::append_entries);
    rpc_server->reg(raft_rpc_opcodes::op_install_snapshot, this, &raft::install_snapshot);

    // Your code here: 
    // Do the initialization
    reset_timeout();
    // log是1-base, 所以要先放个空的
    log.assign(1, log_entry<command>());
    next_idx.assign(num_nodes(), log.size());
    match_idx.assign(num_nodes(), 0);
    last_received_rpc_time = get_time();
}

template<typename state_machine, typename command>
raft<state_machine, command>::~raft() {
    if (background_ping) {
        delete background_ping;
    }
    if (background_election) {
        delete background_election;
    }
    if (background_commit) {
        delete background_commit;
    }
    if (background_apply) {
        delete background_apply;
    }
    delete thread_pool;    
}

/******************************************************************

                        Public Interfaces

*******************************************************************/

template<typename state_machine, typename command>
void raft<state_machine, command>::stop() {
    stopped.store(true);
    background_ping->join();
    background_election->join();
    background_commit->join();
    background_apply->join();
    thread_pool->destroy();
}

template<typename state_machine, typename command>
bool raft<state_machine, command>::is_stopped() {
    return stopped.load();
}

template<typename state_machine, typename command>
bool raft<state_machine, command>::is_leader(int &term) {
    term = current_term;
    return role == leader;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::start() {
    // Your code here:
    
    RAFT_LOG("start");
    this->background_election = new std::thread(&raft::run_background_election, this);
    this->background_ping = new std::thread(&raft::run_background_ping, this);
    this->background_commit = new std::thread(&raft::run_background_commit, this);
    this->background_apply = new std::thread(&raft::run_background_apply, this);
}

template<typename state_machine, typename command>
bool raft<state_machine, command>::new_command(command cmd, int &term, int &index) {
    // Your code here:
    
    term = current_term;
    return true;
}

template<typename state_machine, typename command>
bool raft<state_machine, command>::save_snapshot() {
    // Your code here:
    return true;
}



/******************************************************************

                         RPC Related

*******************************************************************/
template<typename state_machine, typename command>
int raft<state_machine, command>::request_vote(request_vote_args args, request_vote_reply& reply) {
    // Your code here:
    bool vote_granted = false;
    mtx.lock();
    int my_last_log_idx = log.size() - 1;
    int my_last_log_term = log[my_last_log_idx].term;   // ?这个term类型检查怎么失败了
    // 若term < current_term, 返回false
    if(args.term == current_term) {
        /** current term等于他人
         * 如果自己是candidate, 那么已经投票给自己, 拒绝投票
         * 如果自己是follower, 检查此轮是否已经投票(voted_for)
         *     若已经投票, 拒绝投票
         *     若还未投票, 比较日志新旧, 若他人更新, 则投票, 否则拒绝
         **/
        vote_granted = (role == follower && voted_for == -1 && (my_last_log_term < args.last_log_term || (my_last_log_term == args.last_log_term && my_last_log_idx == args.last_log_index)));
    } else if(args.term > current_term) {
        // current term低于他人, 成为follower, 更新term, 重置timeout
        role = follower;
        current_term = args.term;
        vote_num = 0;
        reset_timeout();
        // 进行投票, 若他人的日志更新, 则投票, 否则拒绝
        vote_granted = (my_last_log_term < args.last_log_term || (my_last_log_term == args.last_log_term && my_last_log_idx == args.last_log_index));
    }
    if(vote_granted) voted_for = args.candidate_id;
    mtx.unlock();
    reply.vote_granted = vote_granted;
    // 若投票, 则更新timeout
    if(vote_granted) {
        reset_timeout();
    }
    return raft_rpc_status::OK;
}


template<typename state_machine, typename command>
void raft<state_machine, command>::handle_request_vote_reply(int target, const request_vote_args& arg, const request_vote_reply& reply) {
    // Your code here:
    if(reply.vote_granted) {
        bool succeed = false;
        mtx.lock();
        if(role == candidate) {
            ++ vote_num;
            if(vote_num > num_nodes() / 2) {
                RAFT_LOG("get granted by %d followers, this candidate become a leader", vote_num);
                role = leader;
                succeed = true;
                vote_num = 0;
                leader_id = my_id;
                for(auto& idx : next_idx) {
                    idx = log.size();
                }
            }
        }
        mtx.unlock();
        // todo: 若当选, 通知其他server
        if(succeed) {
            heartbeat();
        }
    }
    return;
}


template<typename state_machine, typename command>
int raft<state_machine, command>::append_entries(append_entries_args<command> arg, append_entries_reply& reply) {
    // Your code here:
    bool success = false;
    mtx.lock();
    // 若term小于current, 拒绝此rpc
    if(arg.term >= current_term) {
        update_time();                      // 更新last receive rpc time
        if(arg.entries.empty()) {
            // 若entries为空, 收到当前term的leader发来的claim或heartbeat
            success = true;
            // 若当前不是follower, 则变成follower
            if(role != follower) {
                role = follower;
                current_term = arg.term;
                vote_num = 0;
                reset_timeout();
            }
        } else {
            // 若entries不为空, 则是收到当前term的leader发来的更新日志请求
            // todo: 处理日志更新
        }
    }
    reply.term = current_term;
    mtx.unlock();
    reply.success = success;
    return raft_rpc_status::OK;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::handle_append_entries_reply(int target, const append_entries_args<command>& arg, const append_entries_reply& reply) {
    // Your code here:
    if(!reply.success) {
        mtx.lock();
        if(reply.term > current_term) {
            // 发现更大的term, 变成follower, 更新current term
            role = follower;
            current_term = arg.term;
            vote_num = 0;
            reset_timeout();
        } else {
            // todo: 其他append entry失败的情况
        }
        mtx.unlock();
    } else {
        // todo: append entry成功的情况
    }
    return;
}


template<typename state_machine, typename command>
int raft<state_machine, command>::install_snapshot(install_snapshot_args args, install_snapshot_reply& reply) {
    // Your code here:
    return 0;
}


template<typename state_machine, typename command>
void raft<state_machine, command>::handle_install_snapshot_reply(int target, const install_snapshot_args& arg, const install_snapshot_reply& reply) {
    // Your code here:
    return;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::send_request_vote(int target, request_vote_args arg) {
    request_vote_reply reply;
    if (rpc_clients[target]->call(raft_rpc_opcodes::op_request_vote, arg, reply) == 0) {
        handle_request_vote_reply(target, arg, reply);
    } else {
        // RPC fails
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::send_append_entries(int target, append_entries_args<command> arg) {
    append_entries_reply reply;
    if (rpc_clients[target]->call(raft_rpc_opcodes::op_append_entries, arg, reply) == 0) {
        handle_append_entries_reply(target, arg, reply);
    } else {
        // RPC fails
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::send_install_snapshot(int target, install_snapshot_args arg) {
    install_snapshot_reply reply;
    if (rpc_clients[target]->call(raft_rpc_opcodes::op_install_snapshot, arg, reply) == 0) {
        handle_install_snapshot_reply(target, arg, reply);
    } else {
        // RPC fails
    }
}

/******************************************************************

                        Background Workers

*******************************************************************/

template<typename state_machine, typename command>
void raft<state_machine, command>::run_background_election() {
    // Check the liveness of the leader.
    // Work for followers and candidates.

    // Hints: You should record the time you received the last RPC.
    //        And in this function, you can compare the current time with it.
    //        For example:
    //        if (current_time - last_received_RPC_time > timeout) start_election();
    //        Actually, the timeout should be different between the follower (e.g. 300-500ms) and the candidate (e.g. 1s).

    
    while (true) {
        if (is_stopped()) return;
        // Your code here:
        switch(role) {
        case follower:
            if(is_timeout()) {
                RAFT_LOG("time out %d ms, this follower has become a candidate", election_timeout);
                start_election();
            }
            break;
        case candidate:
            if(is_timeout()) {
                RAFT_LOG("time out %d ms, this candidate remains a candidate", election_timeout);
                start_election();
            }
            break;
        default:
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }    
    

    return;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::run_background_commit() {
    // Send logs/snapshots to the follower.
    // Only work for the leader.

    // Hints: You should check the leader's last log index and the follower's next log index.        
    
    while (true) {
        if (is_stopped()) return;
        // Your code here:

        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }    
    
    return;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::run_background_apply() {
    // Apply committed logs the state machine
    // Work for all the nodes.

    // Hints: You should check the commit index and the apply index.
    //        Update the apply index and apply the log if commit_index > apply_index

    
    while (true) {
        if (is_stopped()) return;
        // Your code here:

        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }    
    return;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::run_background_ping() {
    // Send empty append_entries RPC to the followers.

    // Only work for the leader.
    
    while (true) {
        if (is_stopped()) return;
        // Your code here:
        if(role == leader) {
            heartbeat();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150)); // 每150ms ping一次followers
    }    
    return;
}


/******************************************************************

                        Other functions

*******************************************************************/

template<typename state_machine, typename command>
void raft<state_machine, command>::update_time() {
    time_mtx.lock();
    last_received_rpc_time = get_time();
    time_mtx.unlock();
}

template<typename state_machine, typename command>
void raft<state_machine, command>::reset_timeout() {
    if(role == candidate) {
        election_timeout = 1000;
    } else {
        // 生成范围为[300, 500]的随机数
        srand((unsigned)time(NULL));
        election_timeout = rand() % 201 + 300;
    }
    
}

template<typename state_machine, typename command>
bool raft<state_machine, command>::is_timeout() {
    return (get_time() - last_received_rpc_time) > election_timeout;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::start_election() {
    mtx.lock();
    role = candidate;                   // convert to candidate
    ++ current_term;                    // increase current term
    vote_num = 1;                       // vote for self
    reset_timeout();                    // reset timeout
    int my_last_log_idx = log.size() - 1;
    int my_last_log_term = log[my_last_log_idx].term;
    mtx.unlock();
    for(int i = 0;i < num_nodes();++ i) {
        if(i != my_id) {
            request_vote_args arg(current_term, my_id, my_last_log_idx, my_last_log_term);
            thread_pool->addObjJob(this, &raft::send_request_vote, i, arg);
        }
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::heartbeat() {
    mtx.lock();
    int num = num_nodes();
    mtx.unlock();
    for(int i = 0;i < num;++ i) {
        if(i != my_id) {
            append_entries_args<command> arg(current_term, my_id, 0, 0, std::vector<log_entry<command>>(), 0);
            thread_pool->addObjJob(this, &raft::send_append_entries, i, arg);
        }
    }
}


#endif // raft_h