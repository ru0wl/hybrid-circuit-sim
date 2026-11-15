#include "simd_internal.hpp"

#include <immintrin.h>

namespace hcs::detail {

void evaluate_avx2_batch(LogicOp opcode, const std::uint8_t* a,
                         const std::uint8_t* b, std::uint8_t* output,
                         std::size_t count) noexcept {
    const __m256i one = _mm256_set1_epi8(1);
    std::size_t offset = 0;
    for (; offset + 32 <= count; offset += 32) {
        const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + offset));
        const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + offset));
        __m256i value = _mm256_setzero_si256();
        switch (opcode) {
            case LogicOp::Constant0: value = _mm256_setzero_si256(); break;
            case LogicOp::Constant1: value = one; break;
            case LogicOp::Buffer: value = va; break;
            case LogicOp::Not: value = _mm256_xor_si256(va, one); break;
            case LogicOp::And: value = _mm256_and_si256(va, vb); break;
            case LogicOp::Or: value = _mm256_or_si256(va, vb); break;
            case LogicOp::Xor: value = _mm256_xor_si256(va, vb); break;
            case LogicOp::Nand: value = _mm256_xor_si256(_mm256_and_si256(va, vb), one); break;
            case LogicOp::Nor: value = _mm256_xor_si256(_mm256_or_si256(va, vb), one); break;
            case LogicOp::Xnor: value = _mm256_xor_si256(_mm256_xor_si256(va, vb), one); break;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + offset), value);
    }
    // The caller batches exact 32-byte chunks. Keep this tail for defensive use.
    for (; offset < count; ++offset) {
        const bool lhs = a[offset] != 0;
        const bool rhs = b[offset] != 0;
        bool value = false;
        switch (opcode) {
            case LogicOp::Constant0: value = false; break;
            case LogicOp::Constant1: value = true; break;
            case LogicOp::Buffer: value = lhs; break;
            case LogicOp::Not: value = !lhs; break;
            case LogicOp::And: value = lhs && rhs; break;
            case LogicOp::Or: value = lhs || rhs; break;
            case LogicOp::Xor: value = lhs != rhs; break;
            case LogicOp::Nand: value = !(lhs && rhs); break;
            case LogicOp::Nor: value = !(lhs || rhs); break;
            case LogicOp::Xnor: value = lhs == rhs; break;
        }
        output[offset] = static_cast<std::uint8_t>(value);
    }
}

} // namespace hcs::detail
