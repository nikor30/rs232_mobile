# mldsa-native (vendored)

ML-DSA nach FIPS 204 — nur das Prüfen von Signaturen, für das Firmware-Update über die Leitstelle (`server/KONZEPT.md` §4.7).

- Quelle: <https://github.com/pq-code-package/mldsa-native>, Tag `v2.0.0`, Commit `834a90d5e846ffa1e1611bd24e160bb2e9b86d35`
- Lizenz: Apache-2.0 ODER ISC ODER MIT (`LICENSE`)
- Übernommen: `mldsa/mldsa_native.h`, `mldsa/mldsa_native_config.h` und `mldsa/src/` (hier `src/mldsa/`)
- Weggelassen: die Assembler-Backends (`native/aarch64`, `native/x86_64`, auch unter `fips202/native`), die Ein-Datei-Fassung (`mldsa_native.c`, `mldsa_native_asm.S`), Tests, Beweise, Beispiele
- **Einzige Änderung:** in `src/mldsa/mldsa_native_config.h` sind gesetzt: `MLD_CONFIG_PARAMETER_SET 65`, `MLD_CONFIG_NO_KEYPAIR_API`, `MLD_CONFIG_NO_SIGN_API`, `MLD_CONFIG_NO_RANDOMIZED_API`, `MLD_CONFIG_REDUCE_RAM` (jeweils mit dem Vermerk `rs232_mobile`)

Das Gerät erzeugt keine Schlüssel und signiert nichts; signiert wird auf dem Rechner des Besitzers mit OpenSSL (`firmware/tools/fw_sign.py`). Eingebunden wird so:

```c
#include <mldsa_native.h>
// PQCP_MLDSA_NATIVE_MLDSA65_verify(sig, msg, msglen, ctx, ctxlen, pk)  ->  0 = gültig
```
