# Mesh protocol compatibility policy

This document records the current compatibility contract for ESP-NOW mesh messages in `src/mesh/MeshTypes.h` / `src/mesh/MeshManager.h`.

## Policy statement

LightWitch currently treats the mesh wire protocol as a **same-firmware protocol**.

What is supported:
- peers running the same firmware build
- normal packet loss / reordering within that build's existing message handling

What is **not** supported:
- mixed-firmware meshes across different revisions
- rolling upgrades where different mesh schemas are expected to interoperate
- additive struct changes that assume older firmware will safely ignore new fields

Right now that is acceptable because there is no released/deployed version that needs compatibility preserved.

## Why the current policy is this strict

Current mesh messages are sent as raw native C++ struct layouts from `MeshTypes.h`. With the exception of `PresenceMsg.version`, they do not have a schema version or an explicit serialization layer.

That means compatibility depends on details such as:
- field order
- struct padding / ABI layout
- enum sizes
- sender and receiver using the same interpretation of nested structs

That is acceptable for a same-firmware mesh, but it is not a durable compatibility story for staggered upgrades.

If the project ever wants mixed-firmware compatibility after a real release, the fix is not to keep tweaking `PRESENCE_MSG_VERSION` alone. The protocol would need explicit framing/serialization rules and a per-message evolution strategy.

## Presence message decision

`PresenceMsg` is the only mesh payload with an explicit version field.

Current rule:
- receiver requires `len == sizeof(PresenceMsg)`
- receiver requires `m->version == PRESENCE_MSG_VERSION`

Decision:
- **reset `PRESENCE_MSG_VERSION` to `1`** before first real deployment
- **do not keep compatibility code for older presence schemas right now**
- **shorter / older presence frames are intentionally rejected**
- **adding tail fields to `PresenceMsg` still counts as an incompatible change and requires a version bump once releases/deployments matter**

So the version field does **not** mean "best-effort backward compatibility". It is a hard schema gate for the current heartbeat frame.

## Current message inventory

All current `MsgType` payloads were audited against `MeshManager::_onRecv`.

