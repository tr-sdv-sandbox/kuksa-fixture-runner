#pragma once

#include <gtest/gtest.h>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <glog/logging.h>

/**
 * @brief Base test fixture for KUKSA integration tests
 *
 * Automatically manages KUKSA databroker Docker container lifecycle.
 * Provides helper functions for async test assertions.
 */
class KuksaTestFixture : public ::testing::Test {
protected:
    static std::string databroker_address_;
    static std::string databroker_container_name_;
    static bool use_external_databroker_;

    static void SetUpTestSuite() {
        // Check if external databroker is configured
        const char* env_address = std::getenv("KUKSA_ADDRESS");
        if (env_address) {
            databroker_address_ = env_address;
            use_external_databroker_ = true;
            LOG(INFO) << "Using external KUKSA databroker at: " << databroker_address_;
            return;
        }

        // Start local Docker container
        use_external_databroker_ = false;
        databroker_address_ = "localhost:55556";
        databroker_container_name_ = "kuksa-test-databroker";

        LOG(INFO) << "Creating VSS test configuration...";
        CreateVSSConfig();

        LOG(INFO) << "Starting KUKSA databroker container...";

        // Stop and remove any existing container
        system(("docker stop " + databroker_container_name_ + " 2>/dev/null").c_str());
        system(("docker rm " + databroker_container_name_ + " 2>/dev/null").c_str());

        // Get absolute path to VSS config
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            LOG(ERROR) << "Failed to get current directory";
            throw std::runtime_error("Failed to get cwd");
        }
        std::string vss_path = std::string(cwd) + "/vss_test.json";

        // Start new container with test VSS schema
        std::string docker_cmd =
            "docker run -d "
            "--name " + databroker_container_name_ + " "
            "-p 55556:55555 "
            "-v " + vss_path + ":/vss/vss_test.json:ro "
            "ghcr.io/eclipse-kuksa/kuksa-databroker:0.6.0 "
            "--vss /vss/vss_test.json";

        int result = system(docker_cmd.c_str());
        if (result != 0) {
            LOG(ERROR) << "Failed to start KUKSA databroker container";
            throw std::runtime_error("Failed to start databroker");
        }

        // Wait for databroker to be ready
        LOG(INFO) << "Waiting for databroker to be ready...";
        std::this_thread::sleep_for(std::chrono::seconds(5));

        LOG(INFO) << "KUKSA databroker ready at: " << databroker_address_;
    }

    static void TearDownTestSuite() {
        if (!use_external_databroker_) {
            LOG(INFO) << "Stopping KUKSA databroker container...";
            system(("docker stop " + databroker_container_name_).c_str());
            system(("docker rm " + databroker_container_name_).c_str());

            // Clean up VSS test file
            std::remove("vss_test.json");
        }
    }

    static void CreateVSSConfig() {
        std::ofstream vss_file("vss_test.json");
        vss_file << R"({
  "Vehicle": {
    "type": "branch",
    "description": "High-level vehicle data",
    "children": {
      "Private": {
        "type": "branch",
        "description": "Private test signals",
        "children": {
          "Test": {
            "type": "branch",
            "description": "Test signals for integration testing",
            "children": {
              "BoolActuator": {
                "type": "actuator",
                "datatype": "boolean",
                "description": "Test bool actuator"
              },
              "Int32Actuator": {
                "type": "actuator",
                "datatype": "int32",
                "description": "Test int32 actuator"
              },
              "FloatSensor": {
                "type": "sensor",
                "datatype": "float",
                "description": "Test float sensor"
              }
            }
          }
        }
      },
      "Cabin": {
        "type": "branch",
        "description": "Cabin signals",
        "children": {
          "Door": {
            "type": "branch",
            "description": "Door signals",
            "children": {
              "Row1": {
                "type": "branch",
                "description": "Row 1",
                "children": {
                  "Left": {
                    "type": "branch",
                    "description": "Left side",
                    "children": {
                      "IsLocked": {
                        "type": "actuator",
                        "datatype": "boolean",
                        "description": "Door lock status"
                      }
                    }
                  }
                }
              }
            }
          },
          "HVAC": {
            "type": "branch",
            "description": "HVAC signals",
            "children": {
              "Station": {
                "type": "branch",
                "description": "HVAC stations",
                "children": {
                  "Row1": {
                    "type": "branch",
                    "description": "Row 1",
                    "children": {
                      "Left": {
                        "type": "branch",
                        "description": "Left side",
                        "children": {
                          "Temperature": {
                            "type": "actuator",
                            "datatype": "int32",
                            "description": "Temperature setpoint"
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})";
        vss_file.close();
    }

    void SetUp() override {
        // Per-test setup
    }

    void TearDown() override {
        // Wait for connections to close cleanly
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    /**
     * @brief Get the KUKSA databroker address
     */
    static std::string getKuksaAddress() {
        return databroker_address_;
    }

    /**
     * @brief Wait for a condition with timeout
     * @param pred Predicate function that returns bool
     * @param timeout Maximum time to wait
     * @return true if condition met, false if timeout
     */
    template<typename Predicate>
    bool wait_for(Predicate pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
};

// Static member initialization
std::string KuksaTestFixture::databroker_address_;
std::string KuksaTestFixture::databroker_container_name_;
bool KuksaTestFixture::use_external_databroker_ = false;
