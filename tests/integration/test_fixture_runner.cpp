/**
 * Integration tests for fixture runner
 *
 * Tests the actual fixture-runner binary's ability to:
 * - Register as actuator provider
 * - Receive actuation commands
 * - Simulate hardware delays
 * - Publish actual values
 */

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <kuksa_cpp/kuksa.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kuksa_test_fixture.hpp"

using namespace kuksa;

// Test signal paths
constexpr const char* TEST_DOOR_ACTUATOR = "Vehicle.Cabin.Door.Row1.Left.IsLocked";
constexpr const char* TEST_HVAC_ACTUATOR = "Vehicle.Cabin.HVAC.Station.Row1.Left.Temperature";

/**
 * @brief Test fixture for fixture-runner binary integration
 *
 * This fixture:
 * 1. Starts KUKSA databroker in Docker
 * 2. Creates fixture configuration files
 * 3. Launches the actual fixture-runner binary as subprocess
 * 4. Tests interaction with the running fixture-runner
 */
class FixtureRunnerIntegrationTest : public KuksaTestFixture {
protected:
    void SetUp() override {
        KuksaTestFixture::SetUp();

        // Create resolver
        auto resolver_result = Resolver::create(getKuksaAddress());
        ASSERT_TRUE(resolver_result.ok()) << "Failed to create resolver: " << resolver_result.status();
        resolver_ = std::move(*resolver_result);

        // Create test fixtures config file
        fixtures_config_path_ = "/tmp/test_fixtures.yaml";
    }

    void TearDown() override {
        StopFixtureRunner();

        // Cleanup config file
        if (!fixtures_config_path_.empty()) {
            unlink(fixtures_config_path_.c_str());
        }

        resolver_.reset();

        // Give databroker time to release provider registrations
        // When a provider disconnects, the databroker needs time to clean up
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        KuksaTestFixture::TearDown();
    }

    /**
     * @brief Create a fixtures configuration file
     */
    void CreateFixturesConfig(const YAML::Node& fixtures) {
        std::ofstream file(fixtures_config_path_);
        ASSERT_TRUE(file.is_open()) << "Failed to create fixtures config file";
        file << fixtures;
        file.close();
    }

    /**
     * @brief Start the fixture-runner binary as subprocess
     */
    void StartFixtureRunner() {
        LOG(INFO) << "Starting fixture-runner subprocess...";

        fixture_runner_pid_ = fork();
        ASSERT_GE(fixture_runner_pid_, 0) << "Failed to fork process";

        if (fixture_runner_pid_ == 0) {
            // Child process - exec fixture-runner
            std::string binary_path = std::string(BUILD_DIR) + "/fixture-runner";
            const char* args[] = {
                binary_path.c_str(),
                "--kuksa", getKuksaAddress().c_str(),
                "--config", fixtures_config_path_.c_str(),
                nullptr
            };

            execv(binary_path.c_str(), const_cast<char* const*>(args));

            // If we get here, exec failed
            LOG(ERROR) << "Failed to exec fixture-runner: " << strerror(errno);
            exit(1);
        }

        // Parent process - wait for fixture-runner to start
        LOG(INFO) << "Fixture-runner started with PID: " << fixture_runner_pid_;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Check if process is still alive (detect early exit due to errors)
        int status;
        pid_t result = waitpid(fixture_runner_pid_, &status, WNOHANG);
        if (result > 0) {
            // Process has exited
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                FAIL() << "Fixture-runner exited prematurely with exit code: " << exit_code;
            } else if (WIFSIGNALED(status)) {
                int signal = WTERMSIG(status);
                FAIL() << "Fixture-runner killed by signal: " << signal;
            }
        } else if (result < 0) {
            FAIL() << "waitpid failed: " << strerror(errno);
        }
        // result == 0 means process is still running (success)
    }

    /**
     * @brief Stop the fixture-runner subprocess
     */
    void StopFixtureRunner() {
        if (fixture_runner_pid_ > 0) {
            LOG(INFO) << "Stopping fixture-runner (PID: " << fixture_runner_pid_ << ")...";
            kill(fixture_runner_pid_, SIGTERM);

            // Wait for process to exit
            int status;
            waitpid(fixture_runner_pid_, &status, 0);

            fixture_runner_pid_ = -1;
            LOG(INFO) << "Fixture-runner stopped";
        }
    }

    std::unique_ptr<Resolver> resolver_;
    std::string fixtures_config_path_;
    pid_t fixture_runner_pid_ = -1;
};