| MsgType | Payload / framing | Current parser rule | Compatibility notes |
|---|---|---|---|
| `Presence` | `PresenceMsg` fixed-size | exact `sizeof(PresenceMsg)` and exact `PRESENCE_MSG_VERSION` | Only explicitly versioned payload. Older/newer layouts are rejected. |
| `LightConfig` | `LightConfigMsg` fixed-size | `len >= sizeof(LightConfigMsg)` | Raw nested `LightConfig` layout; no versioning. |
| `SetGroup` | `SetGroupMsg` fixed-size | `len >= sizeof(SetGroupMsg)` | Raw struct; target filtering happens after parse. |
| `GroupSync` | `GroupSyncMsg` fixed-size | `len >= sizeof(GroupSyncMsg)` | Raw nested `GroupConfig` layout; no versioning. |
| `PhaseSync` | `PhaseSyncMsg` fixed-size | `len >= sizeof(PhaseSyncMsg)` | Raw struct; no versioning. |
| `ProximityPing` | `ProximityPingMsg` fixed-size | exact `sizeof(ProximityPingMsg)` | Receiver ignores payload after framing check. |
| `SceneManifest` | `SceneManifestMsg` variable-length (`count` entries) | header must be present, `count <= MANIFEST_ENTRIES_PER_MSG`, and buffer must contain all advertised entries | Explicit count framing, but still no schema version. |
| `SceneRequest` | `SceneRequestMsg` fixed-size | `len >= sizeof(SceneRequestMsg)` | Raw fixed request with scene id. |
| `SceneChunk` | `SceneChunkMsg` variable-length (`dataLen` bytes used) | header must be present, `dataLen <= CHUNK_DATA_SIZE`, and buffer must contain all advertised bytes | Sender currently transmits full struct size, but receiver validates actual advertised payload length before handing off. |
| `SceneForceSet` | `SceneForceSetMsg` fixed-size | `len >= sizeof(SceneForceSetMsg)` | Raw fixed struct. |
| `SetSceneSync` | `SetSceneSyncMsg` fixed-size | `len >= sizeof(SetSceneSyncMsg)` | Raw fixed struct; target filtering happens after parse. |
| `ConfigChunk` | `ConfigChunkMsg` variable-length (`dataLen` bytes used) | header must be present, `dataLen <= CONFIG_CHUNK_DATA_SIZE`, and buffer must contain all advertised bytes | Sender currently transmits full struct size, but receiver validates actual advertised payload length before handing off. |
| `TriggerUpdate` | `TriggerUpdateMsg` fixed-size | `len >= sizeof(TriggerUpdateMsg)` | Raw fixed struct; no broader mixed-version guarantee is documented yet. |
| `CheckUpdate` | `CheckUpdateMsg` fixed-size | `len >= sizeof(CheckUpdateMsg)` | Raw fixed struct; no broader mixed-version guarantee is documented yet. |
| `SceneEditPush` | `SceneEditPushMsg` fixed-size | `len >= sizeof(SceneEditPushMsg)` | Raw fixed struct. |
| `RequestManifest` | `RequestManifestMsg` fixed-size | exact `sizeof(RequestManifestMsg)` | No payload beyond message type. |
| `TimeSync` | `TimeSyncMsg` fixed-size | `len >= sizeof(TimeSyncMsg)` | Raw fixed struct. |
| `KeyExchangeInit` | `KeyExchangeInitMsg` fixed-size | `len >= sizeof(KeyExchangeInitMsg)` | Raw fixed struct; target filtering happens after parse. |
| `KeyExchangeResp` | `KeyExchangeRespMsg` fixed-size | `len >= sizeof(KeyExchangeRespMsg)` | Raw fixed struct; target filtering happens after parse. |
| `MeshPolicy` | `MeshPolicyMsg` fixed-size | `len >= sizeof(MeshPolicyMsg)` | Raw fixed struct representing replicated state. |
| `WifiRetry` | `WifiRetryMsg` fixed-size | exact `sizeof(WifiRetryMsg)` | No payload beyond message type. |
| `MeshSearch` | `MeshSearchMsg` fixed-size | exact `sizeof(MeshSearchMsg)` | No payload beyond message type. |
| `AudioGroupSync` | `AudioGroupSyncMsg` fixed-size | `len >= sizeof(AudioGroupSyncMsg)` | Raw nested `AudioGroupConfig` layout; no versioning. Mirrors `GroupSync`. |
| `SetPlaylistSync` | `SetPlaylistSyncMsg` fixed-size | `len >= sizeof(SetPlaylistSyncMsg)` | Raw fixed struct; target filtering happens after parse. Mirrors `SetSceneSync`. |
| `PlaylistManifest` | `PlaylistManifestMsg` variable-length (`count` entries) | header must be present, `count <= PLAYLIST_MANIFEST_ENTRIES_PER_MSG`, and buffer must contain all advertised entries | Mirrors `SceneManifest`. |
| `PlaylistRequest` | `PlaylistRequestMsg` fixed-size | `len >= sizeof(PlaylistRequestMsg)` | Mirrors `SceneRequest`. |
| `PlaylistChunk` | `PlaylistChunkMsg` variable-length (`dataLen` bytes used) | header must be present, `dataLen <= PLAYLIST_CHUNK_DATA_SIZE`, and buffer must contain all advertised bytes | Mirrors `SceneChunk`. Playlist metadata only — the audio files a playlist references are never sent this way. |
| `PlaylistForceSet` | `PlaylistForceSetMsg` fixed-size | `len >= sizeof(PlaylistForceSetMsg)` | Mirrors `SceneForceSet`. |
| `PlaylistEditPush` | `PlaylistEditPushMsg` fixed-size | `len >= sizeof(PlaylistEditPushMsg)` | Mirrors `SceneEditPush`. |
| `RequestPlaylistManifest` | `RequestPlaylistManifestMsg` fixed-size | exact `sizeof(RequestPlaylistManifestMsg)` | No payload beyond message type. Mirrors `RequestManifest`. |
| `PlayAudio` | `PlayAudioMsg` fixed-size | `len >= sizeof(PlayAudioMsg)` | Raw fixed struct. One-shot playback trigger — see `PlayAudioMsg`'s comment for the start-sync/participation contract. |
| `StopAudio` | `StopAudioMsg` fixed-size | `len >= sizeof(StopAudioMsg)` | Raw fixed struct. |
| `GenericEvent` | `GenericEventMsg` fixed-size | `len >= sizeof(GenericEventMsg)` | Raw fixed struct. `eventType` is opaque to the mesh layer; no versioning. |
| `Hello` | `HelloMsg` fixed-size | exact `sizeof(HelloMsg)` | **The one deliberate cross-firmware exception** — see below. |
| `QuizInvite` | `QuizInviteMsg` fixed-size, 16 bytes | exact `sizeof(QuizInviteMsg)` | Game session, see below. Bridge to all, also on foreign channels while login is open. |
| `QuizState` | `QuizStateMsg` variable-length (41-byte head + `echoCount` echo bytes) | head must be present, `echoCount <= 200`, and `len` must equal head + `echoCount` | Game session. Bridge to all; the echo is paged by `echoFirst`/`echoCount`. |
| `QuizReport` | `QuizReportMsg` fixed-size, 24 bytes | exact `sizeof(QuizReportMsg)` | Game session. Device to bridge. |
| `QuizStatus` | `QuizStatusMsg` fixed-size, 31 bytes | exact `sizeof(QuizStatusMsg)` | Game session. Device to bridge; with flag bit 1 it is a join request. |
| `QuizAdmit` | `QuizAdmitMsg` fixed-size, 22 bytes | exact `sizeof(QuizAdmitMsg)` | Game session. Bridge to one device, target filtering after parse. |
| `QuizRelease` | `QuizReleaseMsg` fixed-size, 20 bytes | exact `sizeof(QuizReleaseMsg)` | Game session. Bridge to one device or all (`ff:ff:ff:ff:ff:ff`). |
| `QuizChannel` | `QuizChannelMsg` fixed-size, 17 bytes | exact `sizeof(QuizChannelMsg)` | Game session. Bridge to all, announces a channel change. |

