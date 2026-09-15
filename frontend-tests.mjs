import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Exercise the actual inline model without a browser or a second implementation.
const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const source = html.match(/<script id="game-logic">([\s\S]*?)<\/script>/)[1];
const api = vm.runInNewContext(`${source}\n({ newModel, receive, legalTargets, forcedChoice, chooseKeep, playerName })`);
const plain = value => JSON.parse(JSON.stringify(value));
const state = { type: 'game_state', room: 'ABC234', status: 'active', phase: 'play', currentPlayer: '1', players: ['1', '2', '3'], protected: ['3'], discards: {}, favors: { 1: 0, 2: 0, 3: 0 }, cardsRemaining: 17 };
assert.deepEqual(plain(api.legalTargets(state, '1', 1)), ['2']);
assert.deepEqual(plain(api.legalTargets(state, '1', 5)), ['1', '2']);
assert.deepEqual(plain(api.legalTargets({ ...state, protected: ['2', '3'] }, '1', 2)), []);
assert.deepEqual(plain(api.legalTargets({ ...state, protected: ['1', '2', '3'] }, '1', 5)), ['1']);
assert.deepEqual(plain(api.legalTargets(state, '1', 4)), []);
assert.equal(api.forcedChoice({ held: 8, drawn: 5 }), 'held');
assert.equal(api.forcedChoice({ held: 7, drawn: 8 }), 'drawn');
assert.equal(api.forcedChoice({ held: 8, drawn: 9 }), null);

const model = api.newModel();
api.receive(model, { type: 'welcome', clientId: '1' });
api.receive(model, { type: 'room_joined', room: state.room });
api.receive(model, { type: 'hand', room: state.room, held: 0, drawn: null });
api.receive(model, { type: 'hand', room: state.room, held: 0, drawn: 6 });
api.receive(model, state);
assert.deepEqual(plain(model.hand), { held: 0, drawn: 6 });
assert.equal(model.clientId, '1');
api.receive(model, { type: 'chancellor_choice', room: state.room, cards: [1, 1, 9] });
api.receive(model, { ...state, phase: 'chancellor' });
api.chooseKeep(model, 1);
model.bottom.reverse();
assert.equal(model.keep, 1);
assert.deepEqual(plain(model.bottom), [2, 0]);
model.pending = { type: 'server_message' };
api.receive(model, { type: 'error', message: 'Try again.' });
assert.equal(model.pending, null);
assert.deepEqual(plain(model.bottom), [2, 0]);
api.receive(model, { ...state, currentPlayer: '2' });
assert.deepEqual(plain(model.candidates), []);

const event = (type, fields = {}) => api.receive(model, { type, room: model.room, ...fields });
event('card_played', { player: '2', card: 5, target: '1' });
event('card_discarded', { player: '2', card: 5, reason: 'played' });
event('card_discarded', { player: '1', card: 9, reason: 'prince' });
event('player_eliminated', { player: '1', reason: 'princess' });
assert.deepEqual(plain(model.history), [
  'Player 2 played Prince (5) → You. You discarded Princess (9). You was eliminated.'
]);
api.receive(model, { ...state, currentPlayer: '3' });
event('card_played', { player: '3', card: 6 });
event('card_discarded', { player: '3', card: 6, reason: 'played' });
api.receive(model, { ...state, currentPlayer: '3', phase: 'chancellor' });
event('chancellor_choice', { cards: [1, 2, 3] });
event('error', { message: 'Try again.' });
event('hand', { held: 1, drawn: null });
api.receive(model, { ...state, currentPlayer: '2' });
assert.equal(model.history.length, 2); // Chancellor resolution adds no second entry.
event('card_discarded', { player: '2', card: 0, reason: 'left' });
event('card_discarded', { player: '2', card: 4, reason: 'left' });
event('player_eliminated', { player: '2', reason: 'left' });
assert.equal(model.history.length, 3);
assert.equal(model.history[0], 'Player 2 discarded Spy (0). Player 2 discarded Handmaid (4). Player 2 left the round.');
assert.equal(model.history[1], 'Player 3 played Chancellor (6).');
api.receive(model, state);
for (let i = 0; i < 31; i++) {
  event('card_played', { player: '2', card: 0 });
  api.receive(model, state);
}
assert.equal(model.history.length, 30);
event('hand', { held: 0, drawn: 6 });

