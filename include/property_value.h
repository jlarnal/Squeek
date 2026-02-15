#ifndef PROPERTY_VALUE_H
#define PROPERTY_VALUE_H

#include <nvs.h>
#include <esp_log.h>
#include <string.h>

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

    inline esp_err_t nvsWrite(const char* key, float value) {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return nvs_set_u32(handle, key, bits);
    }
}

template <const char* nvsKey, typename T, typename Owner>
class PropertyValue {
    friend Owner;

public:
    /// Callback fired before a value change is applied.
    /// @param oldValue  current stored value
    /// @param newValue  proposed new value
    /// @param override  points to a T initialised to newValue; modify to substitute
    /// @param cancel    points to a bool initialised to false; set true to abort
    using BeforeChangeFn = void(*)(T oldValue, T newValue, T* override, bool* cancel);

private:
    T _value;
    BeforeChangeFn _beforeChange = nullptr;

    // Direct load from NVS at startup (bypasses write-back and callback)
    void loadInitial(const T& value) { _value = value; }

public:
    PropertyValue(T init) : _value(init) {}

    void setBeforeChange(BeforeChangeFn cb) { _beforeChange = cb; }

    PropertyValue& operator=(const T& newValue) {
        T actualValue = newValue;
        if (_beforeChange) {
            bool cancel = false;
            _beforeChange(_value, newValue, &actualValue, &cancel);
            if (cancel) return *this;
        }
        _value = actualValue;
        if (NvsConfig::isOpen) {
            esp_err_t err = NvsConfig::nvsWrite(nvsKey, actualValue);
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
