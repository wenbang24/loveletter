import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import net from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';

// Own the test server: fail rather than accidentally test or stop another process.
const probe = net.createServer();
probe.listen(8080, '127.0.0.1');
await once(probe, 'listening');
await new Promise(resolve => probe.close(resolve));
const server = spawn('./server/out', [], { cwd: import.meta.dirname, stdio: ['ignore', 'pipe', 'pipe'] });
const exited = once(server, 'exit');
let output = '';
server.stdout.on('data', chunk => output += chunk);
server.stderr.on('data', chunk => output += chunk);
const peers = [];

async function until(check, description) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (await check()) return;
    await delay(10);
  }
  throw new Error(`Timed out: ${description}\n${output}`);
}

async function connect() {
  const socket = new WebSocket('ws://127.0.0.1:8080/ws');
  const messages = [];
  const nameUpdates = [];
  const peer = {
    socket, messages, nameUpdates,
    send(type, fields = {}) { socket.send(JSON.stringify({ ...fields, type })); },
    async next(type) {
      await until(() => messages.length > 0, `waiting for ${type}`);
      const message = messages.shift();
      assert.equal(message.type, type);
      return message;
    },
    async close() {
      socket.close();
      await until(() => socket.readyState === WebSocket.CLOSED, 'closing connection');
    },
  };
  peers.push(peer);
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    (message.type === 'player_names' ? nameUpdates : messages).push(message);
  });
  peer.id = (await peer.next('welcome')).clientId;
  return peer;
}

async function quiet() {
  await delay(150);
  for (const peer of peers) assert.deepEqual(peer.messages, [], 'unexpected recipient or duplicate message');
}

const deckCounts = [2, 6, 2, 2, 2, 2, 2, 1, 1, 1];
const action = (peer, name, fields = {}) => peer.send('server_message', { data: { action: name, ...fields } });

async function publicEvent(viewers, type) {
  const events = await Promise.all(viewers.map(peer => peer.next(type)));
  for (const event of events) assert.deepEqual(event, events[0]);
  return events[0];
}

async function readHand(peer, room) {
  const hand = await peer.next('hand');
  assert.equal(hand.room, room);
  for (const card of [hand.held, hand.drawn]) {
    assert.ok(card === null || (Number.isInteger(card) && card >= 0 && card <= 9));
  }
  assert.notEqual(hand.held, null);
  peer.hand = hand;
  return hand;
}

function countDraw(game, card) {
  assert.ok(--game.remaining[card] >= 0, `too many copies of card ${card}`);
}

async function startGame(players, room, starter = players[0]) {
  action(starter, 'start_game');
  const game = { players: [...players], viewers: [...players], room, remaining: [...deckCounts] };
  for (const peer of players) {
    const hand = await readHand(peer, room);
    assert.equal(hand.drawn, null);
    countDraw(game, hand.held);
  }
  const held = players[0].hand.held;
  assert.equal((await readHand(players[0], room)).held, held);
  assert.notEqual(players[0].hand.drawn, null);
  countDraw(game, players[0].hand.drawn);
  game.state = await publicEvent(players, 'game_state');
  assert.deepEqual(game.state, {
    type: 'game_state', room, status: 'active', players: players.map(peer => peer.id),
    currentPlayer: players[0].id, cardsRemaining: 20 - players.length,
    phase: 'play', protected: [], discards: {},
    favors: Object.fromEntries(players.map(peer => [peer.id, peer.favors ?? 0])),
  });
  return game;
}

const publicTypes = new Set(['card_played', 'card_discarded', 'player_eliminated', 'game_state']);

