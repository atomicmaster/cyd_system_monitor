// SPDX-License-Identifier: Apache-2.0
//
// Proves the C++ consumer builds against generated output. Not the
// product protocol's golden-vector suite; that arrives with C01.
#include <cstdio>

#include "protocol_generated.hpp"

int main() {
  protocol::generated::Smoke smoke{1, "hello", true};
  if (smoke.sequence != 1 || smoke.message != "hello" || !smoke.ok) {
    std::fprintf(stderr, "FAIL: smoke message field values did not round-trip\n");
    return 1;
  }
  std::printf("protocol_generated_tests: ok\n");
  return 0;
}
