#include "json.hpp"
#include "zmqpp/curve.hpp"
#include "zmqpp/zmqpp.hpp"

#include "cereal/archives/portable_binary.hpp"
#include "cereal/cereal.hpp"
#include "cereal/types/chrono.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/types/string.hpp"

#include "mqexec_shared.h"

int main(int argc, char** argv) {
    zmqpp::context zmq_ctx;
    zmqpp::socket sock(zmq_ctx, zmqpp::socket_type::router);
    sock.set(zmqpp::socket_option::router_mandatory, true);
    sock.bind(argv[1]);

    int jobs_to_run = 10;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (auto i = 0; i < jobs_to_run; i++) {
        std::stringstream ss;
        ss << "/bin/echo Hello, world - from " << i;

        auto scheduled = std::chrono::system_clock::now();
        auto expires = scheduled + std::chrono::seconds(10);
        Job j = {static_cast<uint64_t>(i), "foo", "bar", ss.str(), scheduled, expires, 0, 0, 0.0};

        zmqpp::message msg;
        msg.add(argv[2]);

        std::ostringstream req_buffer;
        cereal::PortableBinaryOutputArchive archive(req_buffer);
        archive(j);

        msg.add(req_buffer.str());
        try {
            sock.send(msg);
        } catch (zmqpp::zmq_internal_exception e) {
            std::cout << "No connection for: " << i << " " << e.what() << std::endl;
        } catch (std::exception e) {
            std::cout << "Error " << e.what() << std::endl;
        }

        std::cout << "Sent job " << i << std::endl;
        sock.receive(msg);

        if (msg.parts() == 0) {
            continue;
        }

        const int last_part = msg.parts() - 1;
        std::cout << msg.get(0) << std::endl;
        if (msg.get(last_part).size() == 0)
            continue;

        std::istringstream response_buf(msg.get(last_part));
        Result res;
        try {
            cereal::PortableBinaryInputArchive archive(response_buf);
            archive(res);
        } catch (std::exception e) {
            std::cerr << "Error receiving response: " << e.what();
            continue;
        }

        std::cout << "Received job result: " << res.output << std::endl;
        std::cout << "Exit code: " << res.return_code << std::endl;
    }
}
