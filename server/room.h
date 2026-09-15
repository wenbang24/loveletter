#pragma once

#include "crow/json.h"

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace crow::websocket {
struct connection;
}
using Connection = crow::websocket::connection;
using Json       = crow::json::wvalue;

struct Card {
    int         strength;
    std::string name;
};

const std::array<Card, 10> deck = {
    {{0, "Spy"},
     {1, "Guard"},
     {2, "Priest"},
     {3, "Baron"},
     {4, "Handmaid"},
     {5, "Prince"},
     {6, "Chancellor"},
     {7, "King"},
     {8, "Countess"},
     {9, "Princess"}}
};
const std::array<int, 10> cleandeck = {2, 6, 2, 2, 2, 2, 2, 1, 1, 1};

// One instance per room. Call these methods only on the server's request worker.
class Room {
  public:
    explicit Room(const std::string& code) : code_(code) {}

    const std::string& code() const { return code_; }
    bool               empty() const { return members_.empty(); }
    void               addClient(const std::string& clientId, Connection& connection);
    void               setName(const std::string& clientId, const std::string& name);
    void               removeClient(const std::string& clientId);

    // Application entry point. The incoming data reference is valid only during this call.
    void onMessage(const std::string& clientId, const crow::json::rvalue& data);
    void broadcast(const Json& data);
    bool sendToClient(const std::string& clientId, const Json& data);

  private:
    friend struct RoomTestAccess;
    void onJoin(const std::string& clientId);
    void onLeave(const std::string& clientId);
    int  drawCard();
    int  cardsRemaining() const;
    Json gameState() const;
    void sendHand(const std::string& clientId);
    void beginTurn();
    void completeTurn(const std::string& player);
    void discardCard(const std::string& player, int card, const std::string& reason);
    void eliminate(const std::string& player, const std::string& reason);
    void finishGame(const std::string& reason);

    std::string                                  code_;
    std::unordered_map<std::string, std::string> names_;
    std::unordered_map<std::string, Connection*> members_;

    std::vector<std::string>             joinOrder_, players_;
    std::deque<int>                      deck_;
    std::unordered_map<std::string, int> hands_;
    std::unordered_map<std::string, std::vector<int>> discards_;
    std::unordered_map<std::string, int> favors_;
    std::unordered_set<std::string>     protected_;
    std::vector<int>                    chancellor_;
    std::string                         status_ = "lobby";
    std::size_t                         turn_ = 0;
    int                                 drawn_ = -1;
};
