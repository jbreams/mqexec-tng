#pragma once
#include <memory>
#include <string>
#include <chrono>

using TimePoint = std::chrono::time_point<std::chrono::system_clock>;

struct Job {
    uint64_t id;
    std::string host_name;
    std::string service_description;

    std::string command_line;

    TimePoint time_scheduled;
    TimePoint time_expires;

    int check_type;
    int check_options;
    double latency;

    template <class Archive>
    void serialize( Archive& ar ) {
        ar(id, host_name, service_description, command_line, time_scheduled, time_expires);
    }
};

using JobPtr = std::shared_ptr<Job>;
using JobWeakPtr = std::weak_ptr<Job>;

bool operator>(const JobWeakPtr& lhs, const JobWeakPtr& rhs);

struct Result {
    uint64_t id;
    std::string output;
    int return_code;
    TimePoint start_time;
    TimePoint finish_time;

    struct timeval getStartTimeVal() const {
        struct timeval ret;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(start_time.time_since_epoch());
        ret.tv_sec  = seconds.count();
        ret.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(
            start_time.time_since_epoch() - seconds).count();
        return ret;
    }

    struct timeval getFinishTimeVal() const {
        struct timeval ret;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(finish_time.time_since_epoch());
        ret.tv_sec  = seconds.count();
        ret.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(
            finish_time.time_since_epoch() - seconds).count();
        return ret;
    }
    template <class Archive>
    void serialize( Archive& ar ) {
        ar(id, output, return_code, start_time, finish_time);
    }
};

