# Mesh routing for the game session

`src/mesh/MeshRoute.h` carries the game session's radio frames over several
hops, sets the transmit power per device and runs the mesh test.
It is plain C++ (no Arduino); `main.cpp` wires it to ESP-NOW through
`MeshManager`. It runs in every build with `FEATURE_GAME=1`.

## Tree

- Every device sends a beacon (`MeshBeacon`, 29 bytes) to its neighbours:
  root, hops and path cost to the root, its parent, the transmit power of this
  very beacon, number of children, battery, the mesh test state.
- Period: trickle from 0.5 s, doubling to 4 s while nothing changes; back to
  0.5 s when the device's route changes, it takes over a new test state or a
  neighbour without route shows up. 1 s during the mesh test, 4 s while a
  round is open.
- Root: the device whose USB link to the console is open (`link hello`),
  or the device that started the mesh test, for the test's duration.
- Parent: the neighbour with the cheapest path. Link cost in 1/16 ETX from the
  path loss as if sent at full power (1 down to -65 dBm, 2 at -80 dBm, 6 at
  -92 dBm), divided by the share of its beacons received; +2 ETX for a
  neighbour below 15 % battery off USB, +4 ETX for 5 s after 3 unanswered
  unicasts. A new parent must be 0.75 ETX cheaper, at most one change per 2 s.
- Never a parent: a device flagged mobile (teacher device), a device whose
  parent is this one, a route of 6 hops or more.
- A route ends when the parent is not heard for 3 beacon periods (at least
  3 s), when session frames flow but the parent stayed silent for 2.5 s, or
  when the root's beacon counter did not move for 20 s.

## Forwarding

- Quiz frames with `origin` 0 (bridge to devices) go out as a broadcast.
  A device with children sends each new one on once, `hops` + 1, and adds the
  time the frame spent on the device to `openOffsetMs` of `QuizState`.
  The game gets these frames in `msgId` order only: a frame not newer than
  the last one is dropped, a step back of 256 or more or 3 s without a frame
  counts as a restarted bridge.
- Quiz frames with another `origin` (devices to the bridge) and topology
  reports go to the parent as an ESP-NOW unicast (acknowledged and repeated by
  the radio). Without a parent, or when the unicast stays unanswered, the
  frame goes out as a broadcast; a parent also sends on broadcasts of its
  children. Copies are recognised by a hash over the frame without the hop
  counters and dropped for 4 s.
- Fleet frames and LightWitch messages are not forwarded.

## Transmit power

- Wanted level at the far end: -67 dBm. A device sends with
  `path loss + (-67)` dB for the weakest of its parent and children, between
  2 dBm and the build's `WIFI_TX_POWER`; +6 dB for 15 s after an unanswered
  unicast. Up at once, down by 1 dB per second.
- Full power: outside a session (also whenever WiFi is connected), without a
  route, for 5 s after hearing a neighbour without route, on the root while
  the session's login is open and on a root without children, and in the
  whole mesh after `mesh power max`.

## Radio discipline

- While a round is open (seen in the `QuizState` passing through): beacons
  every 4 s, no topology reports, no trickle resets for lost neighbours.
- Topology reports every 30 s outside the test, only from devices with a route.

## Mesh test

Started from the hub's Funk page, or with `mesh test` on any device's USB
link (docs/usb-link.md). The starting device becomes the root, sets the test
in its beacons, every device in reach takes it over (the newer nonce wins)
and passes it on. The test ends after its time (default 300 s) or with `off`.

On a Buzzerboard the test takes over all LEDs (`MeshTestRender.h`):

| Zone | Shows |
|---|---|
| Ring | place in the tree: white root, green 1 hop, cyan 2, blue 3, violet 4, magenta 5+; red blinking = no route |
| Column | level of the parent's beacons as a bar from red (-95 dBm) to green (-40 dBm); white dot = own transmit power (bottom 2 dBm, top full power). Root: one LED per child. Without route: one red LED per neighbour heard |
| Key LEDs Up, Right, Down, Left | number of children |
| Status dot | white flash: own frame sent; hop colour flash: frame from the parent |
| Column comet | a frame passed on: downward toward the children, upward toward the root |
| Ring flash | the root's trace wave (every 3 s, slow motion every 6 s) |

Slow motion (`slow`): every relay holds the trace and topology reports for
0.8 s per hop, so the wave walks visibly through the room. Game frames are
never held.

During the test every device reports every 2 s (slow motion 4 s); the root
keeps the table for the hub display and `mesh nodes`.
