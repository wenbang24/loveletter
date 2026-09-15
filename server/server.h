#pragma once

#include "room.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class Server {
  public:
    void run();

  private:
    struct Client {
        std::string id;
        std::string room;
        std::string name;
    };

    void        handleMessage(Connection& connection, const std::string& text, bool binary);
    void        leaveRoom(Client& client);
    std::string createRoomCode() const;

    std::unordered_map<Connection*, Client> clients_;
    std::unordered_map<std::string, Room>   rooms_;
    std::uint64_t                           nextClientId_ = 1;
};
