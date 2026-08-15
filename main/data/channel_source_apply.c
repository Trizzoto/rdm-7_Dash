/*
 * channel_source_apply.c — single source-bind path for channels.
 *
 * Both the web /api/channels/bind-source handler and the first-run
 * wizard's channels step call channel_apply_preconfig() here so any
 * future change to "what happens when you pick a preconfig source"
 * lands in one place. See the header for the contract.
 */

#include "channel_source_apply.h"
#include "canonical_channels.h"
#include "../widgets/signal.h"
#include "../can/can_manager.h"
#include "../can/obd2.h"
#include "../layout/ecu_presets.h"
#include "../layout/layout_manager.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "ch_src_apply";

/* Derive a signal_name from a preconfig label using the same rule as
 * web_server_channels.c::_derive_signal_name (which itself mirrors
 * config_modal.c::label_to_signal_name): uppercase, runs of non-alnum
 * collapse to single underscore, trim trailing underscores.
 *
 *   "ENGINE RPM"   -> "ENGINE_RPM"
 *   "WHEEL SPD FL" -> "WHEEL_SPD_FL"
 */
static void _derive_signal_name(const char *label, char *out, size_t sz) {
    if (!label || !out || sz == 0) {
        if (out && sz) out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; label[i] && j < sz - 1; i++) {
        char c = label[i];
        if (isalnum((unsigned char)c))
            out[j++] = (char)toupper((unsigned char)c);
        else if (j > 0 && out[j - 1] != '_')
            out[j++] = '_';
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
}

/* Update or register the runtime signal entry so the channel binds to a
 * live decode immediately — without this, the layout-disk write is
 * invisible until the next layout reload, and resolve_signals() falls
 * back to whatever was previously in the registry. */
static esp_err_t _apply_runtime(const preconfig_item_t *item,
                                const char *signal_name) {
    if (!item || !signal_name || !signal_name[0]) return ESP_ERR_INVALID_ARG;

    uint32_t can_id = (uint32_t)strtol(item->can_id, NULL, 16);
    int16_t idx = signal_find_by_name(signal_name);
    if (idx >= 0) {
        signal_t *s = signal_get_by_index((uint16_t)idx);
        if (s) {
            s->can_id     = can_id;
            s->bit_start  = item->bit_start;
            s->bit_length = item->bit_length;
            s->scale      = item->scale;
            s->offset     = item->value_offset;
            s->is_signed  = item->is_signed;
            s->endian     = item->endianess;
            /* Reset decode freshness so the next frame fires
             * notify_subscribers — see signal_dispatch_frame's
             * was_stale/value gate. Without this an existing
             * non-stale slot whose current_value matches the new
             * decode silently swallows the first update. */
            s->is_stale       = true;
            s->current_value  = 0.0f;
            s->last_update_ms = 0;
        }
        return ESP_OK;
    }
    idx = signal_register(signal_name, can_id,
                          item->bit_start, item->bit_length,
                          item->scale, item->value_offset,
                          item->is_signed, item->endianess, "");
    return (idx >= 0) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t channel_apply_preconfig(channel_t *c, const preconfig_item_t *item) {
    if (!c || !item || !item->label) return ESP_ERR_INVALID_ARG;

    char signal_name[32];
    _derive_signal_name(item->label, signal_name, sizeof(signal_name));
    if (!signal_name[0]) return ESP_ERR_INVALID_ARG;

    /* 1. Update the runtime registry first so live values flow now. */
    esp_err_t rt = _apply_runtime(item, signal_name);
    if (rt != ESP_OK) {
        ESP_LOGW(TAG, "runtime apply failed for %s: %d", signal_name, rt);
        /* Fall through — disk write may still succeed; resolve_signals
         * will pick up the binding on the next layout reload. */
    }

    /* 2. Persist into the active layout's signals[] so the binding
     *    survives reboot. Reuse the shared ecu_preset writer — same
     *    field shape as the bulk-preset apply path. */
    char layout[64];
    esp_err_t le = layout_manager_get_active(layout, sizeof(layout));
    if (le == ESP_OK) {
        uint32_t can_id = (uint32_t)strtol(item->can_id, NULL, 16);
        esp_err_t we = ecu_preset_write_signal_to_layout(
            layout, signal_name,
            can_id, item->bit_start, item->bit_length,
            item->scale, item->value_offset,
            item->is_signed, item->endianess,
            /* unit: preconfig items don't carry a unit string */ NULL,
            item->decimals);
        if (we != ESP_OK) {
            ESP_LOGW(TAG, "layout persist failed for %s: %d", signal_name, we);
        }
    } else {
        ESP_LOGW(TAG, "no active layout (%d), skipping persist", le);
    }

    /* 3. Point the channel at the new signal name + write the decode onto the
     *    channel (ADR 0005 — channels OWN their decode, device-local and
     *    authoritative). set_decode UPSERTs the runtime signal + persists
     *    channels.json, so the binding survives reboot without relying on the
     *    layout's signals[] (which Phase B strips on save). The layout write in
     *    step 2 stays as a harmless local cache — the channel decode wins. */
    channel_manager_set_signal(c, signal_name);
    {
        uint32_t can_id = (uint32_t)strtol(item->can_id, NULL, 16);
        /* Mux gate first: set_decode is what pushes the channel into the
         * signal registry and it carries whatever gate the channel holds at
         * that moment. Always set it, including the cleared form, so
         * rebinding a channel off a multiplexed ECU (Link) onto a plain one
         * doesn't leave a gate that drops every frame. */
        channel_manager_set_mux(c, item->mux_bit_start, item->mux_bit_length,
                                item->mux_value, false);
        /* persist_now=false: channel_manager_set_signal() above already flushed
         * channels.json synchronously — avoid a redundant second full-file
         * write; the decode rides the debounced save. */
        channel_manager_set_decode(c, can_id, item->bit_start, item->bit_length,
                                   item->scale, item->value_offset,
                                   item->is_signed, item->endianess, NULL, false);
    }
    channel_manager_resolve_signals();

    /* 4. Rebuild the TWAI hardware acceptance filter so frames with the
     *    new signal's CAN ID actually reach the receive task. Without
     *    this, when the user picks a source whose CAN ID isn't already
     *    in the filter mask (e.g. Battery Voltage on a different ID
     *    than the active ECU's other signals), the channel binds but no
     *    live value ever arrives — the frame gets dropped in hardware.
     *
     *    Skipped when the bus is in promiscuous mode (ACCEPT_ALL),
     *    which is the case during the wizard's ECU probe. */
    if (!can_is_promiscuous()) {
        reconfigure_can_filter();
    }

    ESP_LOGI(TAG, "channel '%s' <- preconfig %s/%s (%s) -> signal '%s'",
             c->id,
             item->ecu     ? item->ecu     : "?",
             item->version ? item->version : "?",
             item->label,
             signal_name);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Studio "full config" layout import (see header for the contract)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Find the channel currently bound to signal_name (case-insensitive),
 * or NULL. O(channels) walk — import is a rare, user-triggered path. */
static channel_t *_channel_bound_to_signal(const char *sname) {
    size_t n = channel_manager_count();
    for (size_t i = 0; i < n; i++) {
        channel_t *c = channel_manager_at(i);
        if (c && c->signal_name[0] && strcasecmp(c->signal_name, sname) == 0)
            return c;
    }
    return NULL;
}

/* Ensure a channel exists for an imported signal and bind it. Mirrors the
 * first-run wizard's _wiz_ensure_channel_for_signal resolution:
 *   1. ECU vocabulary alias → canonical channel
 *   2. exact (case-insensitive) canonical id match
 *   3. auto-create custom_<name>
 * Collision guard: a canonical channel already bound to a DIFFERENT signal
 * is never shared — fall through to a custom channel instead. */
static channel_t *_ensure_channel_for_import(const char *sname,
                                             const char *unit,
                                             uint8_t decimals,
                                             float min, float max) {
    const canonical_channel_def_t *def = NULL;
    const char *canon = ecu_signal_name_to_canonical(sname);
    if (canon) def = canonical_channel_find(canon);
    if (!def)  def = canonical_channel_find_ci(sname);

    channel_t *ch = NULL;
    if (def) {
        ch = channel_manager_get(def->id);
        if (ch && ch->signal_name[0] && strcmp(ch->signal_name, sname) != 0)
            ch = NULL;              /* canonical channel taken — go custom */
        else if (!ch)
            ch = channel_manager_activate(def->id);
    }

    if (!ch) {
        /* "custom_" + lowercased signal name, capped to the 32-char id buf. */
        char cid[32];
        size_t j = 0;
        for (const char *p = "custom_"; *p && j < sizeof(cid) - 1; p++)
            cid[j++] = *p;
        for (size_t k = 0; sname[k] && j < sizeof(cid) - 1; k++) {
            char c = sname[k];
            cid[j++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        cid[j] = '\0';
        ch = channel_manager_get(cid);
        if (!ch)
            ch = channel_manager_create_custom(
                cid, sname, CHGRP_DIAGNOSTIC, CHCARD_SCALAR,
                unit ? unit : "", unit ? unit : "", decimals, min, max);
    }

    if (ch && !ch->signal_name[0])
        channel_manager_set_signal(ch, sname);
    return ch;
}

bool channel_layout_signals_carry_config(const cJSON *signals_arr) {
    if (!cJSON_IsArray(signals_arr)) return false;
    const cJSON *sj;
    cJSON_ArrayForEach(sj, signals_arr) {
        if (!cJSON_IsObject(sj)) continue;
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(sj, "low_warn")) ||
            cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(sj, "high_warn")))
            return true;
        const cJSON *jcid = cJSON_GetObjectItemCaseSensitive(sj, "can_id");
        const cJSON *jbl  = cJSON_GetObjectItemCaseSensitive(sj, "bit_length");
        if (cJSON_IsNumber(jcid) && jcid->valuedouble > 0 &&
            cJSON_IsNumber(jbl) && jbl->valueint > 0)
            return true;
    }
    return false;
}

size_t channel_import_from_layout_signals(cJSON *signals_arr) {
    if (!cJSON_IsArray(signals_arr)) return 0;

    size_t touched = 0;
    bool decode_adopted = false;

    /* One channels.json flush for the whole import (end_bulk), not one
     * full-file LittleFS write per set_* call. */
    channel_manager_begin_bulk();

    cJSON *sj;
    cJSON_ArrayForEach(sj, signals_arr) {
        if (!cJSON_IsObject(sj)) continue;
        const cJSON *jname = cJSON_GetObjectItemCaseSensitive(sj, "name");
        if (!cJSON_IsString(jname) || !jname->valuestring[0]) continue;
        const char *sname = jname->valuestring;

        const cJSON *jlow  = cJSON_GetObjectItemCaseSensitive(sj, "low_warn");
        const cJSON *jhigh = cJSON_GetObjectItemCaseSensitive(sj, "high_warn");
        bool  has_low  = cJSON_IsNumber(jlow);
        bool  has_high = cJSON_IsNumber(jhigh);
        float low_v    = has_low  ? (float)jlow->valuedouble  : 0.0f;
        float high_v   = has_high ? (float)jhigh->valuedouble : 0.0f;

        /* CAN decode — meaningless for internal/OBD2-sourced entries
         * (CALCULATED_GEAR etc. carry no decode; OBD2 decode lives in the
         * PID tables, not the bit-field params). */
        const cJSON *jcid = cJSON_GetObjectItemCaseSensitive(sj, "can_id");
        const cJSON *jbs  = cJSON_GetObjectItemCaseSensitive(sj, "bit_start");
        const cJSON *jbl  = cJSON_GetObjectItemCaseSensitive(sj, "bit_length");
        bool has_decode = cJSON_IsNumber(jcid) && jcid->valuedouble > 0 &&
                          cJSON_IsNumber(jbl) && jbl->valueint > 0;
        const cJSON *jsrc = cJSON_GetObjectItemCaseSensitive(sj, "source");
        if (cJSON_IsString(jsrc) &&
            (strcmp(jsrc->valuestring, "internal") == 0 ||
             strcmp(jsrc->valuestring, "obd2") == 0))
            has_decode = false;

        if (!has_low && !has_high && !has_decode) continue;

        /* Display metadata for custom-channel creation (best effort —
         * studio exports unit/decimals/min/max on the signal entry). */
        uint8_t decimals = 0;
        float   cmin = 0.0f, cmax = 100.0f;
        const char *unit = "";
        const cJSON *it;
        it = cJSON_GetObjectItemCaseSensitive(sj, "decimals");
        if (cJSON_IsNumber(it)) {
            int d = it->valueint;
            decimals = (uint8_t)(d < 0 ? 0 : (d > 3 ? 3 : d));
        }
        it = cJSON_GetObjectItemCaseSensitive(sj, "min");
        if (cJSON_IsNumber(it)) cmin = (float)it->valuedouble;
        it = cJSON_GetObjectItemCaseSensitive(sj, "max");
        if (cJSON_IsNumber(it)) cmax = (float)it->valuedouble;
        it = cJSON_GetObjectItemCaseSensitive(sj, "unit");
        if (cJSON_IsString(it) && it->valuestring) unit = it->valuestring;

        channel_t *ch = _channel_bound_to_signal(sname);
        if (!ch)
            ch = _ensure_channel_for_import(sname, unit, decimals, cmin, cmax);
        if (!ch) {
            ESP_LOGW(TAG, "import: no channel for signal '%s' (cap reached?)",
                     sname);
            continue;       /* keys left in place — nothing consumed them */
        }

        bool changed = false;
        bool adopted_this = false;

        /* Decode: adopt ONLY when the channel has none — never clobber a
         * device-edited decode with the imported layout's copy. */
        if (has_decode && ch->can_id == 0) {
            float   scale = 1.0f, offset = 0.0f;
            bool    is_signed = false;
            uint8_t endian = 1;     /* default Intel, mirrors _load_signals */
            it = cJSON_GetObjectItemCaseSensitive(sj, "scale");
            if (cJSON_IsNumber(it)) scale = (float)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(sj, "offset");
            if (cJSON_IsNumber(it)) offset = (float)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(sj, "is_signed");
            if (cJSON_IsBool(it)) is_signed = cJSON_IsTrue(it);
            it = cJSON_GetObjectItemCaseSensitive(sj, "endian");
            if (cJSON_IsNumber(it)) endian = (uint8_t)it->valueint;

            /* persist_now=false: the bulk guard coalesces into one flush. */
            changed |= channel_manager_set_decode(
                ch, (uint32_t)jcid->valuedouble,
                cJSON_IsNumber(jbs) ? (uint8_t)jbs->valueint : 0,
                (uint8_t)jbl->valueint,
                scale, offset, is_signed, endian, unit, false);
            decode_adopted = true;
            adopted_this = true;
        }

        /* Thresholds: always apply — the import is explicit user intent,
         * and on-device layouts never carry these keys (editor strips
         * them), so a stale re-assert can only come from a re-import. */
        if (has_low)
            changed |= channel_manager_set_threshold(ch, CHZONE_LOW_WARN, low_v);
        if (has_high)
            changed |= channel_manager_set_threshold(ch, CHZONE_HIGH_WARN, high_v);

        /* Strip the consumed threshold keys so the persisted layout can't
         * re-assert them over later on-device channel edits. (Decode keys
         * stay — _load_signals still registers them, and the channel's
         * copy wins via register_decoded_signals.) */
        if (has_low)
            cJSON_DeleteItemFromObjectCaseSensitive(sj, "low_warn");
        if (has_high)
            cJSON_DeleteItemFromObjectCaseSensitive(sj, "high_warn");

        if (changed || has_low || has_high) {
            touched++;
            ESP_LOGI(TAG, "import: '%s' -> channel '%s'%s%s%s",
                     sname, ch->id,
                     has_low || has_high ? " thresholds" : "",
                     adopted_this ? " decode" : "",
                     changed ? "" : " (no change)");
        }
    }

    channel_manager_end_bulk();

    /* Channels created/bound here resolve against whatever is registered
     * now; the subsequent layout load re-resolves after _load_signals
     * registers any brand-new names. Cheap + idempotent. */
    channel_manager_resolve_signals();

    /* New decode can mean new CAN IDs the hardware acceptance filter
     * doesn't pass yet. Skip in promiscuous mode (wizard ECU probe). */
    if (decode_adopted && !can_is_promiscuous())
        reconfigure_can_filter();

    return touched;
}

/* ── OBD2 scan → channels ──────────────────────────────────────────── */

size_t channel_obd2_matches(const uint8_t *pids, size_t n_pids,
                            obd2_channel_match_t *out, size_t max_out) {
    if (!pids || !out || max_out == 0) return 0;

    size_t n = 0;
    /* Walk the map rather than the scan result, so the offer list comes
     * out in canonical order (RPM, coolant, speed…) and stays in the same
     * order across rescans instead of reshuffling with PID numbering. */
    for (size_t i = 0; i < CANONICAL_OBD2_MAP_COUNT && n < max_out; i++) {
        const canonical_obd2_map_t *m = &CANONICAL_OBD2_MAP[i];
        if (m->service != 0x01) continue;      /* discovery reports Mode 01 */

        bool answered = false;
        for (size_t k = 0; k < n_pids; k++) {
            if (pids[k] == (uint8_t)m->pid) { answered = true; break; }
        }
        if (!answered) continue;

        const canonical_channel_def_t *d = canonical_channel_find(m->channel_id);
        const channel_t *c = channel_manager_get(m->channel_id);

        out[n].channel_id  = m->channel_id;
        out[n].label       = (d && d->label) ? d->label : m->channel_id;
        out[n].units       = (d && d->units_native) ? d->units_native : "";
        out[n].signal_name = m->obd2_signal_name;
        out[n].service     = m->service;
        out[n].pid         = m->pid;
        out[n].active      = (c != NULL);
        /* "Bound" means the user already chose a source — including one
         * whose frames haven't arrived this boot (signal_index < 0). Those
         * are deliberate configuration, not gaps to fill. */
        out[n].bound       = (c && c->signal_name[0] != '\0');
        n++;
    }
    return n;
}

esp_err_t channel_apply_obd2(const obd2_channel_match_t *rows, size_t n_rows,
                             size_t *out_bound) {
    if (out_bound) *out_bound = 0;
    if (!rows || n_rows == 0) return ESP_ERR_INVALID_ARG;

    /* Read the currently-polled set once, add every requested PID to it,
     * then save + restart polling once. Binding twelve channels one at a
     * time would rewrite the layout's PID list twelve times. */
    char layout[64];
    bool have_layout = (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK);

    uint32_t pids[OBD2_MAX_ENABLED];
    uint8_t  count = 0;
    if (have_layout)
        ecu_preset_read_obd2_pids(layout, pids, OBD2_MAX_ENABLED, &count);
    else
        count = obd2_get_enabled(pids, OBD2_MAX_ENABLED);

    bool full = false;
    size_t bound = 0;

    channel_manager_begin_bulk();
    for (size_t i = 0; i < n_rows; i++) {
        const obd2_channel_match_t *r = &rows[i];
        if (!r->channel_id || !r->signal_name || !r->signal_name[0]) continue;

        channel_t *c = channel_manager_get(r->channel_id);
        if (!c && canonical_channel_exists(r->channel_id))
            c = channel_manager_activate(r->channel_id);
        if (!c) continue;                       /* unknown id / registry full */

        uint32_t enc = obd2_encode_pid(r->service, r->pid);
        bool dup = false;
        for (uint8_t k = 0; k < count; k++)
            if (pids[k] == enc) { dup = true; break; }
        if (!dup) {
            if (count >= OBD2_MAX_ENABLED) { full = true; continue; }
            pids[count++] = enc;
        }

        channel_manager_set_signal(c, r->signal_name);
        bound++;
    }
    channel_manager_end_bulk();

    if (bound == 0) return full ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;

    channel_manager_resolve_signals();

    /* Poll regardless of whether the save lands — the binds should work
     * for this session even if persistence fails; the caller reports the
     * "won't survive a reboot" part. */
    if (count > 0) obd2_start(pids, count);
    if (out_bound) *out_bound = bound;

    esp_err_t save_err = ESP_OK;
    if (have_layout) save_err = ecu_preset_save_obd2_pids(layout, pids, count);
    else             save_err = ESP_FAIL;

    ESP_LOGI(TAG, "OBD2 bind: %u channel(s), %u PID(s) polled%s",
             (unsigned)bound, (unsigned)count,
             save_err == ESP_OK ? "" : " (not persisted)");

    if (save_err != ESP_OK) return ESP_FAIL;
    return full ? ESP_ERR_NO_MEM : ESP_OK;
}

size_t channel_obd2_prune(void) {
    char layout[64];
    bool have_layout = (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK);

    uint32_t pids[OBD2_MAX_ENABLED];
    uint8_t  count = 0;
    if (have_layout)
        ecu_preset_read_obd2_pids(layout, pids, OBD2_MAX_ENABLED, &count);
    else
        count = obd2_get_enabled(pids, OBD2_MAX_ENABLED);
    if (count == 0) return 0;

    uint32_t keep[OBD2_MAX_ENABLED];
    uint8_t  kept = 0;
    size_t   dropped = 0;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t  svc = obd2_decode_service(pids[i]);
        uint16_t pid = obd2_decode_pid(pids[i]);

        const canonical_obd2_map_t *m = canonical_channel_obd2_for_pid(svc, pid);
        if (!m) { keep[kept++] = pids[i]; continue; }   /* hand-enabled — leave it */

        /* Does any active channel still want this OBD2 signal? Match on the
         * signal name rather than the channel id: two channels can share one
         * OBD2 feed, and the map's channel may have been removed while a
         * custom one kept the binding. */
        bool wanted = false;
        size_t n = channel_manager_count();
        for (size_t k = 0; k < n && !wanted; k++) {
            const channel_t *c = channel_manager_at(k);
            if (c && strcmp(c->signal_name, m->obd2_signal_name) == 0) wanted = true;
        }
        if (wanted) keep[kept++] = pids[i];
        else        dropped++;
    }

    if (dropped == 0) return 0;

    /* obd2_start() with the surviving list, even when it's empty: obd2_stop()
     * deliberately leaves s_poll[] populated (UI may query it after a stop),
     * so stopping alone would leave /api/obd2/pids reporting PIDs nothing is
     * polling any more. obd2_start() zeroes s_poll_count first, so a
     * zero-length call both stops and tells the truth afterwards. */
    obd2_start(keep, kept);
    if (have_layout) ecu_preset_save_obd2_pids(layout, keep, kept);

    ESP_LOGI(TAG, "OBD2 prune: dropped %u unused PID(s), %u still polled",
             (unsigned)dropped, (unsigned)kept);
    return dropped;
}