/**
 * @brief Test: Fixture runner starts and registers actuator
 *
 * Verifies that the fixture-runner binary can:
 * - Load configuration
 * - Connect to KUKSA databroker
 * - Register as actuator provider
 */
TEST_F(FixtureRunnerIntegrationTest, FixtureRunnerStartsAndRegisters) {
    // Create fixture config with new schema
    YAML::Node config;
    YAML::Node fixture;
    fixture["name"] = "Door Lock Fixture";

    // Actuators this fixture serves
    fixture["serves"].push_back(TEST_DOOR_ACTUATOR);

    // DAG mapping: mirror actuator command to actual value with 100ms delay
    YAML::Node mapping;
    mapping["signal"] = TEST_DOOR_ACTUATOR;
    mapping["depends_on"].push_back(TEST_DOOR_ACTUATOR);
    mapping["datatype"] = "boolean";
    mapping["transform"]["code"] = "delayed(deps[\"" + std::string(TEST_DOOR_ACTUATOR) + "\"], 100)";
    fixture["mappings"].push_back(mapping);

    config["fixture"] = fixture;
    CreateFixturesConfig(config);

    // Start fixture-runner
    StartFixtureRunner();

    // Give it time to register with databroker
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Verify we can send actuation (would fail if fixture not registered)
    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);
    auto commander = std::move(*Client::create(getKuksaAddress()));

    LOG(INFO) << "Sending actuation command to fixture-runner...";
    auto status = commander->set(door_handle, true);
    EXPECT_TRUE(status.ok()) << "Failed to actuate (fixture-runner may not be registered): " << status;
}

/**
 * @brief Test: Fixture runner receives actuation and publishes actual value
 *
 * Complete feedback loop test:
 * 1. Start fixture-runner
 * 2. Send actuation command
 * 3. Verify fixture-runner publishes actual value
 * 4. Observer sees the actual value update
 */
TEST_F(FixtureRunnerIntegrationTest, FixtureRunnerPublishesActualValue) {
    // Create fixture config
    YAML::Node config;
    YAML::Node fixture;
    fixture["name"] = "Door Lock Fixture";

    // Serve the actuator
    fixture["serves"].push_back(TEST_DOOR_ACTUATOR);

    // Mirror mapping with 200ms delay using delayed() function
    YAML::Node mapping;
    mapping["signal"] = TEST_DOOR_ACTUATOR;
    mapping["depends_on"].push_back(TEST_DOOR_ACTUATOR);
    mapping["datatype"] = "boolean";
    mapping["transform"]["code"] = "delayed(deps[\"" + std::string(TEST_DOOR_ACTUATOR) + "\"], 200)";
    fixture["mappings"].push_back(mapping);

    config["fixture"] = fixture;
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);

    // Create observer to watch for actual value updates
    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> update_count(0);
    std::atomic<bool> last_value(false);

    observer->subscribe(door_handle, [&](vss::types::QualifiedValue<bool> qv) {
        if (qv.value.has_value()) {
            LOG(INFO) << "Observer received update: " << *qv.value;
            last_value = *qv.value;
            update_count++;
        }
    });

    observer->start();
    observer->wait_until_ready(std::chrono::seconds(5));

    // Wait for initial subscription update
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int initial_count = update_count.load();
    LOG(INFO) << "Initial update count: " << initial_count;

    // Start fixture-runner
    StartFixtureRunner();

    // Wait for fixture-runner to complete provider registration
    // The fixture-runner takes ~100ms to reach STREAMING state
    // Need extra time for subprocess startup and initialization
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Create commander
    auto commander = std::move(*Client::create(getKuksaAddress()));

    // Give databroker time to propagate provider registration info
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send actuation command
    LOG(INFO) << "Sending actuation command: lock door";
    auto status = commander->set(door_handle, true);
    ASSERT_TRUE(status.ok()) << "Failed to send actuation: " << status;

    // Wait for fixture-runner to process and publish actual value
    // Should take ~200ms (delay) + processing time
    ASSERT_TRUE(wait_for([&]() { return update_count.load() > initial_count; }, std::chrono::seconds(5)))
        << "Did not receive actual value update from fixture-runner within timeout";

    EXPECT_TRUE(last_value.load()) << "Fixture-runner published incorrect actual value";

    observer->stop();
}

