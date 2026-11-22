# Torches

LARP torches that light each other. A torch is a normal light with the
**Torch** flag set (light settings in the web UI, `torch` in
`/api/lights/add` and `/api/lights/update`).

## Behaviour

- A torch is either lit or out. It starts out at boot; the state is not saved.
- Out, the light is dark. Lit, it shows its group's look, so give the
  group the look of a flame (for example mode Pattern with Candle or Fire).
- The actions **Torch light** (`TorchIgnite`, 30) and **Torch put out**
  (`TorchExtinguish`, 31) switch a torch light. Assign them to keys like any
  other action, for example long press to light and short press to put out.
  Automations can use them too.
- Every device with a lit torch broadcasts a `TorchFlame` frame every
  200 ms with the groups of its lit torches.
- An out torch catches fire when a lit torch of the same group stays close
  for 1.5 s: its frames arrive at -45 dBm or stronger. A single weak or
  lost frame does not restart the wait; 600 ms without a close frame does.
  A torch put out next to a lit one waits the full 1.5 s again.

The values are constants in `src/torch/TorchSpread.h`
(`IGNITE_RSSI_DBM`, `IGNITE_HOLD_MS`, `BEACON_INTERVAL_MS`, `CLOSE_GAP_MS`).

## Setup

1. Add the strip as a light and tick **Torch**.
2. Put every torch that may light the others into the same group and give
   the group a flame look. Group sync shares the look, not the lit state.
3. Assign **Torch light** and **Torch put out** to the device's key.
