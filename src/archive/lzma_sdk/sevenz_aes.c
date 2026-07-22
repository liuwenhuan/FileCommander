/* See sevenz_aes.h. Uses the vendored SDK's own Sha256.c + Aes.c. */

#include "sevenz_aes.h"

#include "Aes.h"
#include "Sha256.h"

#include <stdint.h>
#include <string.h>

/* Password + state are per-thread: preview decodes run on worker threads and
 * two archives may be open at once, so a shared global would race. */
static __thread Byte   g_pw[2048];
static __thread size_t g_pwSize = 0;
static __thread int    g_pwSet = 0;
static __thread int    g_aesSeen = 0;

/* AesGenTables() fills process-global tables; run once, before threads start. */
static void ttc_aes_init(void) __attribute__((constructor));
static void ttc_aes_init(void) { AesGenTables(); }

void Sevenz_SetPassword(const Byte *utf16le, size_t sizeBytes)
{
  if (sizeBytes > sizeof(g_pw))
    sizeBytes = sizeof(g_pw);
  if (sizeBytes && utf16le)
    memcpy(g_pw, utf16le, sizeBytes);
  g_pwSize = sizeBytes;
  g_pwSet = 1;
}

void Sevenz_ClearPassword(void)
{
  memset(g_pw, 0, sizeof(g_pw));
  g_pwSize = 0;
  g_pwSet = 0;
}

int  Sevenz_HasPassword(void) { return g_pwSet; }
void Sevenz_ResetAesFlag(void) { g_aesSeen = 0; }
int  Sevenz_AesSeen(void) { return g_aesSeen; }

/* 7z key derivation: repeated SHA-256 over salt || password || counter64(LE). */
static void CalcKey(const Byte *salt, size_t saltSize, unsigned numCyclesPower, Byte key[32])
{
  if (numCyclesPower == 0x3F) {
    /* Special "no hashing" mode: key = salt || password, zero-padded. */
    size_t pos = 0;
    for (; pos < saltSize && pos < 32; pos++)
      key[pos] = salt[pos];
    for (size_t i = 0; i < g_pwSize && pos < 32; i++, pos++)
      key[pos] = g_pw[i];
    for (; pos < 32; pos++)
      key[pos] = 0;
    return;
  }

  {
    CSha256 sha;
    Byte counter[8];
    UInt64 rounds = (UInt64)1 << numCyclesPower;
    UInt64 r;
    memset(counter, 0, sizeof(counter));
    Sha256_Init(&sha);
    for (r = 0; r < rounds; r++) {
      if (saltSize)
        Sha256_Update(&sha, salt, saltSize);
      if (g_pwSize)
        Sha256_Update(&sha, g_pw, g_pwSize);
      Sha256_Update(&sha, counter, 8);
      /* increment 64-bit little-endian counter */
      {
        int i;
        for (i = 0; i < 8; i++)
          if (++counter[i] != 0)
            break;
      }
    }
    Sha256_Final(&sha, key);
  }
}

int Sevenz_AesDecrypt(const Byte *props, size_t propsSize, Byte *data, size_t dataSize)
{
  unsigned numCyclesPower, saltSize, ivSize, i;
  size_t pos;
  const Byte *salt;
  Byte b0, iv[AES_BLOCK_SIZE], key[32];
  /* 16-byte-aligned AES state: iv(4 words) + keyMode + round keys. */
  UInt32 aesbuf[AES_NUM_IVMRK_WORDS + 4];
  UInt32 *aes;

  g_aesSeen = 1;
  if (!g_pwSet)
    return 0;
  if (dataSize == 0)
    return 1;
  if ((dataSize & (AES_BLOCK_SIZE - 1)) != 0)
    return 0;
  if (propsSize < 1)
    return 0;

  /* Coder props layout (7zAes): b0 low6 = numCyclesPower; b0 bit7/bit6 =
   * salt/iv present; if either set, b1 high4 += saltSize, low4 += ivSize;
   * then salt[], iv[]. iv is zero-padded to a full AES block. */
  b0 = props[0];
  numCyclesPower = b0 & 0x3F;
  saltSize = (b0 >> 7) & 1;
  ivSize   = (b0 >> 6) & 1;
  pos = 1;
  if ((b0 & 0xC0) != 0) {
    Byte b1;
    if (propsSize < 2)
      return 0;
    b1 = props[1];
    saltSize += (unsigned)(b1 >> 4);
    ivSize   += (unsigned)(b1 & 0x0F);
    pos = 2;
  }
  if (ivSize > AES_BLOCK_SIZE)
    return 0;
  if (propsSize < pos + saltSize + ivSize)
    return 0;

  salt = props + pos;
  memset(iv, 0, sizeof(iv));
  for (i = 0; i < ivSize; i++)
    iv[i] = props[pos + saltSize + i];

  CalcKey(salt, saltSize, numCyclesPower, key);

  aes = aesbuf + (((0 - (size_t)(uintptr_t)aesbuf) & (AES_BLOCK_SIZE - 1)) / sizeof(UInt32));
  Aes_SetKey_Dec(aes + 4, key, 32);
  AesCbc_Init(aes, iv);
  /* Use the reference C decoder, not g_AesCbc_Decode: AesGenTables() may point
   * that at the AES-NI/VAES path, and the AVX2 VAES-256 variant reads the round
   * keys as 256-bit lanes while Aes_SetKey_Dec lays them out 128-bit-wide, which
   * overruns the state. The C path is correct for this layout and fast enough
   * for preview-sized payloads. */
  AesCbc_Decode(aes, data, dataSize / AES_BLOCK_SIZE);
  return 1;
}