async function readUpdate(game, context = {}) {
  await until(() => game.viewers.every(peer => peer.messages.some(message => message.type === 'game_state')), 'public game update');
  const batches = game.viewers.map(peer => peer.messages.splice(0, peer.messages.findIndex(message => message.type === 'game_state') + 1));
  const state = batches[0].at(-1);
  const publicMessages = batches[0].filter(message => publicTypes.has(message.type));
  for (let i = 0; i < batches.length; i++) {
    const peer = game.viewers[i];
    assert.deepEqual(batches[i].filter(message => publicTypes.has(message.type)), publicMessages);
    for (const message of batches[i]) {
      assert.equal(message.room, game.room);
      if (message.type === 'hand') {
        assert.ok([context.actor, context.target, state.currentPlayer].includes(peer.id), 'hand sent to an unrelated viewer');
        for (const card of [message.held, message.drawn]) assert.ok(card === null || (Number.isInteger(card) && card >= 0 && card <= 9));
        if (message.drawn !== null) assert.equal(peer.id, state.currentPlayer);
        peer.hand = message;
      } else if (message.type === 'cards_revealed') {
        assert.ok(peer.id === context.actor || (context.card === 3 && peer.id === context.target));
        assert.equal(message.card, context.card);
        assert.equal(message.player, context.actor);
        assert.deepEqual(message.hands, context.card === 2
          ? { [context.target]: context.targetHand }
          : { [context.actor]: context.retained, [context.target]: context.targetHand });
      } else if (message.type === 'chancellor_choice') {
        assert.equal(peer.id, context.actor);
        assert.equal(message.cards[0], context.retained);
        assert.ok(message.cards.length >= 2 && message.cards.length <= 3);
        game.choice = message.cards;
      } else {
        assert.ok(publicTypes.has(message.type), `unexpected game event: ${message.type}`);
      }
    }
  }
  if (context.card !== undefined) {
    assert.deepEqual(publicMessages[0], {
      type: 'card_played', room: game.room, player: context.actor, card: context.card,
      ...(context.target ? { target: context.target } : {}),
      ...(context.card === 1 && context.target ? { guess: 0 } : {}),
    });
    assert.deepEqual(publicMessages[1], { type: 'card_discarded', room: game.room, player: context.actor, card: context.card, reason: 'played' });
  }
  assert.deepEqual(Object.keys(state).sort(), ['type', 'room', 'status', 'players', 'currentPlayer', 'cardsRemaining', 'phase', 'protected', 'discards', 'favors'].sort());
  assert.ok(state.protected.every(id => state.players.includes(id)));
  if (state.status === 'active') {
    assert.ok(state.players.includes(state.currentPlayer));
    if (state.phase === 'play') assert.ok(!state.protected.includes(state.currentPlayer));
    // Every card remains in the deck, a hand, a public discard, or a pending choice.
    const held = game.viewers.filter(peer => state.players.includes(peer.id)).map(peer => peer.hand.held);
    const current = game.viewers.find(peer => peer.id === state.currentPlayer);
    const extra = state.phase === 'chancellor' ? game.choice.length - 1 : Number(current.hand.drawn !== null);
    assert.equal(state.cardsRemaining + held.length + extra + Object.values(state.discards).flat().length, 21);
  }
  if (state.status === 'finished') {
    const result = await publicEvent(game.viewers, 'game_over');
    const hands = Object.fromEntries(game.viewers.filter(peer => state.players.includes(peer.id)).map(peer => [peer.id, peer.hand.held]));
    assert.deepEqual(result.hands, hands);
    const highest = Math.max(...Object.values(hands).filter(card => card !== null));
    const winners = state.players.filter(id => state.players.length === 1 || (hands[id] !== null && hands[id] === highest));
    assert.deepEqual(result.winners, winners);
    const spies = state.players.filter(id => state.discards[id]?.includes(0));
    for (const peer of game.viewers) {
      const award = Number(winners.includes(peer.id)) + Number(spies.length === 1 && spies[0] === peer.id);
      assert.equal(result.awards[peer.id], award);
      assert.equal(result.favors[peer.id], (peer.favors ?? 0) + award);
      peer.favors = result.favors[peer.id];
    }
    assert.deepEqual(result.favors, state.favors);
    game.result = result;
  }
  game.state = state;
  return state;
}

async function playTurn(game, preferred = 'held') {
  const peer = game.viewers.find(peer => peer.id === game.state.currentPlayer);
  if (game.state.phase === 'chancellor') {
    peer.send('set_name', { name: 'Chancellor 王' });
    await peer.next('name_updated');
    await until(() => game.viewers.every(viewer => viewer.nameUpdates.at(-1).names[peer.id] === 'Chancellor 王'), 'rename during Chancellor');
    action(peer, 'resolve_chancellor', { keep: 0, bottom: game.choice.slice(1).map((_, i) => i + 1).reverse() });
    await readUpdate(game, { actor: peer.id });
    return;
  }
  let choice = preferred;
  if ([peer.hand.held, peer.hand.drawn].includes(8) && [peer.hand.held, peer.hand.drawn].some(card => card === 5 || card === 7))
    choice = peer.hand.held === 8 ? 'held' : 'drawn';
  const card = peer.hand[choice];
  const retained = peer.hand[choice === 'held' ? 'drawn' : 'held'];
  const fields = { choice };
  if ([1, 2, 3, 5, 7].includes(card)) {
    fields.target = game.state.players.find(id => id !== peer.id && !game.state.protected.includes(id));
    if (!fields.target && card === 5) fields.target = peer.id;
    if (fields.target && card === 1) fields.guess = 0;
  }
  const targetHand = game.viewers.find(viewer => viewer.id === fields.target)?.hand.held;
  action(peer, 'play_card', fields);
  // During a Chancellor choice, the ordinary hand remains the retained original.
  peer.hand = { ...peer.hand, held: retained, drawn: null };
  await readUpdate(game, { actor: peer.id, target: fields.target, card, retained, targetHand: fields.target === peer.id ? retained : targetHand });
}

