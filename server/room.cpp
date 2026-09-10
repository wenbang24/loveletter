#include "room.h"
#include "crow/websocket.h"

#include <algorithm>
#include <numeric>
#include <random>

namespace {
// Reject fractions, strings, and oversized integers before narrowing client input.
int cardIndex(const crow::json::rvalue& value, int limit) {
    if (value.t() != crow::json::type::Number) return -1;
    if (value.nt() == crow::json::num_type::Unsigned_integer) {
        const auto number = value.u();
        return number < static_cast<unsigned>(limit) ? static_cast<int>(number) : -1;
    }
    if (value.nt() == crow::json::num_type::Signed_integer) {
        const auto number = value.i();
        return number >= 0 && number < limit ? static_cast<int>(number) : -1;
    }
    return -1;
}
}

void Room::onJoin(const std::string& clientId) {
    joinOrder_.push_back(clientId);
    favors_.emplace(clientId, 0);
    if (status_ != "lobby") sendToClient(clientId, gameState());
}

void Room::onLeave(const std::string& clientId) {
    joinOrder_.erase(std::remove(joinOrder_.begin(), joinOrder_.end(), clientId), joinOrder_.end());
    favors_.erase(clientId);
    if (status_ != "active" || !hands_.count(clientId)) return;
    const bool wasCurrent = clientId == players_[turn_];
    eliminate(clientId, "left");
    if (players_.size() < 2) finishGame("players_left");
    else if (wasCurrent) beginTurn();
    else broadcast(gameState());
}

int Room::cardsRemaining() const {
    return static_cast<int>(deck_.size());
}

int Room::drawCard() {
    // Only called after checking that cards remain. The front is the next draw.
    const int card = deck_.front();
    deck_.pop_front();
    return card;
}

Json Room::gameState() const {
    Json state{{"type", "game_state"}, {"room", code_}, {"status", status_},
               {"cardsRemaining", cardsRemaining()},
               {"phase", chancellor_.empty() ? "play" : "chancellor"},
               {"currentPlayer", status_ == "active" ? Json(players_[turn_]) : Json(nullptr)}};
    state["players"] = players_;
    state["discards"] = Json::object{};
    for (const auto& entry : discards_) state["discards"][entry.first] = entry.second;
    state["favors"] = Json::object{};
    for (const auto& entry : favors_) state["favors"][entry.first] = entry.second;
    std::vector<std::string> protectedPlayers;
    for (const auto& player : players_)
        if (protected_.count(player)) protectedPlayers.push_back(player);
    state["protected"] = protectedPlayers;
    return state;
}

void Room::sendHand(const std::string& clientId) {
    const auto hand = hands_.find(clientId);
    sendToClient(clientId, Json{{"type", "hand"}, {"room", code_},
        {"held", hand != hands_.end() && hand->second >= 0 ? Json(hand->second) : Json(nullptr)},
        {"drawn", status_ == "active" && !players_.empty() && clientId == players_[turn_] && drawn_ >= 0
                      ? Json(drawn_) : Json(nullptr)}});
}

void Room::discardCard(const std::string& player, int card, const std::string& reason) {
    if (card < 0) return;
    discards_[player].push_back(card);
    broadcast(Json{{"type", "card_discarded"}, {"room", code_}, {"player", player},
                   {"card", card}, {"reason", reason}});
    if (card == 9) eliminate(player, "princess");
}

void Room::eliminate(const std::string& player, const std::string& reason) {
    const auto found = std::find(players_.begin(), players_.end(), player);
    if (found == players_.end()) return;
    const auto position = static_cast<std::size_t>(found - players_.begin());
    const bool wasCurrent = position == turn_;
    std::vector<int> discarded{hands_.at(player)};
    if (wasCurrent) {
        if (!chancellor_.empty()) discarded = std::move(chancellor_);
        chancellor_.clear();
        if (drawn_ >= 0) discarded.push_back(drawn_);
        drawn_ = -1;
    }
    // Remove first so discarding a Princess cannot recursively eliminate again.
    players_.erase(found);
    hands_.erase(player);
    protected_.erase(player);
    if (position < turn_) --turn_;
    if (!players_.empty()) turn_ %= players_.size();
    for (int card : discarded) discardCard(player, card, reason);
    broadcast(Json{{"type", "player_eliminated"}, {"room", code_}, {"player", player}, {"reason", reason}});
    sendHand(player);
}

void Room::beginTurn() {
    drawn_ = -1;
    if (cardsRemaining() == 0) {
        finishGame("deck_empty");
        return;
    }
    protected_.erase(players_[turn_]);
    drawn_ = drawCard();
    sendHand(players_[turn_]);
    broadcast(gameState());
}

