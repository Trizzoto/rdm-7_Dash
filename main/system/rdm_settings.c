#include "rdm_settings.h"
#include "layout/layout_manager.h"

esp_err_t rdm_settings_init(void)
{
	/* Delegate to layout_manager_init(), which mounts LittleFS, ensures the
	 * layouts directory exists, and generates/sets a default layout if
	 * necessary. */
	return layout_manager_init();
}

esp_err_t rdm_settings_get_active_layout(char *name_out, size_t len)
{
	if (!name_out || len == 0)
		return ESP_ERR_INVALID_ARG;
	return layout_manager_get_active(name_out, len);
}

esp_err_t rdm_settings_set_active_layout(const char *name)
{
	if (!name)
		return ESP_ERR_INVALID_ARG;
	return layout_manager_set_active(name);
}

