/* esp_attr.h stub for native-host tests.
 *
 * The firmware places large static tables in external PSRAM with
 * EXT_RAM_BSS_ATTR. On the host there is no such segment and no reason to
 * care — plain .bss is the correct translation, so the attributes compile
 * away to nothing and the table under test behaves identically.
 */
#ifndef RDM_TEST_MOCK_ESP_ATTR_H
#define RDM_TEST_MOCK_ESP_ATTR_H

#define EXT_RAM_BSS_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define NOINLINE_ATTR

#endif /* RDM_TEST_MOCK_ESP_ATTR_H */
