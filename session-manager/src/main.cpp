#include "../../service-common/include/service_daemon.hpp"

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    return service_daemon::run("session.manager");
}