void Room::completeTurn(const std::string& player) {
    if (players_.size() < 2) {
        finishGame("players_left");
        return;
    }
    const auto actor = std::find(players_.begin(), players_.end(), player);
    // An eliminated actor already left turn_ pointing at their successor.
    if (actor != players_.end()) turn_ = (actor - players_.begin() + 1) % players_.size();
    beginTurn();
}

void Room::finishGame(const std::string& reason) {
    if (status_ != "active") return;
    status_ = "finished";
    drawn_ = -1;
    // A sole remaining Chancellor actor wins with their original retained hand.
    chancellor_.clear();
    int highest = -1;
    std::vector<std::string> winners, spies;
    Json finalHands(Json::object{}), awards(Json::object{}), favors(Json::object{});
    for (const auto& player : players_) {
        const int card = hands_.at(player);
        finalHands[player] = card >= 0 ? Json(card) : Json(nullptr);
        if (card > highest) {
            highest = card;
            winners.clear();
        }
        if (card >= 0 && card == highest) winners.push_back(player);
        const auto& discarded = discards_[player];
        if (std::find(discarded.begin(), discarded.end(), 0) != discarded.end()) spies.push_back(player);
    }
    if (players_.size() == 1) winners = players_;
    for (auto& entry : favors_) {
        const int award = (std::find(winners.begin(), winners.end(), entry.first) != winners.end() ? 1 : 0) +
                          (spies.size() == 1 && spies.front() == entry.first ? 1 : 0);
        entry.second += award;
        awards[entry.first] = award;
        favors[entry.first] = entry.second;
    }
    Json result{{"type", "game_over"}, {"room", code_}, {"reason", reason}, {"hands", std::move(finalHands)},
                {"awards", std::move(awards)}, {"favors", std::move(favors)}};
    result["winners"] = winners;
    broadcast(gameState());
    broadcast(result);
}

