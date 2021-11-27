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
    std::mutex logs_mtx;                    // A lock to protect logs

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
    inline void update_time();                                                      // 更新last received rpc time
    inline void reset_timeout();                                                    // 重置election timeout
    inline bool is_timeout();                                                       // 判断是否timeout
    void start_election();                                                          // 启动选举
    void heartbeat();                                                               // 向其他节点发送心跳或上任claim
    void append_log(const log_entry<command>& entry, int& index);                   // 新增log, index为新增log的位置
    int append_log(const append_entries_args<command>& args);                       // 根据args新增多个log, 若失败则返回false
    void apply_log();                                                               // 将已经commit的log apply到state machine
    void notify_commit();                                                           // 通知所有节点更新commit_idx
    void send_append_rpc(int target);                                               // 向target server发送append entries rpc
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
    if(role != leader) return false;
    term = current_term;
    append_log(log_entry<command>(term, cmd), index);
    RAFT_LOG("In new_command, leader successfully append log");
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
    RAFT_LOG("Request vote have the big mtx");
    int my_last_log_idx = log.size() - 1;
    int my_last_log_term = log[my_last_log_idx].term;
    // 若term < current_term, 返回false
    if(args.term == current_term) {
        /** current term等于他人
         * 如果自己是candidate, 那么已经投票给自己, 拒绝投票
         * 如果自己是follower, 检查此轮是否已经投票(voted_for)
         *     若已经投票, 拒绝投票
         *     若还未投票, 比较日志新旧, 若他人更新, 则投票, 否则拒绝
         **/
        vote_granted = (role == follower && voted_for == -1 && (my_last_log_term < args.last_log_term || (my_last_log_term == args.last_log_term && my_last_log_idx <= args.last_log_index)));
        // if(!vote_granted) {
        //     RAFT_LOG("Refuse term %d, My role %d, voted for %d, my term %d, arg term %d, my idx %d, arg idx %d", args.term, role, voted_for, my_last_log_term, args.last_log_term, my_last_log_idx, args.last_log_index);
        // }
    } else if(args.term > current_term) {
        // current term低于他人, 成为follower, 更新term, 重置timeout
        role = follower;
        current_term = args.term;
        vote_num = 0;
        voted_for = -1;
        reset_timeout();
        // 进行投票, 若他人的日志更新, 则投票, 否则拒绝
        vote_granted = (my_last_log_term < args.last_log_term || (my_last_log_term == args.last_log_term && my_last_log_idx <= args.last_log_index));
        // if(!vote_granted) {
        //     RAFT_LOG("Refuse term %d, My role %d, voted for %d, my term %d, arg term %d, my idx %d, arg idx %d", args.term, role, voted_for, my_last_log_term, args.last_log_term, my_last_log_idx, args.last_log_index);
        // }
    }
    if(vote_granted) voted_for = args.candidate_id;
    mtx.unlock();
    RAFT_LOG("Request vote release the big mtx");
    reply.vote_granted = vote_granted;
    // 若投票, 则更新last received rpc time
    if(vote_granted) {
        // RAFT_LOG("I voted for %d in term %d", voted_for, current_term);
        update_time();
    }
    return raft_rpc_status::OK;
}