const publicHistory = plain(model.history);
api.receive(model, { type: 'cards_revealed', room: state.room, card: 2, hands: { 2: 9 } });
assert.deepEqual(plain(model.history), publicHistory); // Latest action must never contain private reveals.
model.pending = { type: 'join_room', room: 'DEF234' };
api.receive(model, { type: 'error', message: 'Room not found.' });
assert.equal(model.room, state.room);
assert.equal(model.hand.held, 0);
assert.equal(model.reveal.hands['2'], 9);
model.pending = { type: 'join_room', room: 'DEF234' };
api.receive(model, { ...state, room: 'DEF234' }); // State precedes join confirmation.
assert.equal(model.room, state.room);
api.receive(model, { type: 'room_joined', room: 'DEF234' });
assert.equal(model.game.room, 'DEF234');
assert.equal(model.hand.held, null);
assert.equal(model.reveal, null);
assert.deepEqual(plain(model.history), []);
api.receive(model, { type: 'hand', room: state.room, held: 9, drawn: 8 });
assert.equal(model.hand.held, null); // Ignore old-room secrets.

api.receive(model, { ...state, room: model.room, status: 'finished', currentPlayer: null });
api.receive(model, { type: 'game_over', room: model.room, winners: ['2'], hands: { 2: 9 } });
api.receive(model, { type: 'hand', room: model.room, held: 4, drawn: null });
api.receive(model, { type: 'hand', room: model.room, held: 4, drawn: 8 });
api.receive(model, { ...state, room: model.room });
assert.deepEqual(plain(model.hand), { held: 4, drawn: 8 });
assert.equal(model.result, null);
assert.deepEqual(plain(model.history), []);
api.receive(model, { type: 'room_joined', room: 'XYZ234' });
assert.equal(model.game, null);
assert.equal(model.hand.drawn, null);

// Name updates cannot alter a turn, an in-flight request, or historical text.
const named = api.newModel();
api.receive(named, { type: 'welcome', clientId: '1' });
api.receive(named, { type: 'room_joined', room: state.room, name: 'Alice' });
api.receive(named, state);
named.choice = 'held';
named.candidates = [6, 4, 3];
api.chooseKeep(named, 1);
named.pending = { type: 'server_message', data: { action: 'resolve_chancellor' } };
const beforeName = plain(named);
api.receive(named, { type: 'player_names', room: state.room, names: { 1: 'Alice', 2: '王😀' } });
assert.deepEqual(plain({ ...named, names: {} }), beforeName);
assert.equal(api.playerName('2', '1', named.names), '王😀');
assert.equal(api.playerName('1', '1', named.names), 'You');
assert.equal(api.playerName('3', '1', named.names), 'Player 3');
assert.equal(api.playerName('2', '1', { 1: 'Alice', 2: 'Alice' }), 'Alice (Player 2)');
api.receive(named, { type: 'card_played', room: state.room, player: '2', card: 4 });
const oldHistory = plain(named.history);
api.receive(named, { type: 'player_names', room: state.room, names: { 1: 'Alice', 2: '<b>Bob</b>' } });
assert.deepEqual(plain(named.history), oldHistory);
assert.equal(api.playerName('2', '1', named.names), '<b>Bob</b>');
api.receive(named, { type: 'player_names', room: 'OTHER2', names: {} });
assert.equal(named.names['2'], '<b>Bob</b>');
api.receive(named, { type: 'name_updated', name: 'New Alice' });
assert.deepEqual(plain(named.pending), beforeName.pending);
named.pending = { type: 'set_name' };
api.receive(named, { type: 'name_updated', name: 'New Alice' });
assert.equal(named.pending, null);
api.receive(named, { type: 'room_joined', room: 'OTHER2', name: 'New Alice' });
assert.deepEqual(plain(named.names), {});
assert.equal(named.name, 'New Alice');
assert.equal(api.newModel().name, '');

console.log('PASS: frontend card choices, event ordering, privacy, custom names, and rename state preservation.');
