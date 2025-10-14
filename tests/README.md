# Fixture Runner Integration Tests

Integration tests for the kuksa-fixture-runner that verify interaction with KUKSA databroker.

## Overview

These tests:
- Automatically start KUKSA databroker v0.6.0 in Docker
- Launch the actual `fixture-runner` binary as a subprocess
- Send actuation commands via KUKSA client
- Verify that fixture-runner processes actuations and publishes actual values
- Clean up all resources automatically

## Test Approach

Unlike unit tests, these are **true integration tests** that:
1. Run the actual compiled `fixture-runner` binary (not mocked code)
2. Communicate with a real KUKSA databroker instance
3. Use real gRPC connections
4. Test the complete feedback loop: command → fixture → actual value

This approach is similar to libkuksa-cpp's integration test strategy.

## Prerequisites

- Docker installed and running
- KUKSA databroker image available: `ghcr.io/eclipse-kuksa/kuksa-databroker:0.6.0`
- Built `fixture-runner` binary
- No KUKSA instance running on port 55556

## Building Tests

```bash
# Build with tests enabled (default)
cd kuksa-fixture-runner
mkdir build && cd build
cmake ..
make

# Or explicitly enable tests
cmake .. -DBUILD_FIXTURE_RUNNER_TESTS=ON
make

# Disable tests
cmake .. -DBUILD_FIXTURE_RUNNER_TESTS=OFF
make
```

## Running Tests

### Run all tests
```bash
cd build
./tests/integration/test_fixture_runner
```

### Run with CTest
```bash
cd build
ctest -R test_fixture_runner -V
```

### Run specific test
```bash
cd build
./tests/integration/test_fixture_runner --gtest_filter="*FixtureRunnerStartsAndRegisters*"
```

### List available tests
```bash
cd build
./tests/integration/test_fixture_runner --gtest_list_tests
```

## Test Cases

### 1. FixtureRunnerStartsAndRegisters
**Purpose**: Verify fixture-runner can start, connect, and register as actuator provider

**What it tests**:
- Configuration file loading
- KUKSA databroker connection
- Actuator registration
- Basic actuation command handling

### 2. FixtureRunnerPublishesActualValue
**Purpose**: Test complete feedback loop

**What it tests**:
- Actuation command reception
- Hardware delay simulation
- Actual value publishing
- Observer receives updates

**Flow**:
```
Commander → KUKSA → Fixture-Runner → (delay) → KUKSA → Observer
```

### 3. FixtureRunnerHandlesMultipleActuators
**Purpose**: Verify concurrent handling of multiple actuators

**What it tests**:
- Multiple actuator registration
- Independent processing per actuator
- No cross-contamination between fixtures

### 4. FixtureRunnerSurvivesRapidActuations
**Purpose**: Stress test with rapid actuation commands

**What it tests**:
- Queue handling under load
- No dropped commands
- Stable operation
- Proper worker thread synchronization

### 5. FixtureRunnerRespectsConfiguredDelay
**Purpose**: Verify delay configuration is honored

**What it tests**:
- Delay configuration parsing
- Accurate delay implementation
- Timing requirements (min/max bounds)

## Using External Databroker

To use an existing KUKSA instance instead of Docker:

```bash
export KUKSA_ADDRESS=localhost:55555
./tests/integration/test_fixture_runner
```

This skips Docker container management and connects to your databroker.

## Debugging Tests

### Enable verbose logging
```bash
# glog output
export GLOG_logtostderr=1
export GLOG_v=2
./tests/integration/test_fixture_runner
```

### Keep Docker container running
Modify `kuksa_test_fixture.hpp` to comment out the cleanup in `TearDownTestSuite()`:

```cpp
static void TearDownTestSuite() {
    // if (!use_external_databroker_) {
    //     system(("docker stop " + databroker_container_name_).c_str());
    //     system(("docker rm " + databroker_container_name_).c_str());
    // }
}
```

Then inspect the running container:
```bash
docker ps
docker logs kuksa-test-databroker
```

### Debug fixture-runner process
The tests launch fixture-runner as a subprocess. To debug:

1. Run test in one terminal to start databroker
2. In another terminal, manually run:
```bash
cd build
./fixture-runner --kuksa localhost:55556 --config /tmp/test_fixtures.json
```

3. Attach debugger to the manual instance

## Troubleshooting

### "Failed to start KUKSA databroker container"
- Check Docker is running: `docker ps`
- Pull image manually: `docker pull ghcr.io/eclipse-kuksa/kuksa-databroker:0.6.0`
- Check port 55556 is free: `lsof -i :55556`

### "Failed to exec fixture-runner"
- Verify binary exists: `ls -l build/fixture-runner`
- Check permissions: `chmod +x build/fixture-runner`
- Run manually to check for missing libraries: `ldd build/fixture-runner`

### Tests timeout
- Increase wait timeouts in test code
- Check databroker is responsive: `docker logs kuksa-test-databroker`
- Verify no firewall blocking localhost connections

### "Did not receive actual value update"
- Check fixture-runner logs (visible in test output)
- Verify actuator signal exists in VSS
- Ensure fixture-runner has ownership of actuator

## Architecture

```
┌─────────────────┐
│  Test Process   │
│  (GTest)        │
└────┬────────┬───┘
     │        │
     │        └──────────────────┐
     │                           │
     ▼                           ▼
┌──────────────┐        ┌────────────────┐
│ fixture-     │◀──────▶│ KUKSA          │
│ runner       │  gRPC  │ Databroker     │
│ (subprocess) │        │ (Docker)       │
└──────────────┘        └────────────────┘
                               ▲
                               │ gRPC
                               │
                        ┌──────┴────────┐
                        │ Test Clients  │
                        │ (Commander,   │
                        │  Observer)    │
                        └───────────────┘
```

## Comparison with libkuksa-cpp Tests

This test approach mirrors libkuksa-cpp's integration test strategy:

| Feature | libkuksa-cpp | fixture-runner |
|---------|-------------|----------------|
| Auto Docker management | ✓ | ✓ |
| Real KUKSA databroker | ✓ | ✓ |
| GTest framework | ✓ | ✓ |
| Test fixture base class | ✓ | ✓ |
| Subprocess management | N/A | ✓ |
| YAML-driven tests | ✓ | Future work |

## Future Enhancements

- [ ] Add YAML-driven declarative tests (like libkuksa-cpp's YamlTestFixture)
- [ ] Test fixture configuration validation
- [ ] Performance benchmarks
- [ ] Docker Compose integration for complex scenarios
- [ ] CI/CD pipeline integration

## Contributing

When adding new tests:
1. Follow existing test naming: `FixtureRunner<Description>`
2. Use `wait_for()` helper for async assertions
3. Always stop() clients in test cleanup
4. Document what the test verifies in comments
5. Keep tests independent - no shared state