template<typename state_machine, typename command>
void raft<state_machine, command>::handle_request_vote_reply(int target, const request_vote_args& arg, const request_vote_reply& reply) {
    // Your code here:
    if(reply.vote_granted) {
        bool succeed = false;
        mtx.lock();
        RAFT_LOG("Handle request vote have the big mtx");
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
                for(auto& idx : match_idx) {
                    idx = 0;
                }
            }
        }
        mtx.unlock();
        RAFT_LOG("Handle request vote release the big mtx");
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
    int success = 0;
    mtx.lock();
    RAFT_LOG("Append entries have the big mtx");
    // 若term小于current, 拒绝此rpc
    if(arg.term >= current_term) {
        update_time();                      // 更新last receive rpc time
        leader_id = arg.leader_id;          // 更新leader id
        current_term = arg.term;            // 更新current term
        // 若当前不是follower, 则变成follower
        if(role != follower) {
            role = follower;
            vote_num = 0;
            voted_for = -1;
            reset_timeout();
        }
        if(arg.entries.empty()) {
            // 若entries为空, 收到当前term的leader发来的claim或heartbeat或commit通知
            // 若leader commit不为-1, 则更新commit, success设为2
            // 若leader commit为-1, 则success设为2
            success = 2;
            if(arg.leader_commit != -1) {
                success = 2;
                logs_mtx.lock();
                RAFT_LOG("Append entries have the log mtx");
                commit_idx = arg.leader_commit;
                logs_mtx.unlock();
                RAFT_LOG("Append entries release the log mtx");
                RAFT_LOG("I receive notify update commit_idx to %d", commit_idx);
            } else {
                // RAFT_LOG("Successfully receive leaders heartbeat");
            }
        } else {
            // 若entries不为空, 则是收到当前term的leader发来的更新日志请求
            success = append_log(arg);
            reply.index = log.size() - 1;
        }
    }
    reply.term = current_term;
    mtx.unlock();
    RAFT_LOG("Append entries release the big mtx");
    reply.success = success;
    // if(!reply.success) RAFT_LOG("refuse leader ping in its id %d term %d", arg.leader_id, arg.term);
    return raft_rpc_status::OK;
}

