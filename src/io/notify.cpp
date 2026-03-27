#include "notify.h"
#include <cstdlib>
#include <cstdio>
#include <string>

namespace nba {
namespace notify {

void send(const std::string& message, const std::string& script_path) {
    if (message.empty()) return;

    // Escape single quotes in the message for shell safety
    std::string escaped;
    escaped.reserve(message.size() + 16);
    for (char c : message) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }

    std::string cmd = script_path + " '" + escaped + "'";

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        fprintf(stderr, "notify::send: command returned %d: %s\n",
                ret, cmd.c_str());
    }
}

} // namespace notify
} // namespace nba
