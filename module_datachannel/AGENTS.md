# DataChannel Module

Cross-process DataChannel test module, independent of `module_calculator`.

## Architecture

```
Alpha (producer)                         Beta (consumer)
┌─────────────────────┐                 ┌─────────────────────┐
│ DCProducerModule    │                 │ DCConsumerModule    │
│  TCP: send(data)    │ ──TCP data──→   │  TCP: recv(data)    │
│  ZMQ: DATA_READY    │ ──Message──→    │  handleState()      │
│                     │   via MSMGatewy │                     │
│ MSMGateway(REP+PUB) │ ←─────────────→ │ MSMGateway(REP+PUB) │
└─────────────────────┘                 └─────────────────────┘
```

## Build & Run

```bash
cd module_datachannel
cmake -B build
cmake --build build

# Terminal 1: Beta (consumer) *first*
./build/dc_beta -f dc_beta.json

# Terminal 2: Alpha (producer)
./build/dc_alpha -f dc_alpha.json
```

Config via `-f <json>`; JSON has `dc_producer`/`dc_consumer` + `gateway` sections.

## Data Channel

- `DataChannel` class: POSIX TCP wrapper, producer=server, consumer=clientsize
- Single TCP connection, no length prefix (DATA_READY carries byte_count)
- Alloc-free on data path (pre-allocated buffer)
