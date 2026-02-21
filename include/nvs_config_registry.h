#ifndef NVS_CONFIG_REGISTRY_H
#define NVS_CONFIG_REGISTRY_H

#include <ArduinoJson.h>
#include <stdint.h>

class Print;  // forward declaration

enum ConfigType { CFG_BOOL, CFG_U32, CFG_FLOAT };

struct ConfigField {
    const char* key;         // NVS key (e.g. "hbInt")
    const char* description; // human-readable (e.g. "Heartbeat interval (s)")
    ConfigType  type;
};

// Build JSON with requested fields (all if count==0)
void configBuildJson(JsonDocument& doc, const char** fields, uint8_t count);

// Apply key-value pairs from JSON to NvsConfigManager, returns count of fields set
uint8_t configApplyJson(const JsonObjectConst& obj);

// Print all field names with descriptions
void configListFields(Print& out);

// Look up a field by key (returns nullptr if not found)
const ConfigField* configLookup(const char* key);

// Total number of registry entries
uint8_t configFieldCount();

// Get field by index
const ConfigField* configFieldByIndex(uint8_t idx);

#endif // NVS_CONFIG_REGISTRY_H
