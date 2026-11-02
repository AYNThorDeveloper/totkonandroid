/* Android has no Mach-O sections. Registration records that Apple builds
 * place in "__DATA,<name>" use the ELF section <name> instead (NVN_SECTION),
 * whose bounds the linker publishes as __start_<name>/__stop_<name>. */
#pragma once
const void *android_getsectiondata(const char *section, unsigned long *size);
#define getsectiondata(header, segment, section, size) android_getsectiondata((section), (size))
#define _mh_execute_header android_no_mach_header
extern const char android_no_mach_header;