## The `Hello` exception

Every message above is same-firmware-only, by design. `HelloMsg` is the single deliberate exception, and it exists to resolve a real conflict: onboarding a brand-new device requires it to be discoverable, receive a WiFi-config push, and be nudged to check/install a firmware update — all before it's guaranteed to run firmware compatible with `PresenceMsg`. Without a discovery path that survives a version mismatch, a new device on different firmware can never be brought up to date at all.

This does not reopen the "no compatibility code" rule for the rest of the protocol:

- `HelloMsg` carries only `name` + `fwVersion` + `wifiConnected` + `hasWifiNetworks` — enough to show a device in the dashboard, target it by MAC, and tell whether a WiFi-config push or an update check is the right next step. It never carries state that needs merging or interpreting.
- It has **no version field**. Its shape may still move during this feature's own development (mirrors `PresenceMsg`'s "reset before first real deployment" convention — see above), but once a released firmware is broadcasting a given layout, it must **never change again** — no new fields, ever, from that point on. If onboarding needs more information after that, that's a new `MsgType`, not a change to this struct. That's what keeps its exact-size check safe permanently, instead of becoming another `PRESENCE_MSG_VERSION`-style gate that itself needs versioning.
- Every device broadcasts it unconditionally (same 5 s cadence as `PresenceMsg`), regardless of onboarding state — so discovery is symmetric and doesn't depend on either side knowing it's "new".
- Receiving a `Hello` also counts as "peer heard" for `ChannelManager`'s channel-lock logic, the same as a valid `PresenceMsg` did before — so channel convergence no longer depends on the two devices' `PresenceMsg` schemas matching either.
- Once a device starts sending a `PresenceMsg` this receiver accepts, it's treated as a full peer; `Hello` only ever represents "seen, but not yet interoperable with the full protocol."

## Game session frames (`Quiz*`)

The `Quiz*` types belong to the game session module (`src/game/`). Their layouts live in `src/game/GameFrames.h`, not in `MeshTypes.h`, so the module's protocol code builds without Arduino; `MeshTypes.h` mirrors the type numbers and `static_assert`s that both agree.

- Packed, little-endian, every frame at most 250 bytes (ESP-NOW v1). Each frame starts with a 12-byte header (`type`, `proto`, `fleet`, `origin`, `msgId`, `ttl`, `hops`, `dist`, `flags`) and the session id.
- `proto` (1 in v0) is checked by the game module, not by the mesh layer. The mesh layer only checks framing, sends every framed quiz frame to `MeshManager::setOnQuizFrame` with its RSSI, and `MeshManager::broadcastQuizFrame` applies the same check before sending.
- Devices outside a session handle only `QuizInvite`, and only when they have a buzz key.

### Quiet mode and the Hello rule

While a device is in a game session (participant from join until release, bridge from its first session until restart), `MeshManager::setQuiet(true)` makes it send and handle only `Quiz*` frames. It sends no `Presence`, no `Hello`, no `MeshPolicy`, `GroupSync`, `AudioGroupSync`, `PhaseSync` or proximity pings, and it ignores every other received type. This is a deliberate exception to the unconditional `Hello` cadence above, for the duration of a session only. A participant leaves a session by restarting into normal operation.

## Audit conclusions

1. `PresenceMsg` versioning is a hard reject gate, not a compatibility layer.
2. Most other mesh messages still rely on raw `sizeof(...)` parsing with no explicit versioning.
3. Under the current same-firmware-only policy, that is acceptable for now.
4. The unsafe part found in the review was malformed/truncated variable-length packets, not just version numbering.

This review therefore tightened receive-side framing for:
- `SceneManifest`
- `SceneChunk`
- `ConfigChunk`
- zero-payload event frames (`RequestManifest`, `WifiRetry`)
- fixed-size `PresenceMsg` exact-size enforcement

These changes prevent callbacks from reading past the received buffer when a packet advertises more entries/data than were actually delivered.

## Release / rollout policy

Current rollout assumption:
- there is no released/deployed protocol to remain compatible with yet
- mesh compatibility is defined only for devices on the same current firmware
- if cross-version behavior becomes important after a real release, it must be designed explicitly and documented per message type

## Future rule for protocol changes

Until a real versioned serialization layer exists, use this rule:
- if a mesh message layout changes incompatibly, either
  - bump that message's explicit version field if it has one, or
  - create a new message schema / message type instead of silently reusing the old one
- do not treat "older firmware will probably ignore the extra bytes" as a supported compatibility mechanism

That keeps the current policy honest: same-firmware meshes are supported; cross-version compatibility is not promised unless it is explicitly designed later.
