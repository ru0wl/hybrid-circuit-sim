#pragma once

#include "hcs/circuit.hpp"

#include <cstddef>
#include <cstdint>

namespace hcs::detail {

#if defined(HCS_HAVE_AVX2_KERNEL)
void evaluate_avx2_batch(LogicOp opcode, const std::uint8_t* a,
                         const std::uint8_t* b, std::uint8_t* output,
                         std::size_t count) noexcept;
#endif

} // namespace hcs::detail
