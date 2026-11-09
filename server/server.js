'use strict';

// MAX-on-3DS relay server
// Line-based text protocol over TCP, justified by the small 3DS client.
//
//   Client -> Server:
//     NICK <name>          set/update nickname and join a room
//     JOIN <room>          join (or create) a room
//     MSG <text>           broadcast message to the current room
//     WHO                  list users in the current room
//     HELP                 show commands
//     QUIT                 disconnect
//
//   Server -> Client (one line each, # = control, M = message):
//     #HELLO <name> <room> <playerCount>/<roomCapacity>
//     #ROOM <room>
//     #JOIN <name>
//     #PART <name>
//     #WHO <name>[;<name>...]
//     #ERR <text>
//     M <name>: <text>      (one line, newlines in text are replaced by ' ')
//
// Protocol constants shared with the 3DS client:
const CMD_HELLO = '#HELLO'; // greeting with room info
const CMD_ROOM  = '#ROOM ';  // you changed room
const CMD_JOIN  = '#JOIN ';  // a user joined your room
const CMD_PART  = '#PART ';  // a user left your room
const CMD_WHO   = '#WHO ';   // user list, ';'-separated
const CMD_ERR   = '#ERR ';   // error to show
const MSG_PREFIX = 'M ';     // chat message

const ROOM_CAPACITY = 3;

const net = require('net');

const rooms = new Map(); // roomName -> Set<socket>
const info = new Map();  // socket -> { name, room }

function send(sock, line) {
  sock.write(line + '\r\n');
}

function nameOf(sock) {
  const i = info.get(sock);
  return i ? i.name : '?';
}

function roomMembers(roomName) {
  const s = rooms.get(roomName);
  return s ? [...s] : [];
}

function broadcast(roomName, line, except) {
  for (const s of roomMembers(roomName)) {
    if (s !== except) send(s, line);
  }
}

function sanitizeName(n) {
  return String(n).replace(/[^A-Za-z0-9_\u0400-\u04FF]/g, '').slice(0, 16);
}

function joinRoom(sock, roomName) {
  const i = info.get(sock);
  if (!i) return;

  if (i.room && i.room === roomName) {
    send(sock, CMD_ROOM + roomName);
    return;
  }

  const target = rooms.get(roomName);
  if (target && target.size >= ROOM_CAPACITY) {
    send(sock, CMD_ERR + 'room is full (' + ROOM_CAPACITY + ')');
    return;
  }

  // leave old room
  if (i.room && rooms.has(i.room)) {
    broadcast(i.room, CMD_PART + i.name, sock);
    rooms.get(i.room).delete(sock);
  }

  i.room = roomName;
  if (!rooms.has(roomName)) rooms.set(roomName, new Set());
  rooms.get(roomName).add(sock);

  const count = rooms.get(roomName).size;
  send(sock, CMD_HELLO + ' ' + i.name + ' ' + roomName + ' ' + count + '/' + ROOM_CAPACITY);
  broadcast(roomName, CMD_JOIN + i.name, sock);
}

const server = net.createServer((sock) => {
  sock.setEncoding('utf8');
  sock.setNoDelay(true);
  info.set(sock, { name: 'guest', room: null });

  sock.on('data', (data) => {
    const i = info.get(sock);
    for (const raw of data.split(/\r?\n/)) {
      const line = raw.trim();
      if (!line) continue;

      if (line.startsWith('NICK ')) {
        i.name = sanitizeName(line.slice(5));
        if (!i.name) i.name = 'guest';
        send(sock, '#NAME ' + i.name);
      } else if (line.startsWith('JOIN ')) {
        const room = sanitizeName(line.slice(5)).toLowerCase();
        if (room) joinRoom(sock, room);
      } else if (line === 'WHO') {
        if (!i.room) { send(sock, CMD_ERR + 'not in a room'); continue; }
        send(sock, CMD_WHO + roomMembers(i.room).map(nameOf).join(';'));
      } else if (line === 'HELP') {
        send(sock, 'M Сервер: NICK <имя>, JOIN <комната>, MSG <текст>, WHO, HELP');
      } else if (line.startsWith('MSG ')) {
        if (!i.room) { send(sock, CMD_ERR + 'join a room first'); continue; }
        const text = line.slice(4).replace(/[^\x20-\x7E\u0400-\u04FF]/g, '').slice(0, 400);
        if (text) broadcast(i.room, MSG_PREFIX + i.name + ': ' + text, sock);
      } else if (line === 'QUIT') {
        sock.end();
      }
    }
  });

  sock.on('close', () => {
    const i = info.get(sock);
    if (i && i.room && rooms.has(i.room)) {
      broadcast(i.room, CMD_PART + i.name, sock);
      rooms.get(i.room).delete(sock);
      if (rooms.get(i.room).size === 0) rooms.delete(i.room);
    }
    info.delete(sock);
  });

  sock.on('error', () => {}); // ignore ECONNRESET etc on 3DS
});

const PORT = Number(process.env.PORT || 26662);
server.listen(PORT, () => {
  console.log('MAX-3DS relay listening on :' + PORT);
});