async function finishRound(game) {
  let turns = 0;
  while (game.state.status === 'active') {
    assert.ok(++turns < 100, 'round did not make progress');
    await playTurn(game, turns % 2 ? 'held' : 'drawn');
  }
}

try {
  await until(async () => {
    if (server.exitCode !== null) throw new Error(output);
    try { return (await fetch('http://127.0.0.1:8080')).ok; } catch { return false; }
  }, 'server startup');
  const response = await fetch('http://127.0.0.1:8080');
  assert.match(response.headers.get('content-type'), /text\/html/);
  assert.match(await response.text(), /Create Room/);

  const a = await connect();
  const b = await connect();
  const c = await connect();
  assert.equal(new Set([a.id, b.id, c.id]).size, 3);

  b.send('server_message', { data: { beforeJoining: true } });
  assert.deepEqual(await b.next('server_reply'), {
    type: 'server_reply', sender: 'server', data: { beforeJoining: true },
  });
  await quiet();

  a.send('broadcast', { data: 'too early' });
  await a.next('error');
  a.send('create_room', { name: ' Alice ' });
  const room = (await a.next('room_joined')).room;
  assert.match(room, /^[A-HJ-NP-Z2-9]{6}$/);
  b.send('join_room', { room, name: 'Bob' });
  assert.equal((await b.next('room_joined')).room, room);
  b.send('join_room', { room }); // Joining the same room is harmless.
  await b.next('room_joined');
  c.send('create_room');
  const otherRoom = (await c.next('room_joined')).room;
  assert.notEqual(room, otherRoom);
  c.send('join_room', { room: otherRoom }); // Rejoining as the sole member preserves the room.
  assert.equal((await c.next('room_joined')).room, otherRoom);

  await until(() => b.nameUpdates.length >= 2 && c.nameUpdates.length >= 2, 'initial name maps');
  assert.equal(b.nameUpdates.at(-1).names[a.id], 'Alice');
  const isolatedCount = c.nameUpdates.length;
  a.send('set_name', { name: '  Zoë 王😀  ' });
  assert.equal((await a.next('name_updated')).name, 'Zoë 王😀');
  await until(() => b.nameUpdates.at(-1).names[a.id] === 'Zoë 王😀', 'room rename');
  assert.equal(c.nameUpdates.length, isolatedCount);
  for (const name of [42, null, 'x'.repeat(33), 'bad\nname', '\u007f', '\u0085', '\ud800']) {
    a.send('set_name', { name });
    await a.next('error');
  }
  a.send('set_name');
  await a.next('error');
  a.send('create_room', { name: 'x'.repeat(33) });
  await a.next('error');
  a.send('join_room', { room: otherRoom, name: 42 });
  await a.next('error');
  for (const name of ['😀'.repeat(32), '<b>Alice</b>', '']) {
    a.send('set_name', { name });
    assert.equal((await a.next('name_updated')).name, name);
    await until(() => b.nameUpdates.at(-1).names[a.id] === (name || `Player ${a.id}`), 'accepted name');
  }

  a.send('server_message', { data: { room, action: 'first room' } });
  c.send('server_message', { data: { room: otherRoom, action: 'second room' } });
  assert.deepEqual(await a.next('server_reply'), {
    type: 'server_reply', sender: 'server', data: { room, action: 'first room' },
  });
  assert.deepEqual(await c.next('server_reply'), {
    type: 'server_reply', sender: 'server', data: { room: otherRoom, action: 'second room' },
  });
  await quiet(); // Each room handler replies only to its own requesting member.

  const data = { text: '<script>not HTML</script>', values: [1, true, null], nested: { x: 2 } };
  a.send('broadcast', { data });
  const expected = { type: 'room_message', room, sender: a.id, data };
  assert.deepEqual(await a.next('room_message'), expected);
  assert.deepEqual(await b.next('room_message'), expected);
  await quiet(); // C is in a different room.

  b.send('server_message', { data });
  assert.deepEqual(await b.next('server_reply'), { type: 'server_reply', sender: 'server', data });
  await quiet(); // Neither A nor C receives the private reply.

  for (const invalid of ['{', 'null', '[]', '{}', '{"type":3}', '{"type":"unknown"}',
    '{"type":"join_room"}', '{"type":"join_room","room":3}',
    '{"type":"join_room","room":"bad"}', '{"type":"broadcast"}', '{"type":"server_message"}']) {
    b.socket.send(invalid);
    assert.equal(typeof (await b.next('error')).message, 'string');
  }
  b.socket.send(new Uint8Array([1, 2, 3]));
  await b.next('error');
  let missingRoom = 'ZZZZZZ';
  if ([room, otherRoom].includes(missingRoom)) missingRoom = 'YYYYYY';
  if ([room, otherRoom].includes(missingRoom)) missingRoom = 'XXXXXX';
  b.send('join_room', { room: missingRoom });
  await b.next('error');
  b.send('broadcast', { data: null }); // Failed joins preserve membership; null is valid data.
  assert.equal((await b.next('room_message')).room, room);
  assert.equal((await a.next('room_message')).data, null);

  b.send('join_room', { room: otherRoom });
  await b.next('room_joined');
  await until(() => b.nameUpdates.at(-1).room === otherRoom, 'names after switching');
  assert.equal(b.nameUpdates.at(-1).names[b.id], 'Bob');
  b.send('server_message', { data: { afterSwitching: true } });
  assert.deepEqual(await b.next('server_reply'), {
    type: 'server_reply', sender: 'server', data: { afterSwitching: true },
  });
  await quiet(); // The new room must know B; the old room can no longer send to B.
  a.send('broadcast', { data: 'old room' });
  await a.next('room_message');
  c.send('broadcast', { data: 'new room' });
  await c.next('room_message');
  assert.equal((await b.next('room_message')).data, 'new room');
  await quiet();

  await a.close();
  b.send('join_room', { room });
  await b.next('error'); // Last disconnect deletes the room.
  await c.close();
  b.send('broadcast', { data: 'still here' });
  await b.next('room_message'); // Disconnecting one member preserves the room.
  b.send('create_room');
  await b.next('room_joined');
  b.send('join_room', { room: otherRoom });
  await b.next('error'); // Leaving the last membership also deletes a room.

  const oversized = await connect();
  oversized.socket.send('x'.repeat(64 * 1024 + 1));
  await until(() => oversized.socket.readyState === WebSocket.CLOSED, 'oversized message rejection');
  b.send('server_message', { data: 42 });
  assert.equal((await b.next('server_reply')).data, 42);
  await quiet();
  const players = await Promise.all([connect(), connect(), connect()]);
  players[0].send('create_room');
  const gameRoom = (await players[0].next('room_joined')).room;
  action(players[0], 'start_game');
  await players[0].next('error'); // Too few players.
  action(players[0], 'play_card', { choice: 'held' });
  await players[0].next('error');
  for (const peer of players.slice(1)) {
    peer.send('join_room', { room: gameRoom });
    await peer.next('room_joined');
  }
  let game = await startGame(players, gameRoom, players[2]);
  const spectator = await connect();
  spectator.send('join_room', { room: gameRoom });
  assert.deepEqual(await spectator.next('game_state'), { ...game.state, favors: { ...game.state.favors, [spectator.id]: 0 } });
  await spectator.next('room_joined');
  spectator.send('set_name', { name: 'Watcher' });
  await spectator.next('name_updated');
  await until(() => players[0].nameUpdates.at(-1).names[spectator.id] === 'Watcher', 'spectator rename');
  game.viewers.push(spectator);
  spectator.send('join_room', { room: gameRoom });
  await spectator.next('room_joined'); // No duplicate seat or draw.
  action(players[0], 'start_game');
  await players[0].next('error');
  action(players[1], 'play_card', { choice: 'held' });
  await players[1].next('error');
  action(spectator, 'play_card', { choice: 'drawn' });
  await spectator.next('error');
  action(players[0], 'resolve_chancellor', { keep: 0, bottom: [1, 2] });
  await players[0].next('error');
  for (const fields of [{}, { choice: null }, { choice: 0 }, { choice: [] }, { choice: 'other' }]) {
    action(players[0], 'play_card', fields);
    await players[0].next('error');
  }
  await quiet(); // Invalid actions and private hands never reach another client or room.
  await finishRound(game);
  action(players[0], 'play_card', { choice: 'held' });
  await players[0].next('error');
  await quiet();

  // Every restart includes the former spectator and preserves favor totals.
  // Deterministic C++ checks cover each effect; these rounds exercise real message delivery.
  for (let round = 0; round < 6; round++) {
    game = await startGame([...players, spectator], gameRoom, spectator);
    await finishRound(game);
  }
  game = await startGame([...players, spectator], gameRoom);
  const visitor = await connect();
  visitor.send('join_room', { room: gameRoom });
  await visitor.next('game_state');
  await visitor.next('room_joined');
  await visitor.close();
  await quiet(); // A spectator departure does not change turns.

  // A waiting player leaves; the current player's draw and turn remain intact.
  const originalHand = { ...players[0].hand };
  players[1].send('set_name', { name: 'Departing player' });
  await players[1].next('name_updated');
  await until(() => players[0].nameUpdates.at(-1).names[players[1].id] === 'Departing player', 'name before departure');
  players[1].send('create_room');
  await players[1].next('room_joined');
  game.viewers = game.viewers.filter(peer => peer !== players[1]);
  await readUpdate(game);
  assert.equal(game.state.currentPlayer, players[0].id);
  assert.deepEqual(players[0].hand, originalHand);
  assert.ok(!Object.hasOwn(game.state.favors, players[1].id));
  assert.equal(players[0].nameUpdates.at(-1).names[players[1].id], 'Departing player');

  // The current player disconnects; the next survivor draws exactly once.
  const beforeLeave = game.state.cardsRemaining;
  await players[0].close();
  game.viewers = game.viewers.filter(peer => peer !== players[0]);
  await readUpdate(game);
  assert.equal(game.state.currentPlayer, players[2].id);
  assert.equal(game.state.cardsRemaining, beforeLeave - 1);
  await spectator.close();
  game.viewers = game.viewers.filter(peer => peer !== spectator);
  await readUpdate(game);
  assert.equal(game.result.reason, 'players_left');
  assert.deepEqual(game.result.winners, [players[2].id]);
  await quiet();

  // Twenty seats consume all 21 cards at the start, deterministically checking deck counts.
  const crowd = await Promise.all(Array.from({ length: 21 }, () => connect()));
  crowd[0].send('create_room');
  const crowdedRoom = (await crowd[0].next('room_joined')).room;
  for (const peer of crowd.slice(1)) {
    peer.send('join_room', { room: crowdedRoom });
    await peer.next('room_joined');
  }
  action(crowd[0], 'start_game');
  await crowd[0].next('error');
  await quiet(); // Too many players; no partial deal.
  await crowd.pop().close();
  game = await startGame(crowd, crowdedRoom);
  assert.deepEqual(game.remaining, Array(10).fill(0));
  const pair = crowd.slice(1).filter(peer => peer.hand.held === 1).slice(0, 2);
  assert.equal(pair.length, 2);
  for (const peer of crowd.slice(1).filter(peer => !pair.includes(peer))) {
    await peer.close();
    game.viewers = game.viewers.filter(viewer => viewer !== peer);
    await readUpdate(game);
    assert.equal(game.state.currentPlayer, crowd[0].id);
    assert.equal(game.state.cardsRemaining, 0);
  }
  await crowd[0].close(); // No next draw: both retained Guards win.
  game.viewers = pair;
  await readUpdate(game);
  assert.equal(game.result.reason, 'deck_empty');
  assert.deepEqual(game.result.winners, pair.map(peer => peer.id));
  assert.deepEqual(game.result.awards, Object.fromEntries(pair.map(peer => [peer.id, 1])));
  await quiet();
  console.log('PASS: HTTP, room isolation, validation, cleanup, effect-aware turns, private events, spectators, departures, deck counts, ties, and favor totals across rounds.');

} finally {
  for (const peer of peers) peer.socket.close();
  server.kill('SIGTERM');
  const killTimer = setTimeout(() => server.kill('SIGKILL'), 3000);
  await exited;
  clearTimeout(killTimer);
}
