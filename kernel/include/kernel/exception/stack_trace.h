#pragma once

#include <stdint.h>
#include <stdbool.h>

bool stack_trace_load_symbols(const char *path);

const char *stack_trace_symbol_lookup(uint64_t address, uint64_t *sym_addr_out);

int stack_trace_collect(uint64_t *rbp, uint64_t *rips, int max);

/// @brief print the walk of RBP as hexidecimal
/// @param rbp frame pointer
void stack_trace_print(uint64_t *rbp);