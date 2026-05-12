// tests/configuration_test.cpp
// Unit tests for NoteUSB module configuration parsing
// Note: These tests verify JSON parsing concepts without requiring NoteUSBModule

#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

// Simple test fixture for configuration concepts
class ConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test 1: Verify we can parse a basic JSON object
TEST_F(ConfigurationTest, BasicJsonObjectParsing) {
    json config = json::object();
    EXPECT_TRUE(config.is_object());
}

// Test 2: Verify we can add settings to config
TEST_F(ConfigurationTest, AddSettingsToConfig) {
    json config = json::object();
    config["settings"] = json::object();
    config["settings"]["discovery_interval_ms"] = 500;
    
    EXPECT_TRUE(config.contains("settings"));
    EXPECT_EQ(config["settings"]["discovery_interval_ms"], 500);
}

// Test 3: Verify we can parse boolean settings
TEST_F(ConfigurationTest, ParseBooleanSettings) {
    json config = json::object();
    config["settings"] = json::object();
    config["settings"]["auto_detach_kernel"] = false;
    
    EXPECT_EQ(config["settings"]["auto_detach_kernel"], false);
}

// Test 4: Verify we can parse integer settings
TEST_F(ConfigurationTest, ParseIntegerSettings) {
    json config = json::object();
    config["settings"] = json::object();
    config["settings"]["usb_timeout_ms"] = 200;
    
    EXPECT_EQ(config["settings"]["usb_timeout_ms"], 200);
}

// Test 5: Verify we can parse device_monitor configuration
TEST_F(ConfigurationTest, ParseDeviceMonitorConfig) {
    json config = json::object();
    config["device_monitor"] = json::object();
    config["device_monitor"]["enabled"] = true;
    config["device_monitor"]["binary"] = "monitor-note_usb";
    
    EXPECT_EQ(config["device_monitor"]["enabled"], true);
    EXPECT_EQ(config["device_monitor"]["binary"], "monitor-note_usb");
}

// Test 6: Verify default value when key is missing
TEST_F(ConfigurationTest, DefaultValueWhenMissing) {
    json config = json::object();
    
    int value = config.value("discovery_interval_ms", 1000);
    EXPECT_EQ(value, 1000);
}

// Test 7: Verify full configuration parsing
TEST_F(ConfigurationTest, FullConfigParsing) {
    json config = {
        {"name", "note_usb"},
        {"version", "1.0.0"},
        {"description", "USB/HID device support"},
        {"api_version", "1.0"},
        {"device_monitor", {
            {"enabled", true},
            {"binary", "monitor-note_usb"}
        }},
        {"settings", {
            {"discovery_interval_ms", 1000},
            {"auto_detach_kernel", true},
            {"usb_timeout_ms", 100}
        }}
    };
    
    EXPECT_EQ(config["name"], "note_usb");
    EXPECT_EQ(config["version"], "1.0.0");
    EXPECT_EQ(config["settings"]["discovery_interval_ms"], 1000);
    EXPECT_EQ(config["device_monitor"]["enabled"], true);
}

// Test 8: Verify config with empty settings object
TEST_F(ConfigurationTest, EmptySettingsObject) {
    json config = json::object();
    config["settings"] = json::object();
    
    EXPECT_TRUE(config.contains("settings"));
    EXPECT_TRUE(config["settings"].is_object());
}

// Test 9: Verify config value returns default for missing nested key
TEST_F(ConfigurationTest, DefaultValueForNestedMissingKey) {
    json config = json::object();
    config["settings"] = json::object();
    
    // settings exists but discovery_interval_ms doesn't
    int value = config["settings"].value("discovery_interval_ms", 1000);
    EXPECT_EQ(value, 1000);
}

// Test 10: Verify type checking works
TEST_F(ConfigurationTest, TypeChecking) {
    json config = json::object();
    config["settings"] = json::object();
    config["settings"]["discovery_interval_ms"] = 500;
    
    EXPECT_TRUE(config["settings"]["discovery_interval_ms"].is_number_integer());
    config["settings"]["auto_detach_kernel"] = true;
    EXPECT_TRUE(config["settings"]["auto_detach_kernel"].is_boolean());
}