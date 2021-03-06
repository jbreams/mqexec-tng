#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "json.hpp"
#include "zmqpp/curve.hpp"
#include "zmqpp/zmqpp.hpp"

#include "cereal/archives/portable_binary.hpp"
#include "cereal/cereal.hpp"
#include "cereal/types/chrono.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/types/string.hpp"

#define LOGURU_WITH_STREAMS 1
#include "loguru.hpp"

#include "mqexec_shared.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <wordexp.h>

// for convenience
using json = nlohmann::json;

void syslog_log_callback(void* user_data, const loguru::Message& msg) {
    int msg_prio;
    switch (msg.verbosity) {
        case loguru::Verbosity_FATAL:
            msg_prio = LOG_CRIT;
            break;
        case loguru::Verbosity_ERROR:
            msg_prio = LOG_ERR;
            break;
        case loguru::Verbosity_WARNING:
            msg_prio = LOG_WARNING;
            break;
        case loguru::Verbosity_INFO:
            msg_prio = LOG_INFO;
            break;
        deafult:
            msg_prio = LOG_DEBUG;
            break;
    }

    if (std::strlen(msg.prefix) == 0)
        syslog(msg_prio, msg.message);
    else
        syslog(msg_prio, "%s: %s", msg.prefix, msg.message);
}
void syslog_empty_callback(void* user_data) {}

struct WordExpWrapper {
    const char* progName() {
        return expvec.we_wordv[0];
    }

    char** args() {
        return expvec.we_wordv;
    }

    WordExpWrapper(const char* command_line) {
        std::stringstream ss;
        switch (wordexp(command_line, &expvec, WRDE_NOCMD)) {
            case 0:
                return;
            case WRDE_SYNTAX:
                ss << "Error executing \"" << command_line << "\". Bad syntax.";
                break;
            case WRDE_CMDSUB:
                ss << "Command \"" << command_line << "\" uses unsafe command substitution.";
                break;
            case WRDE_BADVAL:
            case WRDE_BADCHAR:
                ss << "Command \"" << command_line << "\" uses invalid characters or variables.";
                break;
            case WRDE_NOSPACE:
                ss << "Out of memory while parsing command line";
                break;
        }
        throw std::runtime_error(ss.str());
    }

    ~WordExpWrapper() {
        wordfree(&expvec);
    }

private:
    wordexp_t expvec;
};

class RunnerThread {
public:
    RunnerThread(zmqpp::socket _sock, int _worker) : sock{std::move(_sock)}, worker{_worker} {};

    void operator()() {
        {
            std::stringstream ss;
            ss << "worker #" << worker;
            loguru::set_thread_name(ss.str().c_str());
        }
        sigset_t set;
        sigfillset(&set);
        sigprocmask(SIG_SETMASK, &set, nullptr);

        for (;;) {
            zmqpp::message msg;
            try {
                sock.receive(msg);
            } catch (zmqpp::zmq_internal_exception& e) {
                if (e.zmq_error() == ETERM) {
                    break;
                }
                LOG_S(ERROR) << "Error receiving job from nagios: " << e.what();
                continue;
            }

            LOG_S(1) << "Received job from nagios";
            const int last_part = msg.parts() - 1;
            Job job;
            try {
                const std::string msg_buf = msg.get(last_part);
                std::istringstream msg_stream(msg_buf);
                cereal::PortableBinaryInputArchive archive(msg_stream);
                archive(job);
            } catch (std::exception& e) {
                LOG_S(ERROR) << "Error decoding job from nagios: " << e.what();
                continue;
            }

            Result res;
            try {
                res = runProgram(job);
            } catch (std::exception& e) {
                LOG_S(ERROR) << "Error running job: " << e.what();
                continue;
            }
            zmqpp::message response;
            for (auto i = 0; i < last_part; i++) {
                response.add(msg.get(i));
            }

            try {
                std::ostringstream msg_buf;
                cereal::PortableBinaryOutputArchive archive(msg_buf);
                archive(res);
                response.add(msg_buf.str());
            } catch (std::exception& e) {
                LOG_S(ERROR) << "Error encoding result to send to nagios: " << e.what();
                continue;
            }

            try {
                sock.send(response);
            } catch (zmqpp::zmq_internal_exception& e) {
                if (e.zmq_error() == ETERM)
                    break;
                LOG_S(ERROR) << "Error sending result to nagios: " << e.what();
                continue;
            }
            LOG_S(1) << "Sent result back to nagios";
        }
    };