void Room::onMessage(const std::string& clientId, const crow::json::rvalue& data) {
    const auto error = [&](const std::string& message) {
        sendToClient(clientId, Json{{"type", "error"}, {"message", message}});
    };
    const std::string action = data.t() == crow::json::type::Object && data.has("action") &&
        data["action"].t() == crow::json::type::String ? std::string(data["action"].s()) : "";
    if (action == "start_game") {
        if (status_ == "active") return error("A game is already active.");
        const auto deckSize = std::accumulate(cleandeck.begin(), cleandeck.end(), 0);
        if (joinOrder_.size() < 2 || joinOrder_.size() >= static_cast<std::size_t>(deckSize))
            return error("Starting requires 2 to 20 players.");
        players_ = joinOrder_;
        deck_.clear();
        for (int card = 0; card < static_cast<int>(cleandeck.size()); ++card)
            deck_.insert(deck_.end(), cleandeck[card], card);
        static std::mt19937 random(std::random_device{}());
        std::shuffle(deck_.begin(), deck_.end(), random);
        hands_.clear();
        discards_.clear();
        protected_.clear();
        chancellor_.clear();
        turn_ = 0;
        drawn_ = -1;
        status_ = "active";
        for (const auto& player : players_) {
            hands_[player] = drawCard();
            sendHand(player);
        }
        beginTurn();
        return;
    }
    if (action == "play_card" || action == "resolve_chancellor") {
        if (status_ != "active") return error("No game is active.");
        if (!hands_.count(clientId)) return error("Spectators cannot play.");
        if (clientId != players_[turn_]) return error("It is not your turn.");
        if (action == "resolve_chancellor") {
            if (chancellor_.empty()) return error("No Chancellor choice is pending.");
            const int count = static_cast<int>(chancellor_.size());
            const int keep = data.has("keep") ? cardIndex(data["keep"], count) : -1;
            if (keep < 0 || !data.has("bottom") || data["bottom"].t() != crow::json::type::List ||
                data["bottom"].size() != static_cast<std::size_t>(count - 1))
                return error("Choose one keep index and every other index in bottom order.");
            std::vector<int> order{keep};
            for (const auto& value : data["bottom"]) {
                const int index = cardIndex(value, count);
                if (index < 0 || std::find(order.begin(), order.end(), index) != order.end())
                    return error("Chancellor indices must be valid and unique.");
                order.push_back(index);
            }
            hands_.at(clientId) = chancellor_[keep];
            for (std::size_t i = 1; i < order.size(); ++i) deck_.push_back(chancellor_[order[i]]);
            chancellor_.clear();
            sendHand(clientId);
            completeTurn(clientId);
            return;
        }
        if (!chancellor_.empty()) return error("Resolve the Chancellor choice first.");
        if (!data.has("choice") || data["choice"].t() != crow::json::type::String ||
            (data["choice"].s() != "held" && data["choice"].s() != "drawn"))
            return error("Choose held or drawn.");
        const bool playHeld = data["choice"].s() == "held";
        const int held = hands_.at(clientId);
        const int played = playHeld ? held : drawn_;
        const int retained = playHeld ? drawn_ : held;
        if (retained == 8 && (played == 5 || played == 7))
            return error("You must play the Countess when holding a Prince or King.");
        std::vector<std::string> targets;
        const bool targeted = played == 1 || played == 2 || played == 3 || played == 5 || played == 7;
        if (targeted) {
            for (const auto& player : players_)
                if ((player == clientId && played == 5) || (player != clientId && !protected_.count(player)))
                    targets.push_back(player);
        }
        std::string target;
        if (targeted && (!targets.empty() || data.has("target"))) {
            if (!data.has("target") || data["target"].t() != crow::json::type::String)
                return error("Choose a legal target player.");
            target = data["target"].s();
            if (std::find(targets.begin(), targets.end(), target) == targets.end())
                return error("That player cannot be targeted.");
        }
        int guess = -1;
        if (played == 1 && (!target.empty() || data.has("guess"))) {
            guess = data.has("guess") ? cardIndex(data["guess"], 10) : -1;
            if (guess < 0 || guess == 1) return error("Guess a card from 0 to 9 other than Guard.");
        }

        hands_.at(clientId) = retained;
        drawn_ = -1;
        Json play{{"type", "card_played"}, {"room", code_}, {"player", clientId}, {"card", played}};
        if (!target.empty()) play["target"] = target;
        if (guess >= 0) play["guess"] = guess;
        broadcast(play);
        discardCard(clientId, played, "played");
        if (played == 1 && !target.empty()) {
            if (hands_.at(target) == guess) eliminate(target, "guard");
        } else if ((played == 2 || played == 3) && !target.empty()) {
            Json reveal{{"type", "cards_revealed"}, {"room", code_}, {"player", clientId}, {"card", played}};
            reveal["hands"][target] = hands_.at(target);
            if (played == 3) reveal["hands"][clientId] = hands_.at(clientId);
            sendToClient(clientId, reveal);
            if (played == 3) {
                sendToClient(target, reveal);
                if (hands_.at(clientId) < hands_.at(target)) eliminate(clientId, "baron");
                else if (hands_.at(target) < hands_.at(clientId)) eliminate(target, "baron");
            }
        } else if (played == 4) {
            protected_.insert(clientId);
        } else if (played == 5) {
            const int discarded = hands_.at(target);
            hands_.at(target) = -1;
            discardCard(target, discarded, "prince");
            if (hands_.count(target)) {
                if (!deck_.empty()) hands_.at(target) = drawCard();
                sendHand(target);
            }
        } else if (played == 6 && !deck_.empty()) {
            chancellor_ = {retained};
            while (chancellor_.size() < 3 && !deck_.empty()) chancellor_.push_back(drawCard());
            Json choice{{"type", "chancellor_choice"}, {"room", code_}};
            choice["cards"] = chancellor_;
            sendToClient(clientId, choice);
            broadcast(gameState());
            return;
        } else if (played == 7 && !target.empty()) {
            std::swap(hands_.at(clientId), hands_.at(target));
            sendHand(target);
        }
        // Eliminated players already received a cleared hand; self-Prince sent its replacement.
        if (hands_.count(clientId) && !(played == 5 && target == clientId)) sendHand(clientId);
        completeTurn(clientId);
        return;
    }
    // Preserve the room's private echo for unrelated application messages.
    sendToClient(clientId, Json{{"type", "server_reply"}, {"sender", "server"}, {"data", Json(data)}});
}

void Room::addClient(const std::string& clientId, Connection& connection) {
    if (members_.emplace(clientId, &connection).second) onJoin(clientId);
}

void Room::removeClient(const std::string& clientId) {
    if (members_.erase(clientId)) onLeave(clientId);
}

void Room::broadcast(const Json& data) {
    const auto message = data.dump();
    for (const auto& member : members_) member.second->send_text(message);
}

bool Room::sendToClient(const std::string& clientId, const Json& data) {
    const auto found = members_.find(clientId);
    if (found == members_.end()) return false;
    found->second->send_text(data.dump());
    return true;
}
