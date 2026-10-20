/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "bridge_memory.h"
#include <string.h>
#include "esp_log.h"
#include "llm/claw_llm_http_transport.h"

esp_err_t __real_claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
    claw_llm_http_response_t *response, char **error_message);

esp_err_t __wrap_claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
    claw_llm_http_response_t *response, char **error_message)
{
    /* Report sizes and status only; prompts, credentials and URLs stay private.
     * The underlying transport keeps ownership and error handling unchanged.
     */
    unsigned bytes = request && request->body ? (unsigned)strlen(request->body) : 0;
    bridge_memory_log("before LLM HTTP");
    esp_err_t err = __real_claw_llm_http_post_json(request, response, error_message);
    bridge_memory_log("after LLM HTTP");
    ESP_LOGI("bridge_llm_http", "request_bytes=%u result=%s status=%d", bytes,
             esp_err_to_name(err), response ? response->status_code : 0);
    return err;
}
