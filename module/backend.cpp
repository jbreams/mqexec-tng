#include <cstring>
#include <cstdlib>

#define NSCORE 1
#include "nebstructs.h"
#include "nebcallbacks.h"
#include "nebmodules.h"
#include "nebmods.h"
#ifdef HAVE_ICINGA
#include "icinga.h"
#else
#include "nagios.h"
#endif
#include "objects.h"
#include "broker.h"
#include "neberrors.h"

#include "common.h"

bool operator>(const JobWeakPtr& lhs, const JobWeakPtr& rhs) {
    auto rlhs = lhs.lock();
    auto rrhs = rhs.lock();
    if (!rlhs || !rlhs)
        return true;
    return rlhs->time_expires > rrhs->time_expires;
}

std::shared_ptr<JobQueue> job_queue;
std::shared_ptr<JobQueue> getJobQueue() {
    if (job_queue == nullptr)
        job_queue = std::make_shared<JobQueue>();

    return job_queue;
}

uint64_t JobQueue::next_id = 0;
CheckMap JobQueue::_placeholder = CheckMap{};

int64_t default_timeout = 120;

const char* engine_name = "MQexec NG";
const char* mqexec_source_name(const void* unused) {
    return engine_name;
}

struct check_engine mqexec_check_engine = {
    const_cast<char*>(engine_name), mqexec_source_name, std::free};

void processJobError(JobPtr job, std::string errmsg) {
    const Result res = {errmsg,
                        job->service_description.empty() ? 3 : 1,
                        {job->time_scheduled, 0},
                        {std::time(nullptr), 0},
                        1,
                        0};
    processResult(job, res);
}

void processTimeout(JobPtr job) {
    const Result res = {"Check timed out",
                        job->service_description.empty() ? 3 : 1,
                        {job->time_scheduled, 0},
                        {std::time(nullptr), 0},
                        1,
                        0};
    processResult(job, res);
}

