#include <thread>
#include <chrono>
#include <fstream>

#include "json.hpp"
#include "zmqpp/zmqpp.hpp"
#include "zmqpp/curve.hpp"

#include "cereal/cereal.hpp"
#include "cereal/types/chrono.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/archives/portable_binary.hpp"

#include <wordexp.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

#include "mqexec_shared.h"

// for convenience
using json = nlohmann::json;

struct WordExpWrapper {
    const char* progName() {
        return expvec.we_wordv[0];
    }

    char** args() {
        return expvec.we_wordv;
    }

    WordExpWrapper(const char* command_line) {
        std::stringstream ss;
        switch(wordexp(command_line, &expvec, WRDE_NOCMD)) {
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
    RunnerThread(zmqpp::socket _sock) :
        sock { std::move(_sock) }
    {};

    void operator()() {
        for (;;) {
            zmqpp::message msg;
            if (!sock.receive(msg))
                break;
            const int last_part = msg.parts() - 1;
            Job job;
            {
                std::istringstream msg_buf(msg.get(last_part));
                cereal::PortableBinaryInputArchive archive(msg_buf);
                archive(job);
            }

            Result res = runProgram(job);
            zmqpp::message response;
            for (auto i = 0; i < last_part; i++) {
                response.add(msg.get(i));
            }

            {
                std::ostringstream msg_buf;
                cereal::PortableBinaryOutputArchive archive(msg_buf);
                archive(res);
                response.add(msg_buf.str());
            }

            sock.send(response);
        }
    };

    Result runProgram(Job& job) {
        WordExpWrapper command_line_parsed(job.command_line.c_str());

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
            fprintf(stderr, "Error executing shell for %s: %s", job.command_line.c_str(), strerror(errno));
            exit(127);
        }
        else if(pid < 0) {
            std::stringstream ss;
            ss << "Error forking for " << job.command_line;
            throw std::system_error(errno, std::system_category(), ss.str());
        }

        close(pipes[1]);

        int status;
        std::stringstream output;
        ssize_t read_bytes = -1;
        while (waitpid(pid, &status, WNOHANG) == 0 && read_bytes != 0) {
            struct pollfd poller = { pipes[0], POLLIN | POLLERR, 0 };
            auto now = std::chrono::system_clock::now();
            auto timeout = job.time_expires - now;
            if (timeout.count() < 0) {
                kill(pid, SIGTERM);
                status = 3;
                output << "Task timed out\n";
                break;
            }

            int poll_res = poll(&poller, 1, timeout.count());
            if (poll_res == 0 || poller.revents & POLLERR)
                continue;

            std::array<char, 128> read_buf;
            read_bytes = read(pipes[0], read_buf.data(), read_buf.size());
            if (read_bytes > 0) {
                output.write(read_buf.data(), read_bytes);
            }
            if (read_bytes == -1) {
                throw std::system_error(errno, std::system_category());
            }
        }
        auto finish_time = std::chrono::system_clock::now();

        close(pipes[0]);
        status = WEXITSTATUS(status);

        return { job.id, output.str(), status, start_time, finish_time };
    }

private:
    zmqpp::socket sock;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Missing path to config file" << std::endl;
        return 1;
    }

    std::ifstream config_file_fp(argv[1]);
    if (!config_file_fp.is_open()) {
        std::cerr << "Could not open config file " << argv[1] << std::endl;
        return 1;
    }

    json config_obj = json::parse(config_file_fp);

    zmqpp::context zmq_ctx;
    zmqpp::socket master_socket(zmq_ctx, zmqpp::socket_type::dealer);
    int val = 1;
    try {
    master_socket.set(zmqpp::socket_option::probe_router, true);
    } catch (zmqpp::zmq_internal_exception e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    if (config_obj.find("curve_keys") != config_obj.end()) {
        auto curve_keys = config_obj["curve_keys"];
        master_socket.set(zmqpp::socket_option::curve_secret_key, curve_keys["secret"].get<std::string>());
        master_socket.set(zmqpp::socket_option::curve_public_key, curve_keys["public"].get<std::string>());
        master_socket.set(zmqpp::socket_option::curve_server_key, curve_keys["server"].get<std::string>());
    }

    if (config_obj.find("executor_name") != config_obj.end()) {
        master_socket.set(zmqpp::socket_option::identity, config_obj["executor_name"].get<std::string>());
    } else {
        std::string hostname;
        hostname.reserve(sysconf(_SC_HOST_NAME_MAX) + 1);
        if (gethostname(const_cast<char*>(hostname.data()), hostname.size()) == -1) {
            std::cerr << "Error getting hostname: " << strerror(errno) << std::endl;
            return 1;
        }
        master_socket.set(zmqpp::socket_option::identity, hostname);
    }

    if (config_obj.find("server_uri") == config_obj.end()) {
        std::cerr << "Config must specify server_uri parameter" << std::endl;
        return 1;
    }

    master_socket.connect(config_obj["server_uri"].get<std::string>());

    zmqpp::socket distributor_socket(zmq_ctx, zmqpp::socket_type::dealer);
    distributor_socket.bind("inproc://work_distributor");

    int workers = 5;
    if (config_obj.find("workers") != config_obj.end()) {
        workers = config_obj["workers"].get<int>();
    }

    for (auto i = 0; i < workers; i++) {
        zmqpp::socket worker_socket(zmq_ctx, zmqpp::socket_type::dealer);
        worker_socket.connect("inproc://work_distributor");

        RunnerThread runner_obj(std::move(worker_socket));
        std::thread worker(std::move(runner_obj));
        worker.detach();
    }

    for (;;) {
        zmqpp::poller proxy_poller;
        proxy_poller.add(master_socket);
        proxy_poller.add(distributor_socket);
        proxy_poller.poll();

        if (proxy_poller.has_input(master_socket)) {
            zmqpp::message msg;
            master_socket.receive(msg);
            distributor_socket.send(msg);
        }

        if (proxy_poller.has_input(distributor_socket)) {
            zmqpp::message msg;
            distributor_socket.receive(msg);
            master_socket.send(msg);
        }
    }
    return 0;
}
