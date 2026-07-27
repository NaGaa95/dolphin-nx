// Copyright 2018 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Random.h"

#ifdef __SWITCH__
#include <switch.h>

namespace Common::Random
{
void Generate(void* buffer, std::size_t size)
{
  randomGet(buffer, size);
}
}  // namespace Common::Random

#else
// Non-Switch platforms use mbedtls

#include <mbedtls/entropy.h>
#include <mbedtls/hmac_drbg.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common::Random
{
class EntropySeededPRNG final
{
public:
  EntropySeededPRNG()
  {
    mbedtls_entropy_init(&m_entropy);
    mbedtls_hmac_drbg_init(&m_context);
    const int ret = mbedtls_hmac_drbg_seed(&m_context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                           mbedtls_entropy_func, &m_entropy, nullptr, 0);
    ASSERT(ret == 0);
  }

  ~EntropySeededPRNG()
  {
    mbedtls_hmac_drbg_free(&m_context);
    mbedtls_entropy_free(&m_entropy);
  }

  void Generate(void* buffer, std::size_t size)
  {
    const int ret = mbedtls_hmac_drbg_random(&m_context, static_cast<u8*>(buffer), size);
    ASSERT(ret == 0);
  }

private:
  mbedtls_entropy_context m_entropy;
  mbedtls_hmac_drbg_context m_context;
};

static thread_local EntropySeededPRNG s_esprng;

void Generate(void* buffer, std::size_t size)
{
  s_esprng.Generate(buffer, size);
}
}  // namespace Common::Random

#endif
