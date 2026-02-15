#ifndef PROPERTY_VALUE_H
#define PROPERTY_VALUE_H

#include <nvs.h>
#include <esp_log.h>

// NVS handle and state, defined in nvs_config.cpp
namespace NvsConfig {
    extern nvs_handle_t handle;
    extern bool isOpen;

    inline esp_err_t nvsWrite(const char* key, bool value) {
        return nvs_set_u8(handle, key, value ? 1 : 0);
    }

    inline esp_err_t nvsWrite(const char* key, uint16_t value) {
        return nvs_set_u16(handle, key, value);
    }

    inline esp_err_t nvsWrite(const char* key, uint32_t value) {
        return nvs_set_u32(handle, key, value);
    }

    inline esp_err_t nvsWrite(const char* key, uint64_t value) {
        return nvs_set_u64(handle, key, value);
    }
}

template <const char* nvsKey, typename T, typename Owner>
class PropertyValue {
    friend Owner;

private:
    T _value;

    // Direct load from NVS at startup (bypasses write-back)
    void loadInitial(const T& value) { _value = value; }

public:
    PropertyValue(T init) : _value(init) {}

    PropertyValue& operator=(const T& newValue) {
        _value = newValue;
        if (NvsConfig::isOpen) {
            esp_err_t err = NvsConfig::nvsWrite(nvsKey, newValue);
            if (err == ESP_OK)
                nvs_commit(NvsConfig::handle);
            else
                ESP_LOGE("NVS", "write(%s) failed: %s", nvsKey, esp_err_to_name(err));
        }
        return *this;
    }

    operator T() const { return _value; }
};

#endif // PROPERTY_VALUE_H