    Result runProgram(Job& job) {
        LOG_SCOPE_FUNCTION(1);
        WordExpWrapper command_line_parsed(job.command_line.c_str());
        LOG_S(1) << "Parsed command line for " << command_line_parsed.progName();

        int pipes[2];
        if (pipe(pipes) == -1) {
            throw std::system_error(errno, std::system_category());
        }

        auto start_time = std::chrono::system_clock::now();
        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull < 0) {
                perror("Error redirecting stdin: ");
                exit(127);
            }
            dup2(devnull, fileno(stdin));
            close(devnull);
            dup2(pipes[1], fileno(stdout));
            dup2(pipes[1], fileno(stderr));
            close(pipes[1]);

            execv(command_line_parsed.progName(), command_line_parsed.args());
            fprintf(stderr,
                    "Error executing shell for %s: %s",
                    job.command_line.c_str(),
                    strerror(errno));
            exit(127);
        } else if (pid < 0) {
            std::stringstream ss;
            ss << "Error forking for " << job.command_line;
            throw std::system_error(errno, std::system_category(), ss.str());
        }

        close(pipes[1]);

        int status;
        std::stringstream output;
        ssize_t read_bytes = -1;
        while (waitpid(pid, &status, WNOHANG) == 0 && read_bytes != 0) {
            struct pollfd poller = {pipes[0], POLLIN | POLLERR, 0};
            auto now = std::chrono::system_clock::now();
            auto timeout = job.time_expires - now;
            if (timeout.count() < 0) {
                kill(pid, SIGTERM);
                status = 3;
                output << "Task timed out\n";
                LOG_S(WARNING) << "Job timed out running " << command_line_parsed.progName();
                break;
            }

            int poll_res = poll(&poller, 1, timeout.count());
            if (poll_res == 0 || poller.revents & POLLERR) {
                LOG_S(1) << "Poll returned, but there were no useful events!";
                continue;
            }

            std::array<char, 128> read_buf;
            read_bytes = read(pipes[0], read_buf.data(), read_buf.size());
            if (read_bytes > 0) {
                LOG_S(1) << "Captured " << read_bytes << " of output from "
                         << command_line_parsed.progName();
                output.write(read_buf.data(), read_bytes);
            }
            if (read_bytes == -1) {
                throw std::system_error(errno, std::system_category());
            }
        }
        auto finish_time = std::chrono::system_clock::now();

        close(pipes[0]);
        status = WEXITSTATUS(status);
        LOG_S(1) << command_line_parsed.progName() << " finished with exit code " << status;

        return {job.id, output.str(), status, start_time, finish_time};
    }

private:
    zmqpp::socket sock;
    int worker;
};

