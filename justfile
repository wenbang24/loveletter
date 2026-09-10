buildserver:
    c++ -std=c++17 -Wall -Wextra -pedantic -DCROW_ENFORCE_WS_SPEC -Iserver -I"$(brew --prefix asio)/include" server/main.cpp server/server.cpp server/room.cpp -pthread -o server/out

test: buildserver
    node frontend-tests.mjs
    c++ -std=c++17 -Wall -Wextra -pedantic -DCROW_ENFORCE_WS_SPEC -Iserver -I"$(brew --prefix asio)/include" server/room_tests.cpp server/room.cpp -pthread -o server/room_tests
    ./server/room_tests
    node tests.mjs

runserver:
    ./server/out

brs: buildserver runserver
