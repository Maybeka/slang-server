// A deliberately different executable for native macOS identity regression tests.
// It records whether any request byte crossed the signature-validation boundary.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static bool append(const char* path, const char* text) {
    int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return false;
    size_t size = std::strlen(text);
    while (size) {
        ssize_t count = ::write(fd, text, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(fd);
            return false;
        }
        text += count;
        size -= static_cast<size_t>(count);
    }
    return ::close(fd) == 0;
}
int main(int argc, char** argv) {
    ::alarm(10);
    const char* marker = std::getenv("DIAGNOSTIC_TRANSPORT_IDENTITY_MARKER");
    if (argc != 2 || std::strcmp(argv[1], "--diagnostic-worker") != 0 || !marker)
        return 81;
    if (!append(marker, "BOOT\n"))
        return 82;
    char byte = 0;
    ssize_t count;
    do { count = ::read(STDIN_FILENO, &byte, 1); } while (count < 0 && errno == EINTR);
    if (count == 1) {
        if (!append(marker, "INPUT\n"))
            return 83;
    }
    return 0;
}
