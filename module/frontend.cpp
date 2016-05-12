#include <algorithm>
#include <fstream>
#include <set>
#include <vector>

#include "nebstructs.h"
#include "nebcallbacks.h"
#include "nebmodules.h"
#include "nebmods.h"
#include "broker.h"
#ifdef HAVE_ICINGA
#include "icinga.h"
#else
#include "nagios.h"
#endif

#include "cereal/cereal.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/types/chrono.hpp"
#include "cereal/archives/portable_binary.hpp"

#include "zmqpp/zmqpp.hpp"
#include "zmqpp/curve.hpp"

#include "json.hpp"
// for convenience
 using json = nlohmann::json;

#include "module.h"

extern iobroker_set* nagios_iobs;

namespace {
std::unique_ptr<zmqpp::context> zmq_ctx = nullptr;
std::unique_ptr<zmqpp::socket> server_socket = nullptr;
std::unique_ptr<zmqpp::auth> zmq_authenticator = nullptr;
json config_file_obj;
}

void dispatchJob(JobPtr job, std::string executor) {
    if (executor.empty())
        executor = job->host_name;

    const auto job_id = getJobQueue()->addCheck(job);
    try {
        zmqpp::message req_msg;
        req_msg.add(executor);
        std::ostringstream req_buffer;

        cereal::PortableBinaryOutputArchive archive(req_buffer);
        archive(job);
        req_msg.add(req_buffer.str());
        server_socket->send(req_msg);
    } catch (std::exception e) {
        std::stringstream ss;
        ss << "Error sending job to executor: " << e.what();
        processJobError(job, ss.str());
        getJobQueue()->getCheck(job_id);
    }
}

int processIOEvent(int sd, int events, void* arg) {
    for (;;) {
        zmqpp::message raw_msg;
        try {
            if (!server_socket->receive(raw_msg, false))
                break;

            if (raw_msg.parts() == 0)
                continue;

            std::istringstream response_buf(raw_msg.get(1));
            cereal::PortableBinaryInputArchive archive(response_buf);

            Result res;
            archive(res);
            const auto job = getJobQueue()->getCheck(res.id);
            processResult(job, res);
        } catch (std::exception e) {
            logit(NSLOG_RUNTIME_ERROR, TRUE, "Error receiving result from executor: %s", e.what());
        }
    }
    return 0;
}

void initServer(std::string bind_point, std::string key_file_path, zmqpp::curve::keypair keys) {
    if (zmq_ctx || server_socket || zmq_authenticator) {
        throw std::runtime_error("ZeroMQ shouldn't be initialized yet!");
    }
    zmq_ctx = std::unique_ptr<zmqpp::context>(new zmqpp::context{});
    server_socket =
        std::unique_ptr<zmqpp::socket>(new zmqpp::socket(*zmq_ctx, zmqpp::socket_type::router));

    if (!keys.secret_key.empty()) {
        server_socket->set(zmqpp::socket_option::curve_secret_key, keys.secret_key);
        server_socket->set(zmqpp::socket_option::curve_public_key, keys.public_key);
        const int as_server = 1;
        server_socket->set(zmqpp::socket_option::curve_server, as_server);
    }

    if (!key_file_path.empty()) {
        zmq_authenticator = std::unique_ptr<zmqpp::auth>(new zmqpp::auth(*zmq_ctx));

        std::ifstream key_file_fp;
        key_file_fp.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        key_file_fp.open(key_file_path);

        for (std::string line; std::getline(key_file_fp, line);) {
            std::string line_trimmed;

            // Trim out all whitespace characters into a "trimmed" copy of the line
            std::remove_copy_if(line.begin(),
                                line.end(),
                                std::back_inserter(line_trimmed),
                                [](char c) -> bool { return std::isspace(c); });

            // Skip lines that are less than 40 characters or that start with '#', which indicates
            // a comment.
            if (line_trimmed.size() < 40 || line_trimmed[0] == '#')
                continue;

            // We take the first 40 characters as z85 encoded keys.
            zmq_authenticator->configure_curve(line_trimmed.substr(0, 40));
        }
    }

    server_socket->set(zmqpp::socket_option::router_mandatory, 1);
    server_socket->set(zmqpp::socket_option::probe_router, 1);

    server_socket->bind(bind_point);

    int fd;
    server_socket->get(zmqpp::socket_option::file_descriptor, fd);
    iobroker_register(nagios_iobs, fd, nullptr, processIOEvent);
}