template<typename state_machine, typename command>
void raft<state_machine, command>::handle_append_entries_reply(int target, const append_entries_args<command>& arg, const append_entries_reply& reply) {
    // Your code here:
    mtx.lock();
    RAFT_LOG("Handle append entries have the big mtx");
    // append entries rpc失败, 两种情况
    if(reply.success == 0) {
        if(reply.term > current_term) {
            // 发现更大的term, 变成follower, 更新current term
            role = follower;
            current_term = arg.term;
            vote_num = 0;
            voted_for = -1;
            leader_id = arg.leader_id;
            reset_timeout();
            RAFT_LOG("Append rpc fail because I'm old");
        } else {
            // 因为日志term不匹配而append失败, 更新next
            -- next_idx[target];
            RAFT_LOG("Append rpc fail because of slower log");
        }
    }
    // append log成功
    else if(reply.success == 1) {
        next_idx[target] = reply.index + 1;
        match_idx[target] = reply.index;
        // 判断是否commit, 若commit, 则通知followers更新commit_idx
        // todo: 这里只简单的对commit_idx+1进行了试探, 可以考虑找match_idx
        // 的中位数
        // for(int i = 0;i < num;++ i) {
        //     if(i != my_id && match_idx[i] >= commit_idx + 1) ++ cnt;
        // }
        // if(cnt > num / 2) {

        // }
        int num = num_nodes();
        std::vector<int> tmp(match_idx);
        std::nth_element(tmp.begin(), tmp.begin() + (num - 1) / 2, tmp.end());
        int mid = tmp[(num - 1) / 2];
        if(commit_idx < mid) {
            commit_idx = mid;
            RAFT_LOG("Successfully commit with index %d", commit_idx);
            notify_commit();
            apply_log();
        }
    }
    // 若为2, heartbeat的ack
    mtx.unlock();
    RAFT_LOG("Handle append entries release the big mtx");
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
                RAFT_LOG("time out %d ms with %d votes, this candidate remains a candidate", election_timeout, vote_num);
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
        if(role == leader) {
            int num = num_nodes();
            for(int i = 0;i < num;++ i) {
                if(i != my_id && next_idx[i] < log.size()) {
                    send_append_rpc(i);
                }
            }
        }
        
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
        if(commit_idx > last_applied) {
            logs_mtx.lock();
            RAFT_LOG("Run background apply have the log mtx");
            apply_log();
            logs_mtx.unlock();
            RAFT_LOG("Run background apply release the log mtx");
        }
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
            //RAFT_LOG("Leader run background ping, why nobody cares?");
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
        srand((unsigned)time(NULL) + my_id);
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
    RAFT_LOG("Start election have the big mtx");
    role = candidate;                   // convert to candidate
    ++ current_term;                    // increase current term
    voted_for = my_id;
    vote_num = 1;                       // vote for self
    reset_timeout();                    // reset timeout
    int my_last_log_idx = log.size() - 1;
    int my_last_log_term = log[my_last_log_idx].term;
    mtx.unlock();
    RAFT_LOG("Start election release the big mtx");
    for(int i = 0;i < num_nodes();++ i) {
        if(i != my_id) {
            request_vote_args arg(current_term, my_id, my_last_log_idx, my_last_log_term);
            thread_pool->addObjJob(this, &raft::send_request_vote, i, arg);
        }
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::heartbeat() {
    int num = num_nodes();
    //RAFT_LOG("leader ping servers");
    for(int i = 0;i < num;++ i) {
        if(i != my_id) {
            append_entries_args<command> arg(current_term, my_id, 0, 0, std::vector<log_entry<command>>(), -1);
            thread_pool->addObjJob(this, &raft::send_append_entries, i, arg);
        }
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::append_log(const log_entry<command>& entry, int& index) {
    logs_mtx.lock();
    RAFT_LOG("Append log 1 have the log mtx");
    log.push_back(entry);
    index = log.size() - 1;
    logs_mtx.unlock();
    RAFT_LOG("Append log 1 release the log mtx");
}

template<typename state_machine, typename command>
int raft<state_machine, command>::append_log(const append_entries_args<command>& args) {
    // 检查term的工作在调用处做
    int prev_log_idx = args.prev_log_idx;
    logs_mtx.lock();
    RAFT_LOG("Append log have the log mtx");
    commit_idx = args.leader_commit;
    // 日志进度比leader慢, 拒绝
    if(prev_log_idx > log.size() - 1) {
        RAFT_LOG("I fail to append log because of slower log");
        logs_mtx.unlock();
        RAFT_LOG("Append log release the log mtx");
        return 0;
    }
    // 日志进度大于等于leader, 分情况
    if(args.prev_log_term == log[prev_log_idx].term) {
        // 日志与leader一致, 删除一致部分之后的内容, append args里的entry
        while((log.size() - 1) != prev_log_idx) log.pop_back();
        for(const auto& entry : args.entries) {
            log.push_back(entry);
        }
        int tmp = log.size() - 1;
        RAFT_LOG("I append log successfully, last log index is %d", tmp);
        logs_mtx.unlock();
        RAFT_LOG("Append log release the log mtx");
        return 1;
    } else {
        // 日志与leader不一致
        RAFT_LOG("I fail to append log because of nonconsistant log");
        logs_mtx.unlock();
        RAFT_LOG("Append log release the log mtx");
        return 0;
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::apply_log() {
    for(int i = last_applied + 1;i <= commit_idx;++ i) {
        int tmp = log.size();
        state->apply_log(log[i].cmd);
    }
    last_applied = commit_idx;
    RAFT_LOG("Successfully apply log %d", last_applied);
}

template<typename state_machine, typename command>
void raft<state_machine, command>::notify_commit() {
    int num = num_nodes();
    for(int i = 0;i < num;++ i) {
        if(i != my_id) {
            append_entries_args<command> arg(current_term, my_id, 0, 0, std::vector<log_entry<command>>(), commit_idx);
            thread_pool->addObjJob(this, &raft::send_append_entries, i, arg);
        }
    }
}

template<typename state_machine, typename command>
void raft<state_machine, command>::send_append_rpc(int target) {
    mtx.lock();
    RAFT_LOG("Send append rpc have the big mtx");
    int prev_log_idx = next_idx[target] - 1;
    int prev_log_term = log[prev_log_idx].term;
    mtx.unlock();
    RAFT_LOG("Send append rpc release the big mtx");
    RAFT_LOG("Leader send append rpc to server %d, prev_log_idx %d", target, prev_log_idx);
    std::vector<log_entry<command>> entries(log.begin() + prev_log_idx + 1, log.end());
    append_entries_args<command> arg(current_term, my_id, prev_log_idx, prev_log_term, entries, commit_idx);
    thread_pool->addObjJob(this, &raft::send_append_entries, target, arg);
}

#endif // raft_h