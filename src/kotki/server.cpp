#include <sys/socket.h>
#include <cerrno>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include <mutex>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include "server.h"

Kotki *KOTKI = nullptr;

namespace translate_server {

static std::thread serverThread;
static std::atomic<bool> running{false};
static int server_fd = -1;

static std::atomic<int> activeClients{0};
static constexpr int maxClients = 5;

static std::set<int> clientSockets;
static std::mutex mtx_client;
static std::mutex mtx_command;

std::string handleListModels() {
  rapidjson::Document doc;
  doc.SetArray();
  auto &allocator = doc.GetAllocator();

  for (const auto &e : KOTKI->listModels()) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("model", rapidjson::Value(e.first.c_str(), allocator), allocator);
    for (const auto &innerPair : e.second) {
      obj.AddMember(
        rapidjson::Value(innerPair.first.c_str(), allocator),
        rapidjson::Value(innerPair.second.c_str(), allocator),
        allocator
      );
    }
    doc.PushBack(obj, allocator);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
}

std::string handleTranslate(const std::string &text, const std::string &model) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &allocator = doc.GetAllocator();
  doc.AddMember("command", "translate", allocator);
  doc.AddMember("original", rapidjson::Value().SetString(text.c_str(), allocator), allocator);

  const std::string translated = KOTKI->translate(text, model);
  doc.AddMember("translated", rapidjson::Value().SetString(translated.c_str(), allocator), allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
}

  std::string processCommand(const std::string &cmdLine) {
  rapidjson::Document doc;
  {
    std::lock_guard<std::mutex> lock(mtx_command);
    if (doc.Parse(cmdLine.c_str()).HasParseError() || !doc.HasMember("command") || !doc["command"].IsString())
      return R"({"status":"error","error":"invalid json or missing command"})";

    std::string cmd = doc["command"].GetString();

    if (cmd == "translate") {
      if (!doc.HasMember("text") || !doc["text"].IsString())
        return R"({"status":"error","error":"missing text for translation"})";
      const std::string text = doc["text"].GetString();
      const std::string lang = doc["lang"].GetString();
      return handleTranslate(text, lang);
    }

    if (cmd == "listModels")
      return handleListModels();

    return R"({"status":"error","error":"unknown command"})";
  }
}

static void handleClient(int client_fd) {
  {
    std::lock_guard lock(mtx_client);
    clientSockets.insert(client_fd);
  }

  char buffer[1024];
  std::string leftover;

  while (true) {
    memset(buffer, 0, sizeof(buffer));
    int n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) break;

    std::string data = leftover + std::string(buffer, n);
    leftover.clear();
    size_t start = 0, pos;

    while ((pos = data.find('\n', start)) != std::string::npos) {
      std::string cmd = data.substr(start, pos - start);
      start = pos + 1;
      std::string respStr = processCommand(cmd);
      if (!respStr.empty()) {
        ssize_t written = write(client_fd, respStr.c_str(), respStr.size());
        if (written < 0) {
          if (errno == EPIPE || errno == ECONNRESET) {
            std::cout << "Client disconnected (broken pipe)\n";
            goto cleanup;
          }
          perror("write");
          goto cleanup;
        }
      }
    }
    if (start < data.size()) leftover = data.substr(start);
  }

  cleanup:
    close(client_fd);
  {
    std::lock_guard lock(mtx_client);
    clientSockets.erase(client_fd);
  }
  --activeClients;
}

void _serverLoop(const std::string &socketPath) {
  struct sockaddr_un addr;

  if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return;
  }

  unlink(socketPath.c_str());
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(server_fd);
    return;
  }

  if (listen(server_fd, 5) < 0) {
    perror("listen");
    close(server_fd);
    return;
  }

  running = true;
  std::cout << "Listening on " << socketPath << "\n";

  while (running) {
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (!running) break;
      perror("accept");
      continue;
    }

    if (activeClients.load() >= maxClients) {
      close(client_fd);
      continue;
    }

    ++activeClients;
    std::thread clientThread(handleClient, client_fd);
    clientThread.detach();
  }

  close(server_fd);
  unlink(socketPath.c_str());
  running = false;
}

void start(const std::string &socketPath) {
  serverThread = std::thread(_serverLoop, socketPath);
}

void stop() {
  running = false;
  if (server_fd >= 0) {
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
  }
  if (serverThread.joinable()) serverThread.join();
}

} // namespace translate_server
