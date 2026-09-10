#include "room.h"
#include "crow/websocket.h"

#include <cassert>
#include <iostream>

struct TestConnection : Connection {
    std::vector<std::string> messages;
    void send_text(std::string message) override { messages.push_back(std::move(message)); }
    void send_binary(std::string) override {}
    void send_ping(std::string) override {}
    void send_pong(std::string) override {}
    void close(const std::string&, uint16_t) override {}
    std::string get_remote_ip() override { return "test"; }
    std::string get_subprotocol() const override { return ""; }

    std::vector<crow::json::rvalue> events(const std::string& type) const {
        std::vector<crow::json::rvalue> result;
        for (const auto& text : messages) {
            auto event = crow::json::load(text);
            if (event["type"].s() == type) result.push_back(std::move(event));
        }
        return result;
    }
};

struct RoomTestAccess {
    Room room{"TEST23"};
    std::array<TestConnection, 4> connections;

    RoomTestAccess(int held, int drawn, int other = 4, std::deque<int> deck = {1, 2, 3, 4}, bool third = true) {
        room.addClient("a", connections[0]);
        room.addClient("b", connections[1]);
        if (third) room.addClient("c", connections[2]);
        room.players_ = room.joinOrder_;
        room.hands_ = {{"a", held}, {"b", other}};
        if (third) room.hands_["c"] = 2;
        room.drawn_ = drawn;
        room.deck_ = std::move(deck);
        room.status_ = "active";
        room.addClient("s", connections[3]); // A spectator receives public state only.
        clear();
    }

    void clear() { for (auto& connection : connections) connection.messages.clear(); }
    void act(const std::string& player, const std::string& json) {
        room.onMessage(player, crow::json::load(json));
    }
    void play(const std::string& choice = "held", const std::string& target = "", int guess = -1) {
        Json action{{"action", "play_card"}, {"choice", choice}};
        if (!target.empty()) action["target"] = target;
        if (guess >= 0) action["guess"] = guess;
        act("a", action.dump());
    }
    void reject(const std::string& json, const std::string& player = "a", int connection = 0) {
        const auto state = room.gameState().dump();
        const auto deck = room.deck_;
        const auto hands = room.hands_;
        const auto pending = room.chancellor_;
        const auto drawn = room.drawn_;
        const auto turn = room.turn_;
        clear();
        act(player, json);
        assert(room.gameState().dump() == state && room.deck_ == deck && room.hands_ == hands);
        assert(room.chancellor_ == pending && room.drawn_ == drawn && room.turn_ == turn);
        for (int i = 0; i < 4; ++i) {
            assert(connections[i].messages.size() == (i == connection ? 1 : 0));
            if (i == connection) assert(connections[i].events("error").size() == 1);
        }
        clear();
    }