/**
 * @brief Test: Fixture runner handles multiple actuators
 */
TEST_F(FixtureRunnerIntegrationTest, FixtureRunnerHandlesMultipleActuators) {
    // Check if HVAC signal exists
    auto hvac_handle_result = resolver_->get<int32_t>(TEST_HVAC_ACTUATOR);
    if (!hvac_handle_result.ok()) {
        GTEST_SKIP() << "HVAC actuator not available in VSS: " << hvac_handle_result.status();
    }

    // Create fixture config serving multiple actuators
    YAML::Node config;
    YAML::Node fixture;
    fixture["name"] = "Multi-Actuator Fixture";

    // Serve both actuators
    fixture["serves"].push_back(TEST_DOOR_ACTUATOR);
    fixture["serves"].push_back(TEST_HVAC_ACTUATOR);

    // Door mapping
    YAML::Node door_mapping;
    door_mapping["signal"] = TEST_DOOR_ACTUATOR;
    door_mapping["depends_on"].push_back(TEST_DOOR_ACTUATOR);
    door_mapping["datatype"] = "boolean";
    door_mapping["transform"]["code"] = "delayed(deps[\"" + std::string(TEST_DOOR_ACTUATOR) + "\"], 100)";
    fixture["mappings"].push_back(door_mapping);

    // HVAC mapping
    YAML::Node hvac_mapping;
    hvac_mapping["signal"] = TEST_HVAC_ACTUATOR;
    hvac_mapping["depends_on"].push_back(TEST_HVAC_ACTUATOR);
    hvac_mapping["datatype"] = "int32";
    hvac_mapping["transform"]["code"] = "delayed(deps[\"" + std::string(TEST_HVAC_ACTUATOR) + "\"], 150)";
    fixture["mappings"].push_back(hvac_mapping);

    config["fixture"] = fixture;
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);
    auto hvac_handle = *hvac_handle_result;

    // Create observers for both actuators
    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> door_updates(0);
    std::atomic<int> hvac_updates(0);

    observer->subscribe(door_handle, [&](vss::types::QualifiedValue<bool> qv) {
        if (qv.value.has_value()) {
            door_updates++;
        }
    });

    observer->subscribe(hvac_handle, [&](vss::types::QualifiedValue<int32_t> qv) {
        if (qv.value.has_value()) {
            hvac_updates++;
        }
    });

    observer->start();
    observer->wait_until_ready(std::chrono::seconds(5));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int initial_door = door_updates.load();
    int initial_hvac = hvac_updates.load();

    // Start fixture-runner
    StartFixtureRunner();

    // Send actuations
    auto commander = std::move(*Client::create(getKuksaAddress()));

    LOG(INFO) << "Actuating door lock...";
    commander->set(door_handle, true);

    LOG(INFO) << "Actuating HVAC temperature...";
    commander->set(hvac_handle, static_cast<int32_t>(22));

    // Wait for both to be processed
    ASSERT_TRUE(wait_for([&]() { return door_updates.load() > initial_door; }, std::chrono::seconds(5)))
        << "Door actuator not processed by fixture-runner";

    ASSERT_TRUE(wait_for([&]() { return hvac_updates.load() > initial_hvac; }, std::chrono::seconds(5)))
        << "HVAC actuator not processed by fixture-runner";

    observer->stop();
}

