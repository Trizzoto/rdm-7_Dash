/*
 * rdm_can_park.c — park the CAN transmit line recessive at the very start
 * of the second-stage bootloader.
 *
 * WHY THIS EXISTS
 *
 * TWAI TX is GPIO20, which on the ESP32-S3 is also USB D+. Out of reset the
 * USB Serial/JTAG PHY owns the pad and — measured on a dash, 2026-08-25 —
 * leaves it with no pull-up, no pull-down and no driver:
 *
 *     app_main   usb_pad_en=1 dp_pullup=0 iomux20 PU=0 PD=0 out_en=0
 *
 * A floating pin into the transceiver's TXD input reads as a logic 0, and
 * TXD low means "drive the bus dominant". The transceiver therefore holds
 * the whole bus dominant for the entire startup. Nothing else on the bus
 * can transmit, and an ECU that powers up in that window sees a jammed bus,
 * errors out and gives up — which is exactly the Link G4 boot-order fault
 * (plug in after the engine is running and the ECU is already healthy, so
 * it rides the jam out and recovers).
 *
 * The TWAI driver only takes the pad when twai_driver_install() runs, which
 * is ~1.54 s into boot: the app image mapping (~610 ms), the PSRAM XIP copy
 * (~350 ms) and the PSRAM test (~190 ms) all happen before app_main is
 * reached at ~1.40 s. None of that is shrinkable, so parking the line from
 * the application is far too late — hence a bootloader hook, which runs at
 * ~25 ms.
 *
 * Parking recessive is enough on its own: the bus only needs to be QUIET
 * early, not driven. can_init() keeps its existing position in app_main so
 * the acceptance filter is still built after the channel registry loads.
 *
 * Runs before BSS init — no statics, no logging, register writes only.
 */
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/usb_serial_jtag_reg.h"

#define RDM_CAN_TX_GPIO 20

/* Feature stamp, read back out of flash by main/storage/bootloader_selfupdate.c.
 *
 * The app can compare the bootloader in flash against the copy it carries, but
 * a byte comparison answers the wrong question: any rebuild changes bytes (a
 * console pin, a toolchain bump), and prompting a customer to rewrite their
 * bootloader — a write that bricks the dash if power drops mid-way — for a
 * cosmetic difference is not a trade worth making. What the dash actually
 * needs to know is whether the bootloader has THE FIX, so the bootloader says
 * so itself.
 *
 * Fixed layout on purpose: the prefix is searched for verbatim in a flash
 * read-back, and the four ASCII digits after it are the version. Bump the
 * version when a change here must reach dashes already in cars; leave it alone
 * for anything that does not change behaviour.
 *
 *   0000  no marker at all — a bootloader from before this existed, which may
 *         or may not park the CAN line. Treated as "needs the fix", because
 *         offering a redundant write is the safe direction.
 *   0001  parks TWAI TX (GPIO20) recessive before init. ADR-0047.
 */
/* `retain` keeps it through --gc-sections, which the bootloader is built
 * with; `used` alone only stops the compiler dropping it, and the first
 * build of this stamp went out without it because the linker collected it. */
__attribute__((used, retain))
const char rdm_bl_feature_marker[] = "RDM-BL-FEAT:0001";

/* Tell the linker to keep this translation unit — the hooks below override
 * weak symbols and would otherwise be dropped. */
void bootloader_hooks_include(void) {
	/* A real reference to the stamp, so keeping it does not rest on an
	 * attribute alone. Costs nothing: no load is actually performed. */
	__asm__ volatile("" :: "r"(rdm_bl_feature_marker));
}

void bootloader_before_init(void) {
	const uint32_t mask = (1u << RDM_CAN_TX_GPIO);

	/* 1. Hand the pad back from the USB PHY to the IO mux. While
	 *    usb_pad_enable is set the PHY owns GPIO19/20 and the GPIO matrix
	 *    is ignored. */
	CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG,
	                    USB_SERIAL_JTAG_USB_PAD_ENABLE);

	/* 2. Pull-up first, so the pad is already recessive during the few
	 *    cycles before the output driver is enabled, and stays recessive
	 *    if anything later releases the driver. */
	SET_PERI_REG_MASK(IO_MUX_GPIO20_REG, FUN_PU);
	CLEAR_PERI_REG_MASK(IO_MUX_GPIO20_REG, FUN_PD);

	/* 3. Plain GPIO function, driven from the GPIO_OUT register. */
	PIN_FUNC_SELECT(IO_MUX_GPIO20_REG, PIN_FUNC_GPIO);
	REG_WRITE(GPIO_FUNC20_OUT_SEL_CFG_REG, SIG_GPIO_OUT_IDX);

	/* 4. Level BEFORE output-enable — the other order glitches the line
	 *    dominant for a moment, which is the thing we are here to avoid. */
	REG_WRITE(GPIO_OUT_W1TS_REG, mask);
	REG_WRITE(GPIO_ENABLE_W1TS_REG, mask);
}
