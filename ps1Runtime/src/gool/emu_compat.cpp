/**
 * @file emu_compat.cpp
 * @brief Implementation of the GOOL EMU_* compatibility shim.
 *
 * See `runtime/gool/emu_compat.h` for the rationale. Reads and writes go
 * through `Memory` so scratchpad / MMIO behave; native callouts go
 * through `recomp_dispatch`, which (unlike the c1c interpreter) returns
 * normally because each recompiled function is a real C++ function.
 */

#include "runtime/gool/emu_compat.h"

#include "runtime/cpu_context.h"
#include "runtime/emuptr.h"
#include "runtime/memory.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

// Declared by the generated recompiled_out.cpp (and the stub build).
void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace ps1::gool {

namespace {
thread_local recomp_context *t_ctx = nullptr;
thread_local uint8_t *t_rdram = nullptr;

// Cached RAM base for host<->PS1 address conversion. Resolved lazily from
// emuptr_translate so it shares the exact mapping emuptr<T> uses.
inline uint8_t *ram_base() noexcept {
  return static_cast<uint8_t *>(::ps1::emuptr_translate(0x80000000u));
}
} // namespace

void emu_bind(uint8_t *rdram, recomp_context *ctx) noexcept {
  t_rdram = rdram;
  t_ctx = ctx;
}

void emu_unbind() noexcept {
  t_rdram = nullptr;
  t_ctx = nullptr;
}

recomp_context *emu_ctx() noexcept {
  assert(t_ctx && "emu_ctx() used outside a GOOL emu_bind() scope");
  return t_ctx;
}

uint32_t &emu_reg(int index) noexcept {
  recomp_context *ctx = emu_ctx();
  if (index < 32)
    return ctx->r[index];
  if (index == 32)
    return ctx->lo;
  return ctx->hi; // index == 33
}

// --- Memory primitives -------------------------------------------------

uint8_t EMU_ReadU8(uint32_t address) noexcept {
  return emu_ctx()->mem->read8(address);
}
uint16_t EMU_ReadU16(uint32_t address) noexcept {
  return emu_ctx()->mem->read16(address);
}
uint32_t EMU_ReadU32(uint32_t address) noexcept {
  return emu_ctx()->mem->read32(address);
}
int8_t EMU_ReadS8(uint32_t address) noexcept {
  return static_cast<int8_t>(emu_ctx()->mem->read8(address));
}
int16_t EMU_ReadS16(uint32_t address) noexcept {
  return static_cast<int16_t>(emu_ctx()->mem->read16(address));
}

void EMU_Write8(uint32_t address, uint8_t value) noexcept {
  emu_ctx()->mem->write8(address, value);
}
void EMU_Write16(uint32_t address, uint16_t value) noexcept {
  emu_ctx()->mem->write16(address, value);
}
void EMU_Write32(uint32_t address, uint32_t value) noexcept {
  emu_ctx()->mem->write32(address, value);
}

// --- Host pointer <-> PS1 address -------------------------------------

uint32_t EMU_Address(const void *host_ptr) noexcept {
  const auto *p = static_cast<const uint8_t *>(host_ptr);
  uint8_t *base = ram_base();
  if (p >= base && p < base + Memory::RAM_SIZE) {
    return (static_cast<uint32_t>(p - base) & (Memory::RAM_SIZE - 1)) |
           0x80000000u;
  }
  // GOOL object data lives in RAM, so this should not happen. Mirror the
  // c1c abort semantics but log instead of crashing the runtime.
  std::fprintf(stderr,
               "[GOOL] EMU_Address: host pointer %p outside bound RAM\n",
               host_ptr);
  return 0;
}

// --- Native callout ----------------------------------------------------

uint32_t EMU_Invoke(uint32_t address, int argc, ...) noexcept {
  recomp_context *ctx = emu_ctx();
  const uint32_t ra_saved = ctx->r[31]; // RA
  const uint32_t bytes = static_cast<uint32_t>(argc) * 4u;

  // Reserve the outgoing-argument area (o32: a0..a3 home + stack args).
  ctx->r[29] -= bytes; // SP

  std::va_list ap;
  va_start(ap, argc);
  for (int i = 0; i < argc; ++i) {
    const uint32_t v = va_arg(ap, uint32_t);
    if (i < 4)
      ctx->r[4 + i] = v; // A0..A3
    else
      ctx->mem->write32(ctx->r[29] + static_cast<uint32_t>(i) * 4u, v);
  }
  va_end(ap);

  // Static recomp: the target is a C++ function that returns at JR RA.
  recomp_dispatch(t_rdram, ctx, address);

  ctx->r[29] += bytes;   // restore SP
  ctx->r[31] = ra_saved; // restore RA
  return ctx->r[2];      // V0
}

// --- HI/LO mul-div helpers --------------------------------------------

void EMU_SMultiply(int32_t a, int32_t b) noexcept {
  const int64_t result = static_cast<int64_t>(a) * static_cast<int64_t>(b);
  recomp_context *ctx = emu_ctx();
  ctx->lo = static_cast<uint32_t>(result);
  ctx->hi = static_cast<uint32_t>(result >> 32);
}
void EMU_UMultiply(uint32_t a, uint32_t b) noexcept {
  const uint64_t result = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
  recomp_context *ctx = emu_ctx();
  ctx->lo = static_cast<uint32_t>(result);
  ctx->hi = static_cast<uint32_t>(result >> 32);
}
void EMU_SDivide(int32_t a, int32_t b) noexcept {
  recomp_context *ctx = emu_ctx();
  if (b) {
    ctx->lo = static_cast<uint32_t>(a / b);
    ctx->hi = static_cast<uint32_t>(a % b);
  } else {
    ctx->lo = 0xFFFFFFFFu;
    ctx->hi = static_cast<uint32_t>(a);
  }
}
void EMU_UDivide(uint32_t a, uint32_t b) noexcept {
  recomp_context *ctx = emu_ctx();
  if (b) {
    ctx->lo = a / b;
    ctx->hi = a % b;
  } else {
    ctx->lo = 0xFFFFFFFFu;
    ctx->hi = a;
  }
}

void EMU_Break(uint32_t id) noexcept {
  static bool warned = false;
  if (!warned && std::getenv("PS1_GOOL_TRACE")) {
    std::fprintf(stderr, "[GOOL] EMU_Break(0x%X) (no-op)\n", id);
    warned = true;
  }
}

} // namespace ps1::gool
