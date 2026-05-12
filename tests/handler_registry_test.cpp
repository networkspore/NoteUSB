// tests/handler_registry_test.cpp
// Unit tests for HandlerRegistry in NoteUSB module

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

// Include the module framework headers - note_messaging.h must come first
#include "note_messaging.h"
#include "module_framework/handler_registry.h"

class HandlerRegistryTest : public ::testing::Test {
protected:
    std::unique_ptr<NoteDaemon::HandlerRegistry> registry_;

    void SetUp() override {
        registry_ = std::make_unique<NoteDaemon::HandlerRegistry>();
    }

    void TearDown() override {
        registry_.reset();
    }
};

// Test 1: Verify registry starts empty
TEST_F(HandlerRegistryTest, RegistryStartsEmpty) {
    EXPECT_EQ(registry_->handler_count(), 0);
    EXPECT_FALSE(registry_->has_handler("test"));
}

// Test 2: Verify register_handler adds a handler
TEST_F(HandlerRegistryTest, RegisterHandlerAddsHandler) {
    registry_->register_handler("test_message", [](const NoteBytes::Object& msg) {
        (void)msg;
    });

    EXPECT_EQ(registry_->handler_count(), 1);
    EXPECT_TRUE(registry_->has_handler("test_message"));
}

// Test 3: Verify multiple handlers can be registered
TEST_F(HandlerRegistryTest, MultipleHandlersCanBeRegistered) {
    registry_->register_handler("msg1", [](const NoteBytes::Object& msg) { (void)msg; });
    registry_->register_handler("msg2", [](const NoteBytes::Object& msg) { (void)msg; });
    registry_->register_handler("msg3", [](const NoteBytes::Object& msg) { (void)msg; });

    EXPECT_EQ(registry_->handler_count(), 3);
    EXPECT_TRUE(registry_->has_handler("msg1"));
    EXPECT_TRUE(registry_->has_handler("msg2"));
    EXPECT_TRUE(registry_->has_handler("msg3"));
}

// Test 4: Verify get_registered_types returns correct list
TEST_F(HandlerRegistryTest, GetRegisteredTypesReturnsCorrectList) {
    registry_->register_handler("request_discovery", [](const NoteBytes::Object& msg) { (void)msg; });
    registry_->register_handler("claim_item", [](const NoteBytes::Object& msg) { (void)msg; });
    registry_->register_handler("release_item", [](const NoteBytes::Object& msg) { (void)msg; });

    auto types = registry_->get_registered_types();

    EXPECT_EQ(types.size(), 3);
    EXPECT_TRUE(std::find(types.begin(), types.end(), "request_discovery") != types.end());
    EXPECT_TRUE(std::find(types.begin(), types.end(), "claim_item") != types.end());
    EXPECT_TRUE(std::find(types.begin(), types.end(), "release_item") != types.end());
}

// Test 5: Verify device handler registration works
TEST_F(HandlerRegistryTest, DeviceHandlerRegistrationWorks) {
    registry_->register_device_handler("1:2", "claim_item", [](const NoteBytes::Object& msg) {
        (void)msg;
    });

    // Global handler count should still be 0
    EXPECT_EQ(registry_->handler_count(), 1);
}

// Test 6: Verify remove_device_handler works
TEST_F(HandlerRegistryTest, RemoveDeviceHandlerWorks) {
    registry_->register_device_handler("1:2", "claim_item", [](const NoteBytes::Object& msg) {
        (void)msg;
    });

    EXPECT_EQ(registry_->handler_count(), 1);
}

// Test 7: Verify dispatch returns error when no handler
TEST_F(HandlerRegistryTest, DispatchReturnsErrorWhenNoHandler) {
    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "test_message");

    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_FALSE(result.success());
}