/**
 * @brief Test: Fixture runner respects configured delays
 */
TEST_F(FixtureRunnerIntegrationTest, FixtureRunnerRespectsConfiguredDelay) {
    // Create fixture with 500ms delay
    YAML::Node config;
    YAML::Node fixture;
    fixture["name"] = "Slow Door Fixture";

    fixture["serves"].push_back(TEST_DOOR_ACTUATOR);

    YAML::Node mapping;
    mapping["signal"] = TEST_DOOR_ACTUATOR;
    mapping["depends_on"].push_back(TEST_DOOR_ACTUATOR);
    mapping["datatype"] = "boolean";
    mapping["transform"]["code"] = "delayed(deps[\"" + std::string(TEST_DOOR_ACTUATOR) + "\"], 500)";  // 500ms delay
    fixture["mappings"].push_back(mapping);

    config["fixture"] = fixture;
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);

    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> update_count(0);
    std::chrono::steady_clock::time_point update_time;
    std::mutex time_mutex;

    observer->subscribe(door_handle, [&](vss::types::QualifiedValue<bool> qv) {
        if (qv.value.has_value()) {
            std::lock_guard<std::mutex> lock(time_mutex);
            update_time = std::chrono::steady_clock::now();
            update_count++;
        }
    });

    observer->start();
    observer->wait_until_ready(std::chrono::seconds(5));

    // Clear initial subscription updates
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int initial_count = update_count.load();
    LOG(INFO) << "Initial update count: " << initial_count;

    StartFixtureRunner();

    // Wait for fixture-runner to complete provider registration
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto commander = std::move(*Client::create(getKuksaAddress()));

    // Record when we send the actuation
    auto start_time = std::chrono::steady_clock::now();
    commander->set(door_handle, true);

    // Wait for NEW update (not the initial ones)
    ASSERT_TRUE(wait_for([&]() { return update_count.load() > initial_count; }, std::chrono::seconds(5)))
        << "Did not receive actual value update";

    // Measure elapsed time
    std::lock_guard<std::mutex> lock(time_mutex);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        update_time - start_time
    ).count();

    LOG(INFO) << "Elapsed time from actuation to actual: " << elapsed << "ms";

    // Should be at least 500ms (the configured delay)
    // Allow some margin for processing time
    EXPECT_GE(elapsed, 450) << "Update came too fast (delay not respected)";
    EXPECT_LE(elapsed, 1000) << "Update took too long";

    observer->stop();
}

/**
 * @brief Test: Cross-signal fixture effects with automatic type widening
 *
 * Tests that one actuator can affect a different signal with compatible types.
 * Demonstrates automatic type widening (int8 → int32).
 */
