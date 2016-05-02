#pragma once

#include <ctime>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

struct Job {
    int64_t id;
    std::string host_name;
    std::string service_description;

    std::string command_line;

    std::time_t time_scheduled;
    std::time_t time_expires;

    int check_type;
    int check_options;
    double latency;
};

using JobPtr = std::shared_ptr<Job>;
using JobWeakPtr = std::weak_ptr<Job>;

bool operator>(const JobWeakPtr& lhs, const JobWeakPtr& rhs);

using CheckMap = std::unordered_map<std::string, JobPtr>;

struct Result {
    std::string output;
    int return_code;
    struct timeval start_time;
    struct timeval finish_time;
    int exited_ok;
    int early_timeout;
};

void processTimeout(JobPtr job);
void processJobError(JobPtr job, std::string errmsg);
void processResult(JobPtr job, const Result& result);

struct JobQueue {
    std::unordered_map<uint64_t, JobPtr> job_queue;
    std::priority_queue<JobWeakPtr, std::vector<JobWeakPtr>, std::greater<JobWeakPtr>> expiry_queue;

    uint64_t addCheck(JobPtr check) {
        auto cur_id = next_id++;
        job_queue.insert({cur_id, check});
        expiry_queue.emplace(check);
        return cur_id;
    }

    JobPtr getCheck(uint64_t job_id) {
        auto it = job_queue.find(job_id);
        if (it == job_queue.end()) {
            throw std::out_of_range("Could not find job ID in job queue");
        }
        JobPtr ret = it->second;
        job_queue.erase(it);
        return ret;
    }

    JobPtr getNextExpiredJob(std::time_t time) {
        if (expiry_queue.size() == 0)
            return nullptr;

        auto ptr = expiry_queue.top().lock();
        if (ptr == nullptr) {
            expiry_queue.pop();
            return nullptr;
        }

        if (ptr->time_expires > time)
            return nullptr;

        expiry_queue.pop();
        return ptr;
    }

private:
    static CheckMap _placeholder;
    static uint64_t next_id;
};

int handleNebNagiosCheckInitiate(int which, void* obj);
void processTimedOutChecks();
std::shared_ptr<JobQueue> getJobQueue();
void dispatchJob(JobPtr job, std::string executor);
