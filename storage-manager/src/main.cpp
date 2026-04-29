#include "../../service-common/include/service_daemon.hpp"

int main() {
    return service_daemon::run("storage.manager");
}
