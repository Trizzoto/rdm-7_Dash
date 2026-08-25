idf_component_register(SRCS "rdm_can_park.c")

# The bootloader's own hook symbols are weak, so without a strong reference
# into this archive the linker drops it and the hook never runs. Defining
# bootloader_hooks_include() (called by the bootloader) forces the include.