int main(int argc, char** argv) {
    loguru::init(argc, argv);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    if (argc < 2) {
        LOG_S(ERROR) << "Missing path to config file";
        return 1;
    }

    std::ifstream config_file_fp(argv[1]);
    if (!config_file_fp.is_open()) {
        LOG_S(ERROR) << "Could not open config file " << argv[1];
        return 1;
    }

    json config_obj;
    try {
        config_obj = json::parse(config_file_fp);
    } catch (std::exception& e) {
        LOG_S(ERROR) << "Error parsing config file " << e.what();
        return 1;
    }

    if (config_obj.find("use_syslog") != config_obj.end() && config_obj["use_syslog"].get<bool>()) {
        loguru::add_callback("syslog",
                             syslog_log_callback,
                             nullptr,
                             loguru::Verbosity_INFO,
                             syslog_empty_callback,
                             syslog_empty_callback);
    }

    if (config_obj.find("verbose") != config_obj.end() && config_obj["verbose"].get<bool>()) {
        loguru::g_stderr_verbosity = loguru::Verbosity_1;
    }

    zmqpp::context zmq_ctx;
    zmqpp::socket master_socket(zmq_ctx, zmqpp::socket_type::dealer);
    master_socket.set(zmqpp::socket_option::probe_router, true);

    if (config_obj.find("curve_keys") != config_obj.end()) {
        const auto curve_keys = config_obj["curve_keys"];
        const auto public_key = curve_keys["public"].get<std::string>();
        const auto server_key = curve_keys["server"].get<std::string>();
        master_socket.set(zmqpp::socket_option::curve_secret_key,
                          curve_keys["secret"].get<std::string>());
        master_socket.set(zmqpp::socket_option::curve_public_key, public_key);
        master_socket.set(zmqpp::socket_option::curve_server_key, server_key);
        LOG_S(INFO) << "Setting up curve keys";
        LOG_S(1) << "Public key: " << public_key;
        LOG_S(1) << "Server key: " << server_key;
    }

    if (config_obj.find("executor_name") != config_obj.end()) {
        auto executor_name = config_obj["executor_name"].get<std::string>();
        master_socket.set(zmqpp::socket_option::identity, executor_name);
        LOG_S(INFO) << "Setting executor name from config: " << executor_name;
    } else {
        std::string hostname;
        hostname.reserve(sysconf(_SC_HOST_NAME_MAX) + 1);
        if (gethostname(const_cast<char*>(hostname.data()), hostname.size()) == -1) {
            LOG_S(FATAL) << "Error getting hostname: " << strerror(errno);
            return 1;
        }
        master_socket.set(zmqpp::socket_option::identity, hostname);
        LOG_S(INFO) << "Setting executor name from hostname: " << hostname;
    }

    if (config_obj.find("server_uri") == config_obj.end()) {
        LOG_S(FATAL) << "Config must specify server_uri parameter";
        return 1;
    }

    auto server_uri = config_obj["server_uri"].get<std::string>();
    master_socket.connect(server_uri);
    LOG_S(INFO) << "Connecting to " << server_uri;

    zmqpp::socket distributor_socket(zmq_ctx, zmqpp::socket_type::dealer);
    distributor_socket.bind("inproc://work_distributor");

    int workers = 5;
    if (config_obj.find("workers") != config_obj.end()) {
        workers = config_obj["workers"].get<int>();
    }

    LOG_S(INFO) << "Starting " << workers << " worker threads";
    for (auto i = 0; i < workers; i++) {
        zmqpp::socket worker_socket(zmq_ctx, zmqpp::socket_type::dealer);
        worker_socket.connect("inproc://work_distributor");

        std::thread worker(RunnerThread(std::move(worker_socket), i));
        worker.detach();
    }

    for (;;) {
        zmqpp::poller proxy_poller;
        proxy_poller.add(master_socket);
        proxy_poller.add(distributor_socket);
        proxy_poller.poll();

        if (proxy_poller.has_input(master_socket)) {
            zmqpp::message msg;
            try {
                master_socket.receive(msg);
                distributor_socket.send(msg);
            } catch (zmqpp::zmq_internal_exception& e) {
                if (e.zmq_error() == ETERM)
                    break;
                LOG_S(ERROR) << "Error receiving job nagios (main thread) " << e.what();
                continue;
            }
            LOG_S(1) << "Received job from nagios (main thread)";
        }

        if (proxy_poller.has_input(distributor_socket)) {
            zmqpp::message msg;
            try {
                distributor_socket.receive(msg);
                master_socket.send(msg);
            } catch (zmqpp::zmq_internal_exception& e) {
                if (e.zmq_error() == ETERM)
                    break;
                LOG_S(ERROR) << "Error sending result to nagios (main thread) " << e.what();
                continue;
            }
            LOG_S(1) << "Sent result back to nagios (main thread)";
        }
    }
    return 0;
}
