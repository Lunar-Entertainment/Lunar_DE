#include "panel/panel.hpp"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/wait.h>

namespace {
lunar::Panel* g_panel = nullptr;

void handle_termination_signal(int /*sig*/) {
    if (g_panel) {
        g_panel->request_quit();
    }
}

void handle_sigchld(int /*sig*/) {
    const int saved_errno = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved_errno;
}
} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    struct sigaction chld_sa{};
    chld_sa.sa_handler = handle_sigchld;
    chld_sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&chld_sa.sa_mask);
    sigaction(SIGCHLD, &chld_sa, nullptr);

    struct sigaction term_sa{};
    term_sa.sa_handler = handle_termination_signal;
    sigemptyset(&term_sa.sa_mask);
    sigaction(SIGINT, &term_sa, nullptr);
    sigaction(SIGTERM, &term_sa, nullptr);

    lunar::Panel panel;
    g_panel = &panel;
    if (!panel.init()) {
        return 1;
    }
    const int exit_code = panel.run();
    g_panel = nullptr;
    return exit_code;
}
