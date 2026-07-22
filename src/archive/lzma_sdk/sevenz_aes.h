/* AES-256 decryption glue for the vendored 7z reader.
 *
 * The public-domain LZMA SDK's 7zDec.c decodes LZMA/LZMA2/PPMd/BCJ/Delta but
 * intentionally omits the AES-256 coder (0x06F10701) used by `7z -p`/`-mhe`
 * encrypted archives. This tiny module adds the 7z SHA-256 key-derivation and
 * AES-256-CBC decrypt (built on the SDK's own Sha256.c + Aes.c -- still no
 * third-party crypto), plus a thread-local password so the decode path can
 * reach it without threading a parameter through the whole SDK API.
 *
 * Scope: preview only (list + read one entry). Not used for writing.
 */
#ifndef TTC_SEVENZ_AES_H
#define TTC_SEVENZ_AES_H

#include "7zTypes.h"

EXTERN_C_BEGIN

/* Set / clear the password (UTF-16LE bytes, no BOM/terminator) for subsequent
 * decode calls on the current thread. */
void Sevenz_SetPassword(const Byte *utf16le, size_t sizeBytes);
void Sevenz_ClearPassword(void);
int  Sevenz_HasPassword(void);

/* "An AES coder was encountered during the last decode" flag (thread-local).
 * The wrapper resets it before an open/extract and reads it after to tell an
 * encryption failure (-> NeedPassword/WrongPassword) from a generic error. */
void Sevenz_ResetAesFlag(void);
int  Sevenz_AesSeen(void);

/* Decrypt `data` (dataSize bytes, a multiple of 16) in place using the 7z AES
 * coder props (salt/iv/cycles) and the current thread's password. Returns 1 on
 * success, 0 on failure (no password set, or malformed props). Always sets the
 * AES-seen flag. The decrypted bytes are the *compressed* stream that the next
 * coder (LZMA2/...) then decompresses. */
int Sevenz_AesDecrypt(const Byte *props, size_t propsSize, Byte *data, size_t dataSize);

EXTERN_C_END

#endif
