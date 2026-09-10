#include "server.h"
#include "crow/app.h"

#include <random>

void Server::leaveRoom(Client& client) {
    if (client.room.empty()) return;
    const auto previous = client.room;
    client.room.clear();
    auto& room = rooms_.at(previous);
    room.removeClient(client.id);
    if (room.empty()) rooms_.erase(previous);
}

std::string Server::createRoomCode() const {
    static std::random_device random;
    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(alphabet) - 2);
    std::string code(6, ' ');
    do {
        for (auto& character : code) character = alphabet[pick(random)];
    } while (rooms_.count(code));
    return code;
}

void Server::handleMessage(Connection& connection, const std::string& text, bool binary) {
    auto& client = clients_.at(&connection);
    const auto error = [&](const std::string& message) {
        connection.send_text(Json{{"type", "error"}, {"message", message}}.dump());
    };
    if (binary) {
        error("Send JSON text messages, not binary data.");
        return;
    }
    const auto message = crow::json::load(text);
    if (!message || message.t() != crow::json::type::Object ||
        !message.has("type") || message["type"].t() != crow::json::type::String) {
        error("Expected a JSON object with a string type field.");
        return;
    }
    const std::string type = message["type"].s();
    if (type == "create_room" || type == "join_room") {
        std::string room;
        if (type == "create_room") {
            room = createRoomCode();
            rooms_.try_emplace(room, room);
        } else {
            if (!message.has("room") || message["room"].t() != crow::json::type::String) {
                error("Expected a six-character room code.");
                return;
            }
            room = message["room"].s();
            if (room.size() != 6 || room.find_first_not_of("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") != std::string::npos) {
                error("Expected a six-character room code using uppercase letters and digits 2–9, excluding I and O.");
                return;
            }
            if (!rooms_.count(room)) {
                error("Room not found.");
                return;
            }
        }
        if (client.room != room) {
            leaveRoom(client);
            client.room = room;
            rooms_.at(room).addClient(client.id, connection);
        }
        connection.send_text(Json{{"type", "room_joined"}, {"room", room}}.dump());
    } else if (type == "broadcast" || type == "server_message") {
        if (!message.has("data")) {
            error("Expected a data field containing any JSON value.");
            return;
        }
        if (type == "broadcast") {
            if (client.room.empty()) {
                error("Join a room before broadcasting.");
                return;
            }
            rooms_.at(client.room).broadcast(Json{{"type", "room_message"}, {"room", client.room},
                {"sender", client.id}, {"data", Json(message["data"])}});
        } else if (!client.room.empty()) {
            rooms_.at(client.room).onMessage(client.id, message["data"]);
        } else {
            // Clients outside rooms retain the original private echo.
            connection.send_text(Json{{"type", "server_reply"}, {"sender", "server"},
                {"data", Json(message["data"])}}.dump());
        }
    } else {
        error("Unknown message type.");
    }
}

void Server::run() {
    crow::SimpleApp app;
    CROW_ROUTE(app, "/")([] {
        crow::response response;
        response.set_static_file_info("index.html", "text/html; charset=utf-8");
        return response;
    });

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([this](Connection& connection) {
            auto& client = clients_[&connection];
            client.id = std::to_string(nextClientId_++);
            connection.send_text(Json{{"type", "welcome"}, {"clientId", client.id}}.dump());
        })
        .onclose([this](Connection& connection, const std::string&, std::uint16_t) {
            const auto found = clients_.find(&connection);
            if (found == clients_.end()) return;
            leaveRoom(found->second);
            clients_.erase(found);
        })
        .onmessage([this](Connection& connection, const std::string& text, bool binary) {
            handleMessage(connection, text, binary);
        });

    // ponytail: one request worker avoids locks; synchronize state before adding workers or background sends.
    app.bindaddr("127.0.0.1").port(8080).concurrency(2).websocket_max_payload(64 * 1024).run();
}
