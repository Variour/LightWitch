export async function onRequest(ctx) {
  if (ctx.request.headers.get('Upgrade') !== 'websocket') {
    return new Response('Expected WebSocket upgrade', { status: 426 });
  }

  const pair = new WebSocketPair();
  const [client, server] = Object.values(pair);
  server.accept();

  const send = data => server.send(JSON.stringify(data));

  send({ t: 'log', l: 'I', m: 'Mock server connected' });
  send({ t: 'log', l: 'I', m: 'This is a development mock — no hardware attached' });
  send({
    t: 'peers',
    self: { name: 'Mock Device', mac: '11:22:33:44:55:66', groupId: 0, online: true },
    peers: [{ name: 'Mock Light 2', mac: '22:33:44:55:66:77', groupId: 0, online: true, rssi: -65 }],
  });
  send({
    t: 'groups',
    list: [{ id: 0, name: 'Default', mode: 0, pattern: 0, r: 255, g: 200, b: 80, brightness: 200, speed: 1, syncEnabled: true }],
  });

  server.addEventListener('message', () => {});
  server.addEventListener('close', () => {});

  return new Response(null, { status: 101, webSocket: client });
}