void shutdownServer() {
    server_socket->close();
    server_socket = nullptr;
    zmq_ctx->terminate();
    zmq_ctx = nullptr;
    zmq_authenticator = nullptr;
}

// NEB_API_VERSION(CURRENT_NEB_API_VERSION)
nebmodule* handle;

int handleStartup(int which, void* obj) {
    struct nebstruct_process_struct* ps = static_cast<struct nebstruct_process_struct*>(obj);

    switch (ps->type) {
        case NEBTYPE_PROCESS_EVENTLOOPSTART: {
            std::string allowed_key_file;
            if (config_file_obj.find("allowed_keys_file") != config_file_obj.end()) {
                allowed_key_file = config_file_obj.at("allowed_keys_file").get<std::string>();
            }

            zmqpp::curve::keypair keys;
            if (config_file_obj.find("curve_keys") != config_file_obj.end()) {
                auto curve_keys = config_file_obj["curve_keys"];
                keys.public_key = curve_keys.at("public").get<std::string>();
                keys.secret_key = curve_keys.at("secret").get<std::string>();
            }

            initServer(
                config_file_obj.at("bind_address").get<std::string>(), allowed_key_file, keys);
        } break;
        case NEBTYPE_PROCESS_EVENTLOOPEND: {
            shutdownServer();
        } break;
    }
    return 0;
}

int loadConfigFile(char* localargs) {
    try {
        std::ifstream config_file_obj_fp;
        config_file_obj_fp.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        config_file_obj_fp.open(localargs);

        config_file_obj = json::parse(config_file_obj_fp);
    } catch (std::exception e) {
        logit(NSLOG_RUNTIME_ERROR, TRUE, "Error loading config file: %s", e.what());
        return 1;
    }
    return 0;
}

extern "C" {

// This is defined in nagios's objects.c, it should match the current
// ABI version of the nagios nagmq is loaded into.
extern int __nagios_object_structure_version;

int nebmodule_deinit(int flags, int reason) {
    neb_deregister_module_callbacks(handle);
#ifdef HAVE_SHUTDOWN_COMMAND_FILE_WORKER
    shutdown_command_file_worker();
#endif
    return 0;
}

int nebmodule_init(int flags, char* localargs, nebmodule* lhandle) {
    handle = lhandle;

    if (__nagios_object_structure_version != CURRENT_OBJECT_STRUCTURE_VERSION) {
        logit(NSLOG_RUNTIME_ERROR,
              TRUE,
              "NagMQ is loaded into a version of nagios with a different ABI "
              "than it was compiled for! You need to recompile NagMQ against the current "
              "nagios headers!");
        return -1;
    }

    neb_set_module_info(handle, NEBMODULE_MODINFO_TITLE, const_cast<char*>("MQexec TNG"));
    neb_set_module_info(handle, NEBMODULE_MODINFO_AUTHOR, const_cast<char*>("Jonathan Reams"));
    neb_set_module_info(handle, NEBMODULE_MODINFO_VERSION, const_cast<char*>("v1"));
    neb_set_module_info(handle, NEBMODULE_MODINFO_LICENSE, const_cast<char*>("Apache v2"));
    neb_set_module_info(handle,
                        NEBMODULE_MODINFO_DESC,
                        const_cast<char*>("Distributed check execution with ZeroMQ"));

    neb_register_callback(NEBCALLBACK_PROCESS_DATA, lhandle, 0, handleStartup);
    neb_register_callback(NEBCALLBACK_HOST_CHECK_DATA, lhandle, 0, handleNebNagiosCheckInitiate);
    neb_register_callback(NEBCALLBACK_SERVICE_CHECK_DATA, lhandle, 0, handleNebNagiosCheckInitiate);

    return loadConfigFile(localargs);
}

}
