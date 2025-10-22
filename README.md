# KUKSA Fixture Runner

Hardware fixture simulator for KUKSA databroker. Simulates actuator responses using VssDAG.

## What It Does

Claims ownership of actuators and simulates their physical behavior (delays, cross-effects, state).

**Example:** Door lock actuator takes 200ms to engage.

## Usage

```bash
./fixture-runner --kuksa localhost:55555 --config fixture.yaml
```

**Example fixture.yaml:**
```yaml
fixture:
  name: "Door Lock Fixture"
  serves:
    - "Vehicle.Cabin.Door.Row1.Left.IsLocked"
  mappings:
    - signal: "Vehicle.Cabin.Door.Row1.Left.IsLocked"
      depends_on: ["Vehicle.Cabin.Door.Row1.Left.IsLocked"]
      datatype: "boolean"
      transform:
        code: "delayed(deps['Vehicle.Cabin.Door.Row1.Left.IsLocked'], 200)"
```

See [FIXTURE_GUIDE.md](FIXTURE_GUIDE.md) for details.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Requirements

- libvssdag
- libkuksa-cpp
- glog, yaml-cpp

## License

Apache License 2.0