    static void run() {
        // Guard validation, misses, hits, and eliminating a Princess exactly once.
        {
            RoomTestAccess t(1, 8, 9);
            for (const std::string& extra : {"", ",\"target\":null", ",\"target\":3", ",\"target\":\"a\"",
                    ",\"target\":\"s\"", ",\"target\":\"missing\"", ",\"target\":\"b\"",
                    ",\"target\":\"b\",\"guess\":1", ",\"target\":\"b\",\"guess\":10",
                    ",\"target\":\"b\",\"guess\":-1", ",\"target\":\"b\",\"guess\":2.5",
                    ",\"target\":\"b\",\"guess\":\"9\"", ",\"target\":\"b\",\"guess\":18446744073709551616",
                    ",\"target\":\"b\",\"guess\":-18446744073709551616"})
                t.reject("{\"action\":\"play_card\",\"choice\":\"held\"" + extra + "}");
            t.play("held", "b", 9);
            assert((t.room.players_ == std::vector<std::string>{"a", "c"}));
            assert(t.room.players_[t.room.turn_] == "c");
            assert(t.room.discards_["b"] == std::vector<int>{9});
            assert(t.connections[3].events("player_eliminated").size() == 1);
            t.reject(R"({"action":"play_card","choice":"held"})", "b", 1);
        }
        for (const auto& choice : {"held", "drawn"}) {
            RoomTestAccess t(1, 1);
            t.play(choice, "b", 0);
            assert(t.room.players_.size() == 3 && t.room.hands_["a"] == 1);
            assert(t.room.discards_["a"] == std::vector<int>{1});
        }
        // Priest reveals only to its actor. Baron reveals only to both participants.
        {
            RoomTestAccess t(2, 8, 7);
            t.play("held", "b");
            auto reveal = t.connections[0].events("cards_revealed");
            assert(reveal.size() == 1 && reveal[0]["hands"].size() == 1 && reveal[0]["hands"]["b"].i() == 7);
            for (int i = 1; i < 4; ++i) assert(t.connections[i].events("cards_revealed").empty());
        }
        for (int retained : {2, 4, 8}) {
            RoomTestAccess t(3, retained, 4);
            t.play("held", "b");
            for (int i = 0; i < 2; ++i) {
                auto reveal = t.connections[i].events("cards_revealed");
                assert(reveal.size() == 1 && reveal[0]["hands"].size() == 2);
                assert(reveal[0]["hands"]["a"].i() == retained && reveal[0]["hands"]["b"].i() == 4);
            }
            for (int i = 2; i < 4; ++i) assert(t.connections[i].events("cards_revealed").empty());
            assert(t.room.hands_.count("a") == (retained >= 4));
            assert(t.room.hands_.count("b") == (retained <= 4));
            assert(t.room.players_[t.room.turn_] == (retained > 4 ? "c" : "b"));
        }
        // Handmaid protects all targeted effects, expires on the next turn, and permits self-Prince.
        {
            RoomTestAccess t(4, 8);
            t.play();
            assert(t.room.protected_.count("a"));
            t.room.turn_ = 0;
            t.room.beginTurn();
            assert(!t.room.protected_.count("a"));
        }
        for (int card : {1, 2, 3, 5, 7}) {
            RoomTestAccess t(card, 4);
            t.room.protected_ = {"b", "c"};
            t.reject(R"({"action":"play_card","choice":"held","target":"b","guess":0})");
            if (card == 5) {
                t.room.protected_.insert("a");
                t.reject(R"({"action":"play_card","choice":"held"})");
                t.play("held", "a");
                assert(t.room.hands_["a"] == 1);
            } else {
                t.play();
                assert(t.room.hands_["a"] == 4 && t.room.players_.size() == 3);
            }
        }
        // Prince replaces either hand; empty decks produce a null hand and finish.
        for (const auto& target : {"a", "b"}) {
            RoomTestAccess t(5, 4, 0, {7, 2, 1});
            t.play("held", target);
            assert(t.room.hands_[target] == 7 && t.room.drawn_ == 2);
            assert(t.room.discards_[target].back() == (std::string(target) == "a" ? 4 : 0));
        }
        for (const auto& target : {"a", "b"}) {
            RoomTestAccess t(5, 4, 0, {});
            t.play("held", target);
            assert(t.room.status_ == "finished" && t.room.hands_[target] == -1);
            auto over = t.connections[3].events("game_over");
            assert(over.size() == 1 && over[0]["hands"][target].t() == crow::json::type::Null);
            for (const auto& winner : over[0]["winners"]) assert(winner.s() != target);
        }
        for (const auto& target : {"a", "b"}) {
            RoomTestAccess t(5, std::string(target) == "a" ? 9 : 4, 9);
            t.play("held", target);
            assert(!t.room.hands_.count(target) && t.room.deck_.size() == 3); // Only the next turn draws.
            assert(t.connections[3].events("player_eliminated").size() == 1);
        }
        // King swaps retained cards and informs only the affected owners.
        {
            RoomTestAccess t(7, 9, 0);
            t.play("held", "b");
            assert(t.room.hands_["a"] == 0 && t.room.hands_["b"] == 9);
            assert(t.connections[0].events("hand").front()["held"].i() == 0);
            assert(t.connections[1].events("hand").front()["held"].i() == 9);
            assert(t.connections[2].events("hand").empty() && t.connections[3].events("hand").empty());
        }
        // Countess applies to both hand positions, with Prince or King only.
        for (int card : {5, 7}) for (bool held : {false, true}) {
            RoomTestAccess t(held ? card : 8, held ? 8 : card);
            t.reject(std::string(R"({"action":"play_card","target":"b","choice":")") + (held ? "held" : "drawn") + "\"}");
            t.play(held ? "drawn" : "held");
            assert(t.room.hands_["a"] == card && t.room.discards_["a"] == std::vector<int>{8});
        }
        {
            RoomTestAccess t(8, 2);
            t.play();
            assert(t.room.hands_["a"] == 2);
        }
        for (bool held : {false, true}) {
            RoomTestAccess t(held ? 9 : 0, held ? 0 : 9);
            t.play(held ? "held" : "drawn");
            assert(!t.room.hands_.count("a") && t.room.players_[t.room.turn_] == "b");
            assert((t.room.discards_["a"] == std::vector<int>{9, 0}));
        }
        // Chancellor candidates are private and duplicate values have distinct indices.
        {
            RoomTestAccess t(6, 9, 4, {9, 1, 2, 3});
            t.play();
            assert((t.room.chancellor_ == std::vector<int>{9, 9, 1}));
            assert(t.room.turn_ == 0 && t.room.hands_["a"] == 9);
            assert(t.connections[0].events("chancellor_choice").size() == 1);
            for (int i = 1; i < 4; ++i) assert(t.connections[i].events("chancellor_choice").empty());
            for (auto& connection : t.connections) {
                for (const auto& event : connection.events("game_state")) {
                    assert(event["phase"].s() == "chancellor" && !event.has("hands") && !event.has("cards"));
                }
            }
            t.reject(R"({"action":"play_card","choice":"held"})");
            t.reject(R"({"action":"resolve_chancellor","keep":0,"bottom":[1,2]})", "b", 1);
            t.reject(R"({"action":"resolve_chancellor","keep":0,"bottom":[1,2]})", "s", 3);
            for (const auto& invalid : {R"({})", R"({"keep":0})", R"({"keep":0,"bottom":null})",
                    R"({"keep":0,"bottom":[1]})", R"({"keep":0,"bottom":[1,1]})", R"({"keep":0,"bottom":[0,1]})",
                    R"({"keep":0,"bottom":[1,3]})", R"({"keep":3,"bottom":[1,2]})", R"({"keep":-1,"bottom":[1,2]})",
                    R"({"keep":"0","bottom":[1,2]})", R"({"keep":0.5,"bottom":[1,2]})",
                    R"({"keep":0,"bottom":[1,"2"]})", R"({"keep":0,"bottom":[1,18446744073709551616]})"}) {
                auto data = Json(crow::json::load(invalid));
                data["action"] = "resolve_chancellor";
                t.reject(data.dump());
            }
            t.act("a", R"({"action":"resolve_chancellor","keep":1,"bottom":[2,0]})");
            assert(t.room.chancellor_.empty() && t.room.drawn_ == 2 && t.room.hands_["a"] == 9);
            assert((t.room.deck_ == std::deque<int>{3, 1, 9}));
            assert(t.room.discards_["a"] == std::vector<int>{6});
            assert(t.room.drawCard() == 3 && t.room.drawCard() == 1 && t.room.drawCard() == 9);
            t.reject(R"({"action":"resolve_chancellor","keep":1,"bottom":[2,0]})");
        }
        for (int count : {0, 1, 2}) {
            RoomTestAccess t(6, 0, 4, std::deque<int>(count, 1));
            t.play();
            if (count == 0) {
                assert(t.room.status_ == "finished" && t.room.hands_["a"] == 0);
            } else {
                assert(t.room.chancellor_.size() == static_cast<unsigned>(count + 1));
                t.act("a", count == 1 ? R"({"action":"resolve_chancellor","keep":1,"bottom":[0]})" :
                                       R"({"action":"resolve_chancellor","keep":1,"bottom":[2,0]})");
                assert(t.room.status_ == "active" && t.room.deck_.size() == static_cast<unsigned>(count - 1));
                assert(t.room.discards_["a"] == std::vector<int>{6}); // Returning Spy earns no bonus.
            }
        }
        // Spy awards are independent of winning and only consider surviving owners.
        for (int spyCase = 0; spyCase < 5; ++spyCase) {
            RoomTestAccess t(0, 4, 8, {});
            if (spyCase == 1) t.room.discards_["a"] = {0}; // Two Spies, one owner.
            if (spyCase == 2) t.room.discards_["b"] = {0}; // Two surviving owners: no bonus.
            if (spyCase == 3) { t.room.discards_["c"] = {0}; t.room.eliminate("c", "guard"); }
            if (spyCase == 4) t.room.hands_["b"] = 4; // Shared round winners still each get one.
            t.play();
            assert(t.room.favors_["a"] == (spyCase == 2 ? 0 : spyCase == 4 ? 2 : 1));
            assert(t.room.favors_["b"] == 1 && t.room.favors_["c"] == 0 && t.room.favors_["s"] == 0);
            const auto favors = t.room.favors_;
            t.room.finishGame("deck_empty");
            assert(t.room.favors_ == favors); // Award once only.
            t.act("a", R"({"action":"start_game"})");
            assert(t.room.favors_ == favors && t.room.discards_.empty() && t.room.protected_.empty());
            assert(t.room.players_.back() == "s" && t.room.chancellor_.empty());
            std::array<int, 10> counts{};
            for (int card : t.room.deck_) ++counts[card];
            for (const auto& hand : t.room.hands_) ++counts[hand.second];
            ++counts[t.room.drawn_];
            assert(counts == cleandeck);
        }
        // A sole survivor gets the win and Spy bonus; an eliminated Spy gets neither.
        {
            RoomTestAccess t(1, 0, 9, {2}, false);
            t.room.discards_["a"] = {0};
            t.room.discards_["b"] = {0};
            t.play("held", "b", 9);
            assert(t.room.status_ == "finished" && t.room.favors_["a"] == 2 && t.room.favors_["b"] == 0);
        }
        // Departures preserve turn order and handle every pending Chancellor outcome.
        {
            RoomTestAccess t(6, 8, 4, {0, 9, 2, 1});
            t.play();
            t.room.removeClient("a");
            assert(t.room.chancellor_.empty() && !t.room.favors_.count("a"));
            assert((t.room.discards_["a"] == std::vector<int>{6, 8, 0, 9}));
            assert(t.room.players_[t.room.turn_] == "b" && t.room.drawn_ == 2);
        }
        {
            RoomTestAccess t(6, 8, 4, {0, 9, 2});
            t.play();
            t.room.removeClient("b");
            assert(t.room.chancellor_.size() == 3 && t.room.players_[t.room.turn_] == "a");
            t.room.removeClient("c");
            assert(t.room.status_ == "finished" && t.room.chancellor_.empty());
            assert(t.room.hands_["a"] == 8 && t.room.favors_["a"] == 1);
            assert(t.room.discards_["a"] == std::vector<int>{6});
        }
        {
            RoomTestAccess t(6, 8, 4, {0, 9});
            t.play();
            t.room.removeClient("a");
            assert(t.room.status_ == "finished" && t.room.favors_["b"] == 1);
        }
        {
            RoomTestAccess t(4, 8);
            t.play(); // b is current.
            t.room.removeClient("a");
            assert(t.room.players_[t.room.turn_] == "b" && t.room.drawn_ == 1);
            t.room.removeClient("b");
            assert(t.room.status_ == "finished" && t.room.favors_["c"] == 1);
        }
        {
            RoomTestAccess t(9, 8);
            t.play();
            t.room.removeClient("a"); // Eliminated player leaving cannot change the current turn.
            assert(t.room.players_[t.room.turn_] == "b" && !t.room.favors_.count("a"));
            const auto state = t.room.players_;
            t.room.removeClient("s");
            assert(t.room.players_ == state && t.room.drawn_ == 1);
        }
        // If nobody remains when finishing, nobody receives a token.
        {
            RoomTestAccess t(0, 4, 8, {});
            t.room.eliminate("a", "left");
            t.room.eliminate("b", "left");
            t.room.eliminate("c", "left");
            t.room.finishGame("players_left");
            assert(t.connections[3].events("game_over").front()["winners"].size() == 0);
        }
        std::cout << "PASS: all card effects, validation, private reveals, Chancellor ordering, scoring, and departures.\n";
    }
};

int main() { RoomTestAccess::run(); }
