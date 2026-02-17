#include "ftm_manager.h"
#include "mesh_conductor.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include "sq_log.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <string.h>
#include <math.h>

static const char* TAG = "ftm";

// --- File-scope state ---

static SemaphoreHandle_t s_ftmSem   = nullptr;
static bool     s_ftmSuccess        = false;
static uint32_t s_ftmRttRaw         = 0;      // raw RTT in pico-seconds from report
static float    s_ftmDistResult     = -1.0f;   // computed distance in cm
static bool     s_busy              = false;

// Responder calibration offset
static int16_t  s_responderOffset   = 0;

// Current session context (for sending result back)
static uint8_t  s_currentResponder[6] = {};
static uint8_t  s_ownMac[6]           = {};

// --- FTM event handler ---

static void ftmEventHandler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_FTM_REPORT) {
        wifi_event_ftm_report_t* report = (wifi_event_ftm_report_t*)event_data;

        if (report->status == FTM_STATUS_SUCCESS) {
            // Average the RTT samples with 2-sigma outlier rejection
            uint32_t count = report->ftm_report_num_entries;
            if (count > 0 && report->ftm_report_data != NULL) {
                // First pass: compute mean RTT
                double sum = 0;
                uint32_t valid = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (report->ftm_report_data[i].rtt != 0) {
                        sum += (double)report->ftm_report_data[i].rtt;
                        valid++;
                    }
                }

                if (valid > 0) {
                    double mean = sum / valid;

                    // Second pass: compute std deviation
                    double var_sum = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        if (report->ftm_report_data[i].rtt != 0) {
                            double diff = (double)report->ftm_report_data[i].rtt - mean;
                            var_sum += diff * diff;
                        }
                    }
                    double stddev = (valid > 1) ? sqrt(var_sum / (valid - 1)) : 0;

                    // Third pass: average with 2-sigma filter
                    double filtered_sum = 0;
                    uint32_t filtered_count = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        if (report->ftm_report_data[i].rtt != 0) {
                            double rtt = (double)report->ftm_report_data[i].rtt;
                            if (stddev == 0 || fabs(rtt - mean) <= 2.0 * stddev) {
                                filtered_sum += rtt;
                                filtered_count++;
                            }
                        }
                    }

                    if (filtered_count > 0) {
                        double avg_rtt_ps = filtered_sum / filtered_count;
                        double avg_rtt_ns = avg_rtt_ps / 1000.0;  // pico to nano
                        // distance = (RTT_ns * speed_of_light_cm_per_ns) / 2
                        // speed of light ≈ 30 cm/ns ≈ 0.03 cm/ps
                        s_ftmDistResult = (float)((avg_rtt_ns * 30.0) / 2.0);
                        s_ftmDistResult += (float)s_responderOffset;
                        s_ftmSuccess = true;

                        SqLog.printf("[ftm] RTT avg=%.0f ps (kept %u/%u), dist=%.1f cm\n",
                            avg_rtt_ps, filtered_count, count, s_ftmDistResult);
                    } else {
                        s_ftmSuccess = false;
                    }
                } else {
                    s_ftmSuccess = false;
                }
            } else {
                // Use the report-level distance estimate as fallback
                s_ftmDistResult = (float)report->dist_est / 100.0f;  // mm to cm
                s_ftmDistResult += (float)s_responderOffset;
                s_ftmSuccess = true;
                SqLog.printf("[ftm] Report-level dist=%.1f cm\n", s_ftmDistResult);
            }

            // Free the report data
            free(report->ftm_report_data);
        } else {
            SqLog.printf("[ftm] FTM failed, status=%d\n", report->status);
            s_ftmSuccess = false;
            s_ftmDistResult = -1.0f;
        }

        if (s_ftmSem) xSemaphoreGive(s_ftmSem);
    }
}

// --- Public API ---

