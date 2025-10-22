/**
 * Hardware Fixture Runner - KUKSA Actuator Simulator
 *
 * Simulates hardware responses to actuator commands using VssDAG for computation.
 * Claims ownership of actuators (serves) and uses DAG to calculate effect values.
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <map>
#include <memory>
#include <vector>
#include <algorithm>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sys/stat.h>
#include <glog/logging.h>
#include <kuksa_cpp/kuksa.hpp>
#include <vssdag/signal_processor.h>
#include <vssdag/mapping_types.h>
#include <vssdag/signal_source_info.h>
#include <vss/types/value.hpp>
#include <vss/types/quality.hpp>

using namespace kuksa;
using namespace vssdag;

struct FixtureConfig {
    std::string name;
    std::vector<std::string> serves;  // Actuators to register
    std::unordered_map<std::string, SignalMapping> mappings;  // DAG mappings
};

class FixtureRunner {
private:
    std::unique_ptr<Resolver> resolver_;
    std::shared_ptr<Client> client_;
    std::string kuksa_address_;
    FixtureConfig config_;
    std::unique_ptr<SignalProcessorDAG> dag_processor_;
    bool running_ = false;

    // Map signal paths to resolved handles for faster publishing
    std::unordered_map<std::string, std::shared_ptr<DynamicSignalHandle>> signal_handles_;

    // Transform mappings for VssDAG: add .target suffix to served actuators
    std::unordered_map<std::string, SignalMapping> CreateDAGMappings() {
        std::unordered_map<std::string, SignalMapping> dag_mappings;

        // Transform user mappings: add .target suffix for served actuators
        for (const auto& [signal_name, mapping] : config_.mappings) {
            SignalMapping dag_mapping = mapping;

            // Transform depends_on: add .target to served actuators
            dag_mapping.depends_on.clear();
            for (const auto& dep : mapping.depends_on) {
                if (std::find(config_.serves.begin(), config_.serves.end(), dep) != config_.serves.end()) {
                    dag_mapping.depends_on.push_back(dep + ".target");
                } else {
                    dag_mapping.depends_on.push_back(dep);
                }
            }

            // Transform code: replace served actuator references with .target
            if (std::holds_alternative<vssdag::CodeTransform>(mapping.transform)) {
                std::string code = std::get<vssdag::CodeTransform>(mapping.transform).expression;

                for (const auto& actuator : config_.serves) {
                    // Find and replace: deps["actuator"] -> deps["actuator.target"]
                    std::string search = "deps[\"" + actuator + "\"]";
                    std::string replace = "deps[\"" + actuator + ".target\"]";
                    size_t pos = 0;
                    while ((pos = code.find(search, pos)) != std::string::npos) {
                        code.replace(pos, search.length(), replace);
                        pos += replace.length();
                    }
                    // Also handle single quotes: deps['actuator']
                    search = "deps['" + actuator + "']";
                    replace = "deps['" + actuator + ".target']";
                    pos = 0;
                    while ((pos = code.find(search, pos)) != std::string::npos) {
                        code.replace(pos, search.length(), replace);
                        pos += replace.length();
                    }
                }

                dag_mapping.transform = vssdag::CodeTransform{.expression = code};
            }

            dag_mappings[signal_name] = dag_mapping;
        }

        // Add .target signals as source signals (external inputs)
        for (const auto& actuator : config_.serves) {
            std::string target_signal = actuator + ".target";
            if (dag_mappings.find(target_signal) == dag_mappings.end()) {
                SignalMapping target_mapping;
                target_mapping.datatype = vss::types::ValueType::UNSPECIFIED;
                // Mark as input signal by setting source
                target_mapping.source = vssdag::SignalSource{"actuator", target_signal};
                // No depends_on = external input signal
                dag_mappings[target_signal] = target_mapping;
            }
        }

        return dag_mappings;
    }

public:
    FixtureRunner(const std::string& kuksa_address)
        : kuksa_address_(kuksa_address) {
    }

    void LoadConfig(const std::string& config_file) {
        // Check if file exists and is a regular file
        struct stat st;
        if (stat(config_file.c_str(), &st) != 0) {
            LOG(ERROR) << "Config file does not exist: " << config_file;
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            LOG(ERROR) << "Config path is a directory, not a file: " << config_file;
            return;
        }

        try {
            YAML::Node root = YAML::LoadFile(config_file);

            if (!root["fixture"]) {
                LOG(ERROR) << "No 'fixture' section in config";
                return;
            }

            const YAML::Node& fixture = root["fixture"];

            // Parse fixture name
            config_.name = fixture["name"].as<std::string>("Unnamed Fixture");

            // Parse serves section
            if (!fixture["serves"]) {
                LOG(ERROR) << "No 'serves' section in fixture config";
                return;
            }

            for (const auto& signal_node : fixture["serves"]) {
                std::string signal_path = signal_node.as<std::string>();
                config_.serves.push_back(signal_path);
            }

            LOG(INFO) << "Fixture '" << config_.name << "' will serve "
                      << config_.serves.size() << " actuator(s)";

            // Parse mappings section (VssDAG format)
            if (!fixture["mappings"]) {
                LOG(ERROR) << "No 'mappings' section in fixture config";
                return;
            }

            for (const auto& mapping_node : fixture["mappings"]) {
                if (!mapping_node["signal"]) {
                    continue;
                }

                std::string signal_name = mapping_node["signal"].as<std::string>();
                SignalMapping mapping;

                // Parse datatype
                if (mapping_node["datatype"]) {
                    std::string datatype_str = mapping_node["datatype"].as<std::string>();
                    auto datatype_opt = vss::types::value_type_from_string(datatype_str);
                    if (datatype_opt.has_value()) {
                        mapping.datatype = *datatype_opt;
                    } else {
                        LOG(WARNING) << "Unknown datatype '" << datatype_str << "' for signal " << signal_name;
                        mapping.datatype = vss::types::ValueType::UNSPECIFIED;
                    }
                } else {
                    mapping.datatype = vss::types::ValueType::UNSPECIFIED;
                }

                // Parse depends_on (keep original signal names)
                if (mapping_node["depends_on"]) {
                    for (const auto& dep : mapping_node["depends_on"]) {
                        std::string dep_signal = dep.as<std::string>();
                        mapping.depends_on.push_back(dep_signal);
                    }
                }

                // Parse delay (convert to interval_ms for DAG)
                if (mapping_node["delay"]) {
                    double delay_seconds = mapping_node["delay"].as<double>();
                    mapping.interval_ms = static_cast<int>(delay_seconds * 1000);
                }

                // Parse transform code (keep original signal names)
                if (mapping_node["transform"] && mapping_node["transform"]["code"]) {
                    std::string code = mapping_node["transform"]["code"].as<std::string>();
                    mapping.transform = vssdag::CodeTransform{.expression = code};
                }

                config_.mappings[signal_name] = mapping;
            }

            LOG(INFO) << "Loaded " << config_.mappings.size() << " signal mappings";

        } catch (const YAML::Exception& e) {
            LOG(ERROR) << "Failed to parse YAML config: " << e.what();
        }
    }

    void Start() {
        // Create resolver
        auto resolver_result = Resolver::create(kuksa_address_);
        if (!resolver_result.ok()) {
            LOG(ERROR) << "Failed to create resolver: " << resolver_result.status();
            running_ = false;
            return;
        }
        resolver_ = std::move(*resolver_result);

        // Create client
        auto client_result = Client::create(kuksa_address_);
        if (!client_result.ok()) {
            LOG(ERROR) << "Failed to create client: " << client_result.status();
            running_ = false;
            return;
        }
        client_ = std::move(*client_result);

        // Pre-resolve all signal handles (for served actuators and DAG outputs)
        std::unordered_set<std::string> all_signals(config_.serves.begin(), config_.serves.end());
        for (const auto& [signal_path, mapping] : config_.mappings) {
            all_signals.insert(signal_path);
        }

        for (const auto& signal_path : all_signals) {
            auto handle_result = resolver_->get_dynamic(signal_path);
            if (!handle_result.ok()) {
                LOG(ERROR) << "Failed to resolve signal " << signal_path
                          << ": " << handle_result.status();
                LOG(ERROR) << "Cannot start fixture - signal resolution failed";
                running_ = false;
                return;  // FAIL FAST - critical error
            }
            signal_handles_[signal_path] = *handle_result;
        }

        // Register actuator handlers for all served actuators
        for (const auto& actuator_path : config_.serves) {
            auto it = signal_handles_.find(actuator_path);
            if (it == signal_handles_.end()) {
                LOG(ERROR) << "Cannot register actuator " << actuator_path
                          << " - signal handle not resolved";
                running_ = false;
                return;  // FAIL FAST - critical error
            }

            LOG(INFO) << "Registering actuator: " << actuator_path;

            client_->serve_actuator(*it->second,
                [this, actuator_path](
                    const vss::types::Value& target, const DynamicSignalHandle& handle) {
                    HandleActuation(actuator_path, target);
                }
            );
        }

        // Initialize DAG processor with transformed mappings (.target suffix added)
        dag_processor_ = std::make_unique<SignalProcessorDAG>();
        auto dag_mappings = CreateDAGMappings();
        LOG(INFO) << "Created " << dag_mappings.size() << " DAG mappings (including "
                  << config_.serves.size() << " .target inputs)";
        if (!dag_processor_->initialize(dag_mappings)) {
            LOG(ERROR) << "Failed to initialize DAG processor";
            running_ = false;
            return;
        }

        // Start client
        auto start_status = client_->start();
        if (!start_status.ok()) {
            LOG(ERROR) << "Failed to start client: " << start_status;
            running_ = false;
            return;
        }

        // Wait for client to be ready
        auto ready_status = client_->wait_until_ready(std::chrono::seconds(10));
        if (!ready_status.ok()) {
            LOG(ERROR) << "Client not ready: " << ready_status;
            running_ = false;
            return;
        }

        // SUCCESS - mark as running
        running_ = true;

        LOG(INFO) << "Started fixture '" << config_.name << "' serving "
                  << config_.serves.size() << " actuator(s)";
    }

    void Run() {
        // Run at 10Hz to process DAG for delayed outputs and continuous simulation
        const auto tick_interval = std::chrono::milliseconds(100);  // 10Hz

        while (running_) {
            // Process DAG periodically to handle:
            // 1. Delayed outputs (signals with interval_ms/delay)
            // 2. Continuous simulation (periodic signals)
            // Call with empty updates to trigger time-based processing
            std::vector<vssdag::SignalUpdate> empty_updates;
            std::vector<vssdag::VSSSignal> outputs = dag_processor_->process_signal_updates(empty_updates);

            // Publish any outputs from DAG
            for (const auto& vss_signal : outputs) {
                if (!vss_signal.qualified_value.is_valid()) {
                    continue;
                }

                auto handle_it = signal_handles_.find(vss_signal.path);
                if (handle_it == signal_handles_.end()) {
                    LOG(WARNING) << "No handle for output signal: " << vss_signal.path;
                    continue;
                }

                LOG(INFO) << "[" << config_.name << "] Publishing DAG output: " << vss_signal.path;

                auto status = client_->publish(*handle_it->second, vss_signal.qualified_value);
                if (!status.ok()) {
                    LOG(ERROR) << "Failed to publish " << vss_signal.path << ": " << status;
                }
            }

            // Sleep for 100ms (10Hz tick rate)
            std::this_thread::sleep_for(tick_interval);
        }
    }

    bool IsRunning() const {
        return running_;
    }

    void Stop() {
        running_ = false;

        if (client_) {
            client_->stop();
        }
        LOG(INFO) << "Fixture stopped";
    }

private:
    // Handle actuation request from databroker
    void HandleActuation(const std::string& actuator_path, const vss::types::Value& target) {
        LOG(INFO) << "[" << config_.name << "] Received actuation: " << actuator_path;

        // Transform actuation to .target signal for VssDAG
        // This allows DAG to distinguish between TARGET (input) and ACTUAL (output)
        std::string target_signal = actuator_path + ".target";

        vssdag::SignalUpdate update{
            target_signal,  // Use .target suffix for DAG input
            target,
            std::chrono::steady_clock::now(),
            vss::types::SignalQuality::VALID
        };

        // Process through DAG
        std::vector<vssdag::SignalUpdate> updates = {update};
        LOG(INFO) << "[" << config_.name << "] Processing DAG with input: " << target_signal;

        // Debug: Check if we have required input signals
        auto required_inputs = dag_processor_->get_required_input_signals();
        LOG(INFO) << "[" << config_.name << "] DAG expects " << required_inputs.size() << " input signals";
        for (const auto& input : required_inputs) {
            LOG(INFO) << "[" << config_.name << "]   - " << input;
        }

        std::vector<vssdag::VSSSignal> outputs = dag_processor_->process_signal_updates(updates);
        LOG(INFO) << "[" << config_.name << "] DAG produced " << outputs.size() << " output(s)";

        // Debug: Log all outputs
        for (const auto& vss_signal : outputs) {
            LOG(INFO) << "[" << config_.name << "]   Output: " << vss_signal.path
                      << " (valid=" << vss_signal.qualified_value.is_valid() << ")";
        }

        // Publish all DAG output signals (these are ACTUAL values)
        for (const auto& vss_signal : outputs) {
            if (!vss_signal.qualified_value.is_valid()) {
                LOG(WARNING) << "Skipping invalid output signal: " << vss_signal.path;
                continue;
            }

            auto handle_it = signal_handles_.find(vss_signal.path);
            if (handle_it == signal_handles_.end()) {
                LOG(WARNING) << "No handle for output signal: " << vss_signal.path;
                continue;
            }

            LOG(INFO) << "[" << config_.name << "] Publishing DAG output: " << vss_signal.path;
            LOG(INFO) << "[" << config_.name << "]   Value variant index: " << vss_signal.qualified_value.value.index();
            LOG(INFO) << "[" << config_.name << "]   Value: " << vssdag::VSSTypeHelper::to_string(vss_signal.qualified_value.value);

            auto status = client_->publish(*handle_it->second, vss_signal.qualified_value);
            if (!status.ok()) {
                LOG(ERROR) << "Failed to publish " << vss_signal.path << ": " << status;
            }
        }
    }
};

int main(int argc, char* argv[]) {
    // Initialize glog
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    std::string kuksa_address = "databroker:55555";
    std::string config_file = "/app/fixture.yaml";

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--kuksa" && i + 1 < argc) {
            kuksa_address = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
    }

    LOG(INFO) << "=== Hardware Fixture Runner ===" ;
    LOG(INFO) << "KUKSA address: " << kuksa_address;
    LOG(INFO) << "Config file: " << config_file;

    FixtureRunner runner(kuksa_address);
    runner.LoadConfig(config_file);
    runner.Start();

    if (!runner.IsRunning()) {
        LOG(ERROR) << "Failed to start fixture runner";
        return 1;
    }

    runner.Run();

    runner.Stop();
    return 0;
}
