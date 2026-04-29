#include <cstdlib.hpp>
#include <cstring.hpp>
#include <new.hpp>

int main() {
    const char* msg = "Hello from Userland!\n";
    std::write(1, msg, std::strlen(msg));

    auto* buffer = new char[32];
    std::memset(buffer, 'A', 31);
    buffer[31] = '\n';
    std::write(1, buffer, 32);
    delete[] buffer;

    return 0;
}
