#pragma once

#include <string>

namespace nba {
namespace notify {

// Send a notification via notify.sh (or a custom script path).
// Calls the script with the message as argument using system().
void send(const std::string& message, const std::string& script_path = "./notify.sh");

} // namespace notify
} // namespace nba
