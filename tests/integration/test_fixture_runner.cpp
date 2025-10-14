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
#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kuksa_test_fixture.hpp"

using namespace kuksa;
using json = nlohmann::json;

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
        fixtures_config_path_ = "/tmp/test_fixtures.json";
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
    void CreateFixturesConfig(const json& fixtures) {
        std::ofstream file(fixtures_config_path_);
        ASSERT_TRUE(file.is_open()) << "Failed to create fixtures config file";
        file << fixtures.dump(2);
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
    // Create fixtures config with one actuator
    json config = {
        {"fixtures", json::array({
            {
                {"type", "actuator_mirror"},
                {"name", "Door Lock Fixture"},
                {"target_signal", TEST_DOOR_ACTUATOR},
                {"actual_signal", TEST_DOOR_ACTUATOR},
                {"delay", 0.1}
            }
        })}
    };
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
    // Create fixtures config
    json config = {
        {"fixtures", json::array({
            {
                {"type", "actuator_mirror"},
                {"name", "Door Lock Fixture"},
                {"target_signal", TEST_DOOR_ACTUATOR},
                {"actual_signal", TEST_DOOR_ACTUATOR},
                {"delay", 0.2}  // 200ms delay
            }
        })}
    };
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);

    // Create observer to watch for actual value updates
    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> update_count(0);
    std::atomic<bool> last_value(false);

    observer->subscribe(door_handle, [&](std::optional<bool> value) {
        if (value.has_value()) {
            LOG(INFO) << "Observer received update: " << *value;
            last_value = *value;
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

    // Create fixtures config with multiple actuators
    json config = {
        {"fixtures", json::array({
            {
                {"type", "actuator_mirror"},
                {"name", "Door Lock Fixture"},
                {"target_signal", TEST_DOOR_ACTUATOR},
                {"actual_signal", TEST_DOOR_ACTUATOR},
                {"delay", 0.1}
            },
            {
                {"type", "actuator_mirror"},
                {"name", "HVAC Fixture"},
                {"target_signal", TEST_HVAC_ACTUATOR},
                {"actual_signal", TEST_HVAC_ACTUATOR},
                {"delay", 0.15}
            }
        })}
    };
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);
    auto hvac_handle = *hvac_handle_result;

    // Create observers for both actuators
    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> door_updates(0);
    std::atomic<int> hvac_updates(0);

    observer->subscribe(door_handle, [&](std::optional<bool> value) {
        if (value.has_value()) {
            door_updates++;
        }
    });

    observer->subscribe(hvac_handle, [&](std::optional<int32_t> value) {
        if (value.has_value()) {
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
    json config = {
        {"fixtures", json::array({
            {
                {"type", "actuator_mirror"},
                {"name", "Slow Door Fixture"},
                {"target_signal", TEST_DOOR_ACTUATOR},
                {"actual_signal", TEST_DOOR_ACTUATOR},
                {"delay", 0.5}  // 500ms delay
            }
        })}
    };
    CreateFixturesConfig(config);

    auto door_handle = *resolver_->get<bool>(TEST_DOOR_ACTUATOR);

    auto observer = std::move(*Client::create(getKuksaAddress()));
    std::atomic<int> update_count(0);
    std::chrono::steady_clock::time_point update_time;
    std::mutex time_mutex;

    observer->subscribe(door_handle, [&](std::optional<bool> value) {
        if (value.has_value()) {
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

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
