#include "wm/window_manager.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
lunar::WindowManager* g_wm = nullptr;

void handle_termination_signal(int /*sig*/) {
    if (g_wm) {
        g_wm->request_quit();
    }
}

void handle_restart_signal(int /*sig*/) {
    if (g_wm) {
        g_wm->request_restart();
    }
}

void handle_sigchld(int /*sig*/) {
    const int saved_errno = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved_errno;
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::signal(SIGPIPE, SIG_IGN);

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

    struct sigaction usr1_sa{};
    usr1_sa.sa_handler = handle_restart_signal;
    usr1_sa.sa_flags = 0; // Interrupt poll() immediately with EINTR
    sigemptyset(&usr1_sa.sa_mask);
    sigaction(SIGUSR1, &usr1_sa, nullptr);

    bool do_restart = false;
    int exit_code = 0;
    {
        lunar::WindowManager wm;
        g_wm = &wm;
        if (!wm.init()) {
            g_wm = nullptr;
            return 1;
        }
        exit_code = wm.run();
        do_restart = wm.restart_requested();
        g_wm = nullptr;
    }

    if (do_restart) {
        const char* argv0_fb = (argc > 0 && argv && argv[0]) ? argv[0] : "/usr/bin/lunar-wm";
        const std::string exe_path = lunar::resolve_self_exe_path(argv0_fb);
        std::cout << "lunar-wm: restarting in-place via " << exe_path << '\n';
        execv(exe_path.c_str(), argv);
        std::perror("lunar-wm: execv failed");
        return 1;
    }

    return exit_code;
}