void FtmManager::init() {
    if (s_ftmSem == nullptr) {
        s_ftmSem = xSemaphoreCreateBinary();
    }

    esp_read_mac(s_ownMac, ESP_MAC_WIFI_STA);

    // Register FTM event handler
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT,
                               &ftmEventHandler, NULL);

    // Set responder offset from NVS
    s_responderOffset = (int16_t)(uint32_t)NvsConfigManager::ftmResponderOffset_cm;

    SqLog.printf("[ftm] Initialized, responder offset=%d cm\n", s_responderOffset);
}

float FtmManager::initiateSession(const uint8_t* target_ap_mac, uint8_t channel, uint8_t samples) {
    if (s_busy) {
        SqLog.println("[ftm] Session already in progress");
        return -1.0f;
    }

    s_busy = true;
    s_ftmSuccess = false;
    s_ftmDistResult = -1.0f;

    wifi_ftm_initiator_cfg_t cfg = {};
    memcpy(cfg.resp_mac, target_ap_mac, 6);
    cfg.channel = channel;
    cfg.frm_count = (samples > 0) ? samples : 8;
    cfg.burst_period = 2;  // 200ms burst period

    SqLog.printf("[ftm] Initiating to %02X:%02X:%02X:%02X:%02X:%02X ch=%d frames=%d\n",
        target_ap_mac[0], target_ap_mac[1], target_ap_mac[2],
        target_ap_mac[3], target_ap_mac[4], target_ap_mac[5],
        channel, cfg.frm_count);

    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    if (err != ESP_OK) {
        SqLog.printf("[ftm] esp_wifi_ftm_initiate_session failed: %s\n", esp_err_to_name(err));
        s_busy = false;
        return -1.0f;
    }

    // Wait for result (with timeout)
    uint32_t timeout_ms = (uint32_t)NvsConfigManager::ftmPairTimeout_ms;
    if (xSemaphoreTake(s_ftmSem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        SqLog.println("[ftm] Session timed out");
        esp_wifi_ftm_end_session();
        s_busy = false;
        return -1.0f;
    }

    s_busy = false;
    return s_ftmSuccess ? s_ftmDistResult : -1.0f;
}

void FtmManager::setResponderOffset(int16_t offset_cm) {
    s_responderOffset = offset_cm;
}

void FtmManager::onFtmWake(const uint8_t* initiator_mac, const uint8_t* responder_mac,
                            const uint8_t* responder_ap_mac) {
    // Check if we're involved in this pair
    bool isInitiator = (memcmp(s_ownMac, initiator_mac, 6) == 0);
    bool isResponder = (memcmp(s_ownMac, responder_mac, 6) == 0);

    if (!isInitiator && !isResponder) return;

    SqLog.printf("[ftm] WAKE received — I am %s\n", isInitiator ? "INITIATOR" : "RESPONDER");

    // Store responder AP MAC for the GO message
    if (isInitiator) {
        memcpy(s_currentResponder, responder_ap_mac, 6);
    }

    // Send FTM_READY back to gateway
    FtmReadyMsg ready;
    ready.type = MSG_TYPE_FTM_READY;
    memcpy(ready.mac, s_ownMac, 6);
    MeshConductor::sendToRoot(&ready, sizeof(ready));
}

void FtmManager::onFtmGo(const uint8_t* target_ap_mac, uint8_t samples) {
    SqLog.printf("[ftm] GO received — ranging to %02X:%02X:%02X:%02X:%02X:%02X\n",
        target_ap_mac[0], target_ap_mac[1], target_ap_mac[2],
        target_ap_mac[3], target_ap_mac[4], target_ap_mac[5]);

    float dist = initiateSession(target_ap_mac, MESH_CHANNEL, samples);

    // Send result back to gateway
    FtmResultMsg result;
    result.type = MSG_TYPE_FTM_RESULT;
    memcpy(result.initiator, s_ownMac, 6);
    memcpy(result.responder, s_currentResponder, 6);  // STA MAC, not AP MAC
    result.distance_cm = dist;
    result.status = (dist >= 0) ? 0 : 1;  // 0=ok, 1=timeout

    MeshConductor::sendToRoot(&result, sizeof(result));
}

bool FtmManager::isBusy() {
    return s_busy;
}
