#include <ctime>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "mqexec_shared.h"

using JobMap = std::unordered_map<std::string, JobPtr>;

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

    JobPtr getNextExpiredJob(TimePoint time) {
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
    static JobMap _placeholder;
    static uint64_t next_id;
};

int handleNebNagiosCheckInitiate(int which, void* obj);
void processTimedOutChecks();
std::shared_ptr<JobQueue> getJobQueue();
void dispatchJob(JobPtr job, std::string executor);
