// This file is part of picoFace (GNU GPL v3 or later; see LICENSE).
// The few things BTstack's SBC files want from the rest of BTstack, for the PC check
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
void btstack_assert_failed(const char* file, unsigned short line_nr) { fprintf(stderr, "assert %s:%u\n", file, line_nr); abort(); }
void hci_dump_log(int log_level, const char* format, ...) { (void) log_level; (void) format; }
