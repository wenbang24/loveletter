import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Exercise the actual inline model without a browser or a second implementation.
const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const source = html.match(/<script id="game-logic">([\s\S]*?)<\/script>/)[1];
const api = vm.runInNewContext(`${source}\n({ newModel, receive, legalTargets, forcedChoice, chooseKeep })`);
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
  'Player 2 played 5 · Prince → You. You discarded 9 · Princess. You was eliminated.'
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
assert.equal(model.history[0], 'Player 2 discarded 0 · Spy. Player 2 discarded 4 · Handmaid. Player 2 left the round.');
assert.equal(model.history[1], 'Player 3 played 6 · Chancellor.');
api.receive(model, state);
for (let i = 0; i < 31; i++) {
  event('card_played', { player: '2', card: 0 });
  api.receive(model, state);
}
assert.equal(model.history.length, 30);
event('hand', { held: 0, drawn: 6 });

api.receive(model, { type: 'cards_revealed', room: state.room, card: 2, hands: { 2: 9 } });
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
console.log('PASS: frontend targets, forced Countess, Chancellor indices, event ordering, retries, round resets, and room privacy.');