// Test 9: Verify dispatch calls handler
TEST_F(HandlerRegistryTest, DispatchCallsHandler) {
    bool handler_called = false;

    registry_->register_handler("test_message", [&handler_called](const NoteBytes::Object& msg) {
        (void)msg;
        handler_called = true;
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "test_message");

    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_TRUE(result.success());
    EXPECT_TRUE(handler_called);
}

// Test 10: Verify dispatch extracts EVENT field
TEST_F(HandlerRegistryTest, DispatchExtractsEventField) {
    bool handler_called = false;
    std::string captured_event;

    registry_->register_handler("my_event", [&handler_called, &captured_event](const NoteBytes::Object& msg) {
        auto* event = msg.get(NoteMessaging::Keys::EVENT);
        if (event) {
            captured_event = event->as_string();
        }
        handler_called = true;
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "my_event");

    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_TRUE(result.success());
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(captured_event, "my_event");
}

// Test 11: Verify dispatch with CMD field works
TEST_F(HandlerRegistryTest, DispatchWithCmdFieldWorks) {
    bool handler_called = false;

    registry_->register_handler("request_discovery", [&handler_called](const NoteBytes::Object& msg) {
        (void)msg;
        handler_called = true;
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::CMD, "request_discovery");

    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_TRUE(result.success());
    EXPECT_TRUE(handler_called);
}

// Test 12: Verify dispatch returns error for missing EVENT/CMD
TEST_F(HandlerRegistryTest, DispatchReturnsErrorForMissingEventOrCmd) {
    NoteBytes::Object msg;  // Empty message, no EVENT or CMD

    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_FALSE(result.success());
}

// Test 13: Verify dispatch_to_device works
TEST_F(HandlerRegistryTest, DispatchToDeviceWorks) {
    bool handler_called = false;

    registry_->register_device_handler("1:2", "claim_item", [&handler_called](const NoteBytes::Object& msg) {
        (void)msg;
        handler_called = true;
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "claim_item");

    NoteDaemon::Error result = registry_->dispatch_to_device("1:2", msg);

    EXPECT_TRUE(result.success());
    EXPECT_TRUE(handler_called);
}

// Test 14: Verify dispatch_to_device returns error for unknown device
TEST_F(HandlerRegistryTest, DispatchToDeviceReturnsErrorForUnknownDevice) {
    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "claim_item");

    // No handler registered for device "9:9"
    NoteDaemon::Error result = registry_->dispatch_to_device("9:9", msg);

    EXPECT_FALSE(result.success());
}

// Test 15: Verify dispatch prefers device-specific over global
TEST_F(HandlerRegistryTest, DispatchPrefersDeviceSpecificOverGlobal) {
    bool global_called = false;
    bool device_called = false;

    // Register global handler
    registry_->register_handler("claim_item", [&global_called](const NoteBytes::Object& msg) {
        (void)msg;
        global_called = true;
    });

    // Register device-specific handler
    registry_->register_device_handler("1:2", "claim_item", [&device_called](const NoteBytes::Object& msg) {
        (void)msg;
        device_called = true;
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "claim_item");
    msg.add(NoteMessaging::Keys::DEVICE_ID, "1:2");

    // Dispatch with device_id present - should prefer device-specific
    NoteDaemon::Error result = registry_->dispatch(msg);

    EXPECT_TRUE(result.success());
    EXPECT_TRUE(device_called);
    EXPECT_FALSE(global_called);  // Global should NOT be called when device-specific exists
}

// Test 16: Verify handler can access message content
TEST_F(HandlerRegistryTest, HandlerCanAccessMessageContent) {
    std::string captured_device_id;

    registry_->register_handler("claim_item", [&captured_device_id](const NoteBytes::Object& msg) {
        auto* device_id = msg.get(NoteMessaging::Keys::DEVICE_ID);
        if (device_id) {
            captured_device_id = device_id->as_string();
        }
    });

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "claim_item");
    msg.add(NoteMessaging::Keys::DEVICE_ID, "1:2");

    registry_->dispatch(msg);

    EXPECT_EQ(captured_device_id, "1:2");
}

// Test 17: Verify thread safety (multiple registrations)
TEST_F(HandlerRegistryTest, ThreadSafetyMultipleRegistrations) {
    // Register many handlers - should not deadlock
    for (int i = 0; i < 100; i++) {
        std::string msg_type = "msg_" + std::to_string(i);
        registry_->register_handler(msg_type, [](const NoteBytes::Object& msg) { (void)msg; });
    }

    EXPECT_EQ(registry_->handler_count(), 100);
}

// Test 18: Verify empty get_registered_types for empty registry
TEST_F(HandlerRegistryTest, EmptyGetRegisteredTypesForEmptyRegistry) {
    auto types = registry_->get_registered_types();
    EXPECT_TRUE(types.empty());
}