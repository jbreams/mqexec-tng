#pragma once
#include <memory>
#include <string>
#include <unordered_map>

struct Job {
    int64_t id;
    std::string host_name;
    std::string service_description;

    std::string command_line;

    int64_t time_scheduled;
    int64_t time_expires;

    int check_type;
    int check_options;
    double latency;

    template <class Archive>
    void serialize( Archive & ar ) {
        ar( id &
            host_name &
            service_description &
            time_scheduled &
            time_expires
          );
    }
};

using JobPtr = std::shared_ptr<Job>;
using JobWeakPtr = std::weak_ptr<Job>;

bool operator>(const JobWeakPtr& lhs, const JobWeakPtr& rhs);

using JobMap = std::unordered_map<std::string, JobPtr>;

struct Result {
    uint64_t id;
    std::string output;
    int return_code;
    int64_t start_time;
    int64_t finish_time;

    template <class Archive>
    void serialize( Archive & ar ) {
        ar ( id &
             output &
             return_code &
             start_time &
             finish_time
           );
    }
};


