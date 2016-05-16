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

#include "module.h"

std::shared_ptr<JobQueue> job_queue;
std::shared_ptr<JobQueue> getJobQueue() {
    if (job_queue == nullptr)
        job_queue = std::make_shared<JobQueue>();

    return job_queue;
}

uint64_t JobQueue::next_id = 0;
JobMap JobQueue::_placeholder = JobMap{};

int64_t default_timeout = 120;

const char* engine_name = "MQexec NG";
const char* mqexec_source_name(const void* unused) {
    return engine_name;
}

struct check_engine mqexec_check_engine = {
    const_cast<char*>(engine_name), mqexec_source_name, std::free};

void processJobError(JobPtr job, std::string errmsg) {
    log_debug_info(DEBUGL_EVENTS, DEBUGV_BASIC,
        "Sending error check result for mqexec job (id: %lu host: %s service: %s)\n",
        job->id, job->host_name.c_str(), job->service_description.c_str());
    const Result res = {job->id,
                        errmsg,
                        job->service_description.empty() ? 3 : 1,
                        job->time_scheduled,
                        std::chrono::system_clock::now()};
    processResult(job, res);
}

void processTimeout(JobPtr job) {
    log_debug_info(DEBUGL_EVENTS, DEBUGV_BASIC,
        "Sending timeout check result for mqexec job (id: %lu host: %s service: %s)\n",
        job->id, job->host_name.c_str(), job->service_description.c_str());
    const Result res = {job->id,
                        "Check timed out",
                        job->service_description.empty() ? 3 : 1,
                        job->time_scheduled,
                        std::chrono::system_clock::now()};
    processResult(job, res);
}

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

    log_debug_info(DEBUGL_EVENTS, DEBUGV_BASIC,
        "Processing mqexec check result (id: %lu host: %s service: %s)\n",
        job->id, job->host_name.c_str(), job->service_description.c_str());

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
    new_result.start_time = result.getStartTimeVal();
    new_result.finish_time = result.getFinishTimeVal();

    new_result.exited_ok = 1;
    new_result.early_timeout = 0;

    new_result.check_type = job->check_type;
    new_result.check_options = job->check_options;
    new_result.scheduled_check = job->scheduled_check;
    new_result.reschedule_check = job->reschedule_check;
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

bool shouldOverrideCheck(customvariablesmember* vars) {
    while (vars != nullptr) {
        if (std::strcmp(vars->variable_name, "_MQEXEC_IGNORE") == 0)
            return (std::strcmp(vars->variable_value, "true") != 0);
        vars = vars->next;
    }
    return true;
}

void timeout_callback(void* ptr) {
    auto job_id = reinterpret_cast<uint64_t>(ptr);
    try {
        log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "MQexec timing out job  %lu\n", job_id);
        auto job = getJobQueue()->getCheck(job_id);
        logit(NSLOG_INFO_MESSAGE, FALSE, "MQexec timing out check (hostname: %s service: %s)\n",
                job->host_name.c_str(), job->service_description.c_str());
        processTimeout(job);
    } catch (std::out_of_range& e) {
        log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE,
            "MQexec timeout callback called for already completed check\n");
    }
}

void scheduleTimeout(JobPtr jobptr) {
    log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "Scheduling timeout in mqexec\n");
    auto expiry_time = std::chrono::system_clock::to_time_t(jobptr->time_expires);
    schedule_new_event(EVENT_USER_FUNCTION,
            TRUE, expiry_time, FALSE, 0, NULL, TRUE, reinterpret_cast<void*>(timeout_callback),
            reinterpret_cast<void*>(jobptr->id), 0);
}

void processHostCheckInitiate(nebstruct_host_check_data* state) {
    host* obj = (host*)state->object_ptr;

    char* processed_command = nullptr;
    double old_latency = obj->latency;
    int old_current_attempt = obj->current_attempt;
    obj->latency = state->latency;

    log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "Entering mqexec service check dispatcher\n");
    if (fixup_async_presync_hostcheck(obj, &processed_command) != 0)
        return;
    std::unique_ptr<char> processed_command_guard(processed_command);

    auto job = std::make_shared<Job>();
    job->host_name = std::string(state->host_name);
    job->command_line = std::string(processed_command);
    job->time_scheduled = std::chrono::system_clock::from_time_t(state->timestamp.tv_sec);
    job->time_expires =
        job->time_scheduled + std::chrono::seconds(
            (state->timeout > 0) ? (state->timeout) : (default_timeout));

    job->check_options = obj->check_options;
    job->check_type = state->check_type;
    job->scheduled_check = 1;
    job->reschedule_check = 1;
    job->latency = state->latency;

    dispatchJob(job, getExecutorName(obj->custom_variables));

    obj->latency = old_latency;
    obj->current_attempt = old_current_attempt;
}

void processServiceCheckInitiate(nebstruct_service_check_data* state) {
    const service* obj = (service*)state->object_ptr;
    check_result* cri = state->check_result_ptr;
    auto job = std::make_shared<Job>();

    log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "Entering mqexec service check dispatcher\n");
    job->host_name = std::string(state->host_name);
    job->service_description = std::string(state->service_description);
    job->command_line = std::string(state->command_line);
    job->time_scheduled = std::chrono::system_clock::from_time_t(state->timestamp.tv_sec);
    job->time_expires =
        job->time_scheduled + std::chrono::seconds(
            (state->timeout > 0) ? (state->timeout) : (default_timeout));

    job->check_options = cri->check_options;
    job->check_type = state->check_type;
    job->scheduled_check = cri->scheduled_check;
    job->reschedule_check = cri->reschedule_check;
    job->latency = state->latency;

    dispatchJob(job, getExecutorName(obj->custom_variables));
}

int handleNebNagiosCheckInitiate(int which, void* obj) {
    nebstruct_process_data* raw = static_cast<nebstruct_process_data*>(obj);

    switch (which) {
        case NEBCALLBACK_HOST_CHECK_DATA:
            if (raw->type != NEBTYPE_HOSTCHECK_ASYNC_PRECHECK)
                return 0;
            try {
                auto state = static_cast<nebstruct_host_check_data*>(obj);
                auto vars = (static_cast<service*>(state->object_ptr))->custom_variables;
                if (!shouldOverrideCheck(vars)) {

                    return 0;
                }

                processHostCheckInitiate(static_cast<nebstruct_host_check_data*>(obj));
            } catch (std::exception e) {
                logit(NSLOG_RUNTIME_ERROR, TRUE, "Error processing host check for %s", e.what());
            }
            log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "MQexec handled host check\n");
            return NEBERROR_CALLBACKOVERRIDE;
        case NEBCALLBACK_SERVICE_CHECK_DATA:
            if (raw->type != NEBTYPE_SERVICECHECK_INITIATE)
                return 0;
            try {
                auto state = static_cast<nebstruct_service_check_data*>(obj);
                auto vars = (static_cast<service*>(state->object_ptr))->custom_variables;
                if (!shouldOverrideCheck(vars))
                    return 0;

                processServiceCheckInitiate(state);
            } catch (std::exception e) {
                logit(NSLOG_RUNTIME_ERROR, TRUE, "Error processing host check for %s", e.what());
            }
            log_debug_info(DEBUGL_CHECKS, DEBUGV_MORE, "MQexec handled service check\n");
            return NEBERROR_CALLBACKOVERRIDE;
        default:
            return 0;
    }
}

