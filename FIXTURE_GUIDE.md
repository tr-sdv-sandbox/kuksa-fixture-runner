# Hardware Fixture Runner - Configuration Guide

Simulates hardware responses to actuator commands using VssDAG.

## Basic Structure

```yaml
fixture:
  name: "Door Lock Fixture"

  # Actuators this fixture claims ownership of
  serves:
    - "Vehicle.Cabin.Door.Row1.Left.IsLocked"

  # How actuators respond
  mappings:
    - signal: "Vehicle.Cabin.Door.Row1.Left.IsLocked"
      depends_on: ["Vehicle.Cabin.Door.Row1.Left.IsLocked"]
      datatype: "boolean"
      transform:
        code: "delayed(deps['Vehicle.Cabin.Door.Row1.Left.IsLocked'], 200)"
```

**How it works:**
1. Databroker sends command: `IsLocked.target = true`
2. Fixture processes through DAG with 200ms delay
3. After 200ms, fixture publishes: `IsLocked` (actual) = `true`

## Mapping Fields

- `signal`: Output signal path
- `depends_on`: Dependency signals
- `datatype`: `boolean`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `float`, `double`
- `transform.code`: Lua code to compute output

## Built-in Functions

### `delayed(value, delay_ms)`
Returns `nil` while waiting, then returns value after delay.

### `get_state()`
Persistent state for this signal.

### Filters
- `lowpass(x, alpha)` - Exponential moving average
- `moving_average(x, name, window)` - Sliding window
- `derivative(x, name)` - Rate of change
- `sustained_condition(condition, duration)` - Debounce

### Context
- `deps['SignalName']` - Dependency value (nil if invalid)
- `status['SignalName']` - `STATUS_VALID`, `STATUS_INVALID`, `STATUS_NOT_AVAILABLE`
- `_current_time` - Current time (seconds)

## Running

```bash
./fixture-runner --kuksa localhost:55555 --config fixture.yaml
```
