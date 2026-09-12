#pragma once
#include <string>
#include <vector>
#include "store.hpp"

// Runs one parsed command and returns the RESP reply bytes.
// No sockets here: that keeps it unit-testable, and in Phase 5 the write-ahead
// log can be replayed through this exact path on recovery.
std::string execute(Store& store, const std::vector<std::string>& args);

// Keys this command modifies, or empty if it only reads. The append-only log
// uses this to decide what state to record after running a command.
std::vector<std::string> write_keys(const std::vector<std::string>& args);