namespace {

void sendResultToNagios(check_result& new_result) {
    new_result.engine = &mqexec_check_engine;
    process_check_result(&new_result);
    free_check_result(&new_result);
}

void processResult(JobPtr job, const Result& result) {
    check_result new_result;
    init_check_result(&new_result);
    new_result.output_file = NULL;
    new_result.output_file_fp = NULL;

    new_result.host_name = strdup(job->host_name.c_str());
    if (!job->service_description.empty()) {
        new_result.service_description = strdup(job->service_description.c_str());
        new_result.object_check_type = SERVICE_CHECK;
        new_result.return_code = result.return_code;
    } else {
        new_result.object_check_type = HOST_CHECK;
        new_result.return_code = result.return_code;
    }

    new_result.output = strdup(result.output.c_str());
    new_result.start_time = result.start_time;
    new_result.finish_time = result.finish_time;

    new_result.exited_ok = result.exited_ok;
    new_result.early_timeout = result.early_timeout;

    new_result.check_type = job->check_type;
    new_result.check_options = job->check_options;
    new_result.latency = job->latency;

    sendResultToNagios(new_result);
}

// This function does what run_sync_host_check in checks.c of Nagios would
// do between HOSTCHECK_ASYNC_PRE_CHECK and HOSTCHECK_INITIATE.
// It's here to fix things up and produce the fully parsed command line.
int fixup_async_presync_hostcheck(host* hst, char** processed_command) {
    nagios_macros mac;
    char* raw_command = NULL;
    int macro_options = STRIP_ILLEGAL_MACRO_CHARS | ESCAPE_MACRO_CHARS;

    /* clear check options - we don't want old check options retained */
    /* only clear options if this was a scheduled check - on demand check options shouldn't affect
     * retained info */
    // The above comments don't many any sense. As of Nagios 4.0.8, all checks that reach
    // this code path are scheduled checks - so I've taken out the if statement.
    hst->check_options = CHECK_OPTION_NONE;

    /* adjust host check attempt */
    adjust_host_check_attempt(hst, TRUE);

    /* grab the host macro variables */
    memset(&mac, 0, sizeof(mac));
    grab_host_macros_r(&mac, hst);

    /* get the raw command line */
    get_raw_command_line_r(
        &mac, hst->check_command_ptr, hst->check_command, &raw_command, macro_options);
    if (raw_command == NULL) {
        clear_volatile_macros_r(&mac);
        log_debug_info(
            DEBUGL_CHECKS, 0, "Raw check command for host '%s' was NULL - aborting.\n", hst->name);
        return ERROR;
    }

    /* process any macros contained in the argument */
    process_macros_r(&mac, raw_command, processed_command, macro_options);
    my_free(raw_command);
    if (processed_command == NULL) {
        clear_volatile_macros_r(&mac);
        log_debug_info(DEBUGL_CHECKS,
                       0,
                       "Processed check command for host '%s' was NULL - aborting.\n",
                       hst->name);
        return ERROR;
    }

    clear_volatile_macros_r(&mac);

    return 0;
}

std::string getExecutorName(customvariablesmember* vars) {
    while (vars != nullptr) {
        if (std::strcmp(vars->variable_name, "_MQEXEC_EXECUTOR") == 0)
            return std::string{vars->variable_value};
        vars = vars->next;
    }
    return "";
}

void processHostCheckInitiate(nebstruct_host_check_data* state) {
    host* obj = (host*)state->object_ptr;

    char* processed_command = nullptr;
    double old_latency = obj->latency;
    int old_current_attempt = obj->current_attempt;
    obj->latency = state->latency;

    if (fixup_async_presync_hostcheck(obj, &processed_command) != 0)
        return;
    std::unique_ptr<char> processed_command_guard(processed_command);

    auto job = std::make_shared<Job>();
    job->host_name = std::string(state->host_name);
    job->command_line = std::string(processed_command);
    job->time_scheduled = state->timestamp.tv_sec;
    job->time_expires =
        job->time_scheduled + (state->timeout > 0) ? (state->timeout) : (default_timeout);

    job->check_options = obj->check_options;
    job->check_type = state->check_type;
    job->latency = state->latency;

    dispatchJob(job, getExecutorName(obj->custom_variables));

    obj->latency = old_latency;
    obj->current_attempt = old_current_attempt;
}

void processServiceCheckInitiate(nebstruct_service_check_data* state) {
    const service* obj = (service*)state->object_ptr;
    check_result* cri = state->check_result_ptr;
    auto job = std::make_shared<Job>();
    job->host_name = std::string(state->host_name);
    job->service_description = std::string(state->service_description);
    job->command_line = std::string(state->command_line);
    job->time_scheduled = state->timestamp.tv_sec;
    job->time_expires =
        job->time_scheduled + (state->timeout > 0) ? (state->timeout) : (default_timeout);

    job->check_options = cri->check_options;
    job->check_type = state->check_type;
    job->latency = state->latency;

    dispatchJob(job, getExecutorName(obj->custom_variables));
}
}

int handleNebNagiosCheckInitiate(int which, void* obj) {
    nebstruct_process_data* raw = static_cast<nebstruct_process_data*>(obj);

    switch (which) {
        case NEBCALLBACK_HOST_CHECK_DATA:
            if (raw->type != NEBTYPE_HOSTCHECK_ASYNC_PRECHECK)
                return 0;
            try {
                processHostCheckInitiate(static_cast<nebstruct_host_check_data*>(obj));
            } catch (std::exception e) {
                logit(NSLOG_RUNTIME_ERROR, TRUE, "Error processing host check for %s", e.what());
            }
            return NEBERROR_CALLBACKOVERRIDE;
        case NEBCALLBACK_SERVICE_CHECK_DATA:
            if (raw->type != NEBTYPE_SERVICECHECK_INITIATE)
                return 0;
            try {
                processServiceCheckInitiate(static_cast<nebstruct_service_check_data*>(obj));
            } catch (std::exception e) {
                logit(NSLOG_RUNTIME_ERROR, TRUE, "Error processing host check for %s", e.what());
            }
            return NEBERROR_CALLBACKOVERRIDE;
        default:
            return 0;
    }
}

void processTimedOutChecks() {
    JobPtr expired;
    while ((expired = getJobQueue()->getNextExpiredJob(std::time(nullptr))) != nullptr) {
        processTimeout(expired);
    }
}
