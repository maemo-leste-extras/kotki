#pragma once
#include <string>

#include "kotki/kotki.h"

extern Kotki *KOTKI;

namespace translate_server {
    void start(const std::string& socketPath);
    void stop();

    void broadcastData();
    void broadcastError(const std::string &command, const std::string &errorMsg);

    void broadcast(const std::string& message);
    std::string _makeErrorResponse(const std::string &command, const std::string &errorMsg);
    std::string _handleCommand(const std::string &cmdLine);
    void _serverLoop(const std::string &socketPath);
}
