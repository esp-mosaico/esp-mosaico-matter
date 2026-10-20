/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_event_router.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "router_defaults.h"

esp_err_t __real_claw_event_router_init(const claw_event_router_config_t *config);

esp_err_t __wrap_claw_event_router_init(const claw_event_router_config_t *config)
{
    if (!config) {
        return __real_claw_event_router_init(config);
    }

    if (config->rules_path && config->rules_path[0] &&
        config->recovery_rules_path && config->recovery_rules_path[0]) {
        /* Upstream treats a missing primary file as a valid empty ruleset,
         * so its recovery path never runs on a new LittleFS DATA partition.
         */
        bool seeded = false;
        esp_err_t err = matter_bridge_seed_router_rules(config->rules_path,
                                                       config->recovery_rules_path,
                                                       &seeded);
        if (err != ESP_OK) {
            ESP_LOGE("matter_bridge", "Seed router rules failed: %s", esp_err_to_name(err));
            return err;
        }
        if (seeded) ESP_LOGI("matter_bridge", "Installed default chat routing rules");
    }

    /* Routing retains several event/rule copies while calling the agent,
     * session storage and outbound capabilities. The upstream 8 KiB budget
     * leaves too little room for those nested calls on ESP32-S3. IDF stack
     * sizes are bytes; retain a caller's larger budget and all other options.
     * The router copies this configuration during initialization.
     */
    claw_event_router_config_t adjusted = *config;
    if (adjusted.task_stack_size < CONFIG_MATTER_BRIDGE_ROUTER_STACK_SIZE) {
        adjusted.task_stack_size = CONFIG_MATTER_BRIDGE_ROUTER_STACK_SIZE;
    }
    ESP_LOGI("matter_bridge", "Event router stack: %lu bytes",
             (unsigned long)adjusted.task_stack_size);
    return __real_claw_event_router_init(&adjusted);
}
