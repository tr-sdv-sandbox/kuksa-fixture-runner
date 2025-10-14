/**
 * Hardware Fixture Runner - KUKSA Actuator Simulator
 *
 * Simulates hardware responses to actuator commands.
 * Claims ownership of actuators and mirrors commanded values to actual values.
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <map>
#include <memory>
#include <variant>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <glog/logging.h>
#include <kuksa_cpp/kuksa.hpp>

using json = nlohmann::json;
using namespace kuksa;

struct ActuatorFixture {
    std::string name;
    std::string target_signal;
    std::string actual_signal;
    double delay_seconds;
};

struct WorkItem {
    std::shared_ptr<DynamicSignalHandle> handle;
    Value value;
};

class FixtureRunner {
private:
    std::unique_ptr<Resolver> resolver_;
    std::shared_ptr<Client> client_;
    std::string kuksa_address_;
    std::vector<ActuatorFixture> fixtures_;
    bool running_ = false;

    // Work queue for async processing
    std::queue<WorkItem> work_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;

public:
    FixtureRunner(const std::string& kuksa_address)
        : kuksa_address_(kuksa_address) {
    }

    void LoadFixtures(const std::string& config_file) {
        // Check if file exists and is a regular file
        struct stat st;
        if (stat(config_file.c_str(), &st) != 0) {
            std::cerr << "[ERROR] Config file does not exist: " << config_file << std::endl;
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            std::cerr << "[ERROR] Config path is a directory, not a file: " << config_file << std::endl;
            return;
        }

        std::ifstream file(config_file);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Failed to open fixture config: " << config_file << std::endl;
            return;
        }

        json config;
        file >> config;

        if (!config.contains("fixtures")) {
            std::cerr << "No fixtures defined in config" << std::endl;
            return;
        }

        for (const auto& fixture_json : config["fixtures"]) {
            if (fixture_json["type"] != "actuator_mirror") {
                std::cerr << "Unsupported fixture type: " << fixture_json["type"] << std::endl;
                continue;
            }

            ActuatorFixture fixture;
            fixture.name = fixture_json.value("name", "Unnamed Fixture");
            fixture.target_signal = fixture_json["target_signal"];
            fixture.actual_signal = fixture_json["actual_signal"];
            fixture.delay_seconds = fixture_json.value("delay", 0.1);

            fixtures_.push_back(fixture);
            std::cout << "[INFO] Loaded fixture: " << fixture.name << std::endl;
        }
    }

    void Start() {
        running_ = true;

        // Create resolver to resolve signal handles
        auto resolver_result = Resolver::create(kuksa_address_);
        if (!resolver_result.ok()) {
            LOG(ERROR) << "Failed to create resolver: " << resolver_result.status();
            return;
        }
        resolver_ = std::move(*resolver_result);

        // Create client
        auto client_result = Client::create(kuksa_address_);
        if (!client_result.ok()) {
            LOG(ERROR) << "Failed to create client: " << client_result.status();
            return;
        }
        client_ = std::move(*client_result);

        // Register actuator handlers for all fixtures
        for (const auto& fixture : fixtures_) {
            LOG(INFO) << "Fixture: " << fixture.name
                      << " will provide " << fixture.target_signal
                      << " with " << fixture.delay_seconds << "s delay";

            // Resolve signal handle dynamically
            auto handle_result = resolver_->get_dynamic(fixture.target_signal);
            if (!handle_result.ok()) {
                LOG(ERROR) << "Failed to resolve signal " << fixture.target_signal
                          << ": " << handle_result.status();
                continue;
            }
            auto handle = *handle_result;

            // Register actuator handler
            // Note: callback runs on gRPC thread, so we queue work to process later
            auto status = client_->serve_actuator(*handle,
                [this, fixture, delay = fixture.delay_seconds](
                    const Value& target, const DynamicSignalHandle& handle) {
                    HandleActuation(target, fixture, delay, handle);
                }
            );

            if (!status.ok()) {
                LOG(ERROR) << "Failed to register actuator " << fixture.target_signal
                          << ": " << status;
            }
        }

        // Start worker thread for processing actuations
        worker_thread_ = std::thread(&FixtureRunner::WorkerLoop, this);

        // Start client (validates all handlers and connects)
        auto start_status = client_->start();
        if (!start_status.ok()) {
            LOG(ERROR) << "Failed to start client: " << start_status;
            return;
        }

        // Wait for client to be ready
        auto ready_status = client_->wait_until_ready(std::chrono::seconds(10));
        if (!ready_status.ok()) {
            LOG(ERROR) << "Client not ready: " << ready_status;
            return;
        }

        LOG(INFO) << "Started fixture runner for " << fixtures_.size() << " actuator(s)";
    }

    void Run() {
        // Keep running
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void Stop() {
        running_ = false;

        // Signal worker thread to stop
        queue_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        if (client_) {
            client_->stop();
        }
        LOG(INFO) << "All fixtures stopped";
    }

private:
    // Handle actuation request from databroker (runs on gRPC thread)
    void HandleActuation(const Value& target, const ActuatorFixture& fixture,
                        double delay_seconds, const DynamicSignalHandle& handle) {
        LOG(INFO) << "[" << fixture.name << "] Received actuation: "
                  << handle.path() << " (id=" << handle.id() << ")";

        // Queue work item for worker thread to process
        // Do NOT publish from this callback - it causes gRPC errors!
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            work_queue_.push({
                std::make_shared<DynamicSignalHandle>(handle),
                target
            });
        }
        queue_cv_.notify_one();
    }

    // Worker thread loop - processes actuations and publishes actual values
    void WorkerLoop() {
        LOG(INFO) << "Worker thread started";

        while (running_) {
            WorkItem item;

            // Wait for work
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return !work_queue_.empty() || !running_; });

                if (!running_ && work_queue_.empty()) {
                    break;
                }

                if (work_queue_.empty()) {
                    continue;
                }

                item = work_queue_.front();
                work_queue_.pop();
            }

            // Find fixture config to get delay
            const ActuatorFixture* fixture_config = nullptr;
            for (const auto& f : fixtures_) {
                if (f.target_signal == item.handle->path()) {
                    fixture_config = &f;
                    break;
                }
            }

            // Simulate hardware delay
            if (fixture_config && fixture_config->delay_seconds > 0) {
                LOG(INFO) << "[" << fixture_config->name << "] Simulating hardware delay: "
                         << fixture_config->delay_seconds << "s";
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(
                        static_cast<int>(fixture_config->delay_seconds * 1000)
                    )
                );
            }

            // Publish actual value (safe to do from worker thread)
            LOG(INFO) << "Publishing actual value for " << item.handle->path();
            auto status = client_->publish(*item.handle, item.value);
            if (!status.ok()) {
                LOG(ERROR) << "Failed to publish actual value: " << status;
            } else {
                LOG(INFO) << "Actuation complete for " << item.handle->path();
            }
        }

        LOG(INFO) << "Worker thread stopped";
    }
};

int main(int argc, char* argv[]) {
    // Initialize glog
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    std::string kuksa_address = "databroker:55555";
    std::string config_file = "/app/fixtures.json";

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--kuksa" && i + 1 < argc) {
            kuksa_address = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
    }

    LOG(INFO) << "=== Hardware Fixture Runner ===";
    LOG(INFO) << "KUKSA address: " << kuksa_address;
    LOG(INFO) << "Config file: " << config_file;

    FixtureRunner runner(kuksa_address);
    runner.LoadFixtures(config_file);
    runner.Start();
    runner.Run();

    runner.Stop();
    return 0;
}
