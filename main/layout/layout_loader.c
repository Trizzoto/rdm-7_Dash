#include "layout_loader.h"
#include "layout_manager.h"
#include "esp_log.h"

static const char *TAG = "layout_loader";

esp_err_t layout_loader_load_named(const char *name, lv_obj_t *parent)
{
	if (!name || !parent)
		return ESP_ERR_INVALID_ARG;

	/* layout_manager_load() reads /lfs/layouts/{name}.json and instantiates
	 * all widgets described in that JSON onto @p parent. */
	esp_err_t err = layout_manager_load(name, parent);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "layout_loader_load_named('%s') failed: %s", name,
				 esp_err_to_name(err));
	}
	return err;
}

esp_err_t layout_loader_load_active(lv_obj_t *parent)
{
	if (!parent)
		return ESP_ERR_INVALID_ARG;

	char name[LAYOUT_MAX_NAME];
	esp_err_t err = layout_manager_get_active(name, sizeof(name));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "get_active_layout failed: %s", esp_err_to_name(err));
		return err;
	}

	return layout_loader_load_named(name, parent);
}

