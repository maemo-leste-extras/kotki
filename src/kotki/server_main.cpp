#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <map>
#include <vector>

#include "server.h"

#include "kotki/kotki.h"

using namespace std;

int main(const int argc, char** argv) {
  const std::string PATH_SOCKET = "/tmp/kotki_server.socket";

  KOTKI = new Kotki();
  KOTKI->scan();

  translate_server::start(PATH_SOCKET);
  std::cout << "daemon running. Press Ctrl+C to exit.\n";
  fflush(stdout);
  while (true) std::this_thread::sleep_for(std::chrono::seconds(10));

  translate_server::stop();
  return 0;
}