TEST_F(FixtureRunnerIntegrationTest, FixtureCrossSignalEffect) {
    // Use int8 actuator affecting int32 actuator (compatible types - auto widening)
    constexpr const char* ACTUATOR_SIGNAL = "Vehicle.Private.Test.Int8Actuator";
    constexpr const char* AFFECTED_SIGNAL = "Vehicle.Private.Test.Int32Actuator";

    // Create fixture where actuating one signal affects another
    YAML::Node config;
    YAML::Node fixture;
    fixture["name"] = "Cross-Signal Test Fixture";

    // Serve both actuators
    fixture["serves"].push_back(ACTUATOR_SIGNAL);
    fixture["serves"].push_back(AFFECTED_SIGNAL);

    // Mapping 1: int8 → int32 (cross-signal with auto-widening)
    YAML::Node mapping1;
    mapping1["signal"] = AFFECTED_SIGNAL;
    mapping1["depends_on"].push_back(ACTUATOR_SIGNAL);
    mapping1["datatype"] = "int32";
    mapping1["transform"]["code"] = "delayed(deps[\"" + std::string(ACTUATOR_SIGNAL) + "\"], 300)";
    fixture["mappings"].push_back(mapping1);

    // Mapping 2: Mirror actuator to itself (with narrowing support)
    YAML::Node mapping2;
    mapping2["signal"] = ACTUATOR_SIGNAL;
    mapping2["depends_on"].push_back(ACTUATOR_SIGNAL);
    mapping2["datatype"] = "int8";
    mapping2["transform"]["code"] = "delayed(deps[\"" + std::string(ACTUATOR_SIGNAL) + "\"], 100)";
    fixture["mappings"].push_back(mapping2);

    config["fixture"] = fixture;
    CreateFixturesConfig(config);

    auto actuator_handle = *resolver_->get<int8_t>(ACTUATOR_SIGNAL);
    auto affected_handle = *resolver_->get<int32_t>(AFFECTED_SIGNAL);

    // Create observer for the affected signal
    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> affected_updates(0);
    std::atomic<int32_t> affected_value(0);
    std::atomic<int> actuator_updates(0);

    // Subscribe to the affected signal (int32)
    observer->subscribe(affected_handle, [&](vss::types::QualifiedValue<int32_t> qv) {
        if (qv.value.has_value()) {
            LOG(INFO) << "Affected signal (int32) received update: " << *qv.value;
            affected_value = *qv.value;
            affected_updates++;
        }
    });

    // Also subscribe to the actuator signal to see both updates (int8)
    observer->subscribe(actuator_handle, [&](vss::types::QualifiedValue<int8_t> qv) {
        if (qv.value.has_value()) {
            LOG(INFO) << "Actuator signal (int8) received update: " << static_cast<int>(*qv.value);
            actuator_updates++;
        }
    });

    observer->start();
    observer->wait_until_ready(std::chrono::seconds(5));

    // Wait for initial subscription updates
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int initial_affected = affected_updates.load();
    int initial_actuator = actuator_updates.load();
    LOG(INFO) << "Initial updates - Affected: " << initial_affected
              << ", Actuator: " << initial_actuator;

    // Start fixture-runner
    StartFixtureRunner();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Create commander
    auto commander = std::move(*Client::create(getKuksaAddress()));

    // Send actuation command to the actuator signal (int8 = 42)
    LOG(INFO) << "Sending actuation command to " << ACTUATOR_SIGNAL << " = 42";
    auto status = commander->set(actuator_handle, static_cast<int8_t>(42));
    ASSERT_TRUE(status.ok()) << "Failed to send actuation: " << status;

    // Wait for both effects to be published
    // Effect 1 (affected signal) has 300ms delay
    // Effect 2 (actuator signal) has 100ms delay
    ASSERT_TRUE(wait_for([&]() {
        return affected_updates.load() > initial_affected;
    }, std::chrono::seconds(5)))
        << "Affected signal (int32) did not receive update within timeout";

    ASSERT_TRUE(wait_for([&]() {
        return actuator_updates.load() > initial_actuator;
    }, std::chrono::seconds(5)))
        << "Actuator signal (int8) did not receive update within timeout";

    // Verify the cross-signal effect worked with automatic type widening
    // int8(42) should be automatically widened to int32(42)
    EXPECT_EQ(affected_value.load(), 42)
        << "Cross-signal effect: int32 actuator should be 42 when int8 actuator is 42 (auto-widening)";

    LOG(INFO) << "Cross-signal fixture test passed with automatic type widening!";
    LOG(INFO) << "Actuated: " << ACTUATOR_SIGNAL << " (int8=42) → Affected: " << AFFECTED_SIGNAL << " (int32=42)";

    observer->stop();
}

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
