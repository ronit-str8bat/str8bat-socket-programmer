/* Augment the Pico SDK default linker script; do not replace it. */
SECTIONS { .programmer_storage_guard (INFO) : { } }
INSERT AFTER .flash_end;
ASSERT(__flash_binary_end <= __programmer_app_limit, "Application overlaps OTA image slot A; adjust partition layout before release")
ASSERT(__StackTop == 0x20080000 && __StackBottom >= 0x2007c000, "Network build needs the SDK 2.3 relocatable 16 KiB core-0 stack")
ASSERT(__HeapLimit <= 0x2007c000, "Heap overlaps network stack")
