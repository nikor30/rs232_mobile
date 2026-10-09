# mlkem-native (vendored)

ML-KEM nach FIPS 203 für den Kanal zur Leitstelle (`server/KONZEPT.md`).

- Quelle: <https://github.com/pq-code-package/mlkem-native>, Tag `v1.0.0`, Commit `048fc2a7a7b4ba0ad4c989c1ac82491aa94d5bfa`
- Lizenz: Apache-2.0 ODER ISC ODER MIT (`LICENSE`)
- Übernommen: `mlkem/mlkem_native.h` und `mlkem/src/` (hier `src/mlkem/`)
- Weggelassen: die Assembler-Backends für AArch64 und x86-64 (`native/aarch64`, `native/x86_64`), Tests, Beweise, Beispiele
- **Einzige Änderung:** in `src/mlkem/config.h` ist `MLK_CONFIG_CUSTOM_RANDOMBYTES` gesetzt; Zufall kommt aus `c2_randombytes()` der Firmware (das ESP32-SDK bringt mit libsodium schon ein `randombytes()` mit)

Parametersatz ist ML-KEM-768 (Vorgabe der `config.h`). Eingebunden wird so:

```c
#define MLK_CONFIG_API_PARAMETER_SET 768
#define MLK_CONFIG_API_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM768
#include <mlkem_native.h>
```
