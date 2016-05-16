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

    uint64_t addCheck(JobPtr check) {
        auto cur_id = ++next_id;
        check->id = cur_id;
        job_queue.insert({cur_id, check});
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

private:
    static JobMap _placeholder;
    static uint64_t next_id;
};

int handleNebNagiosCheckInitiate(int which, void* obj);
std::shared_ptr<JobQueue> getJobQueue();
void dispatchJob(JobPtr job, std::string executor);
void scheduleTimeout(JobPtr jobptr);
