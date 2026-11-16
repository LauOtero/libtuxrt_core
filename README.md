# libtuxrt_core

Núcleo en C17 de TuxRT: planificación de tiempo real, movimiento, integridad,
resiliencia (KARES/USS/WAL), transporte industrial y protocolos de red
(EtherCAT y EtherTux). Se compila como `libtuxrt_core.so` y expone una ABI C
estable con prefijo `tuxrt_` que consumen Mojo, el SDK y los proveedores
externos (p. ej. `ChronosRT`).

## Estructura

El código se organiza por **subsistema**, con las fuentes y las cabeceras
espejadas (`src/<sistema>/…` ↔ `include/<sistema>/…`):

```
libtuxrt_core/
├── CMakeLists.txt
├── src/
│   ├── core/          scheduler, sirc, tuxrt_mutex, build_system, error_boundary,
│   │                  observability, config, provider, dependency, autocfg
│   ├── motion/        motion, motion_lookahead,
│   │                  trapq, stepcompress, steppersync, input_shaper
│   ├── parser/        gcode_parser.c
│   ├── compression/   ksc.c, jlpp.c
│   ├── integrity/     ultracrc.c
│   ├── accel/         hwdet.c, accel_runtime.c(+h), accel_kernels.h, hwaccel.c
│   ├── resilience/    kares/, uss/, wal/, job_queue/, tuxatlas/
│   ├── transport/     transport(+_usb/_uart/_can/_spi/_i2c/_ethernet),
│   │                  transport_websocket.c (cliente WebSocket asíncrono),
│   │                  phsync
│   ├── protocol/      protocol.c, ethercat/, ethertux/
│   └── ipc/           channel.c, zckb_shm.c, msgblock.c
├── test/              23 suites (ver «Tests»)
└── ../sdk/include/    tuxrt_sdk.h, tuxrt_provider.h, tuxrt_ethertux.h,
                       tuxrt_hwaccel.h, tuxrt_protocol.h  (ABI pública)
```

## Subsistemas

| Subsistema | Responsabilidad |
| --- | --- |
| `core` | Planificador de tiempo real, SIRC, mutex prioritario, metadatos de build, frontera de errores, configuración, contrato de proveedores (`dlopen`) y autoconfiguración. |
| `motion` | Cinemática (analítica y cargada por plugin), planificación de movimiento, lookahead, cola de trapezoides, generación de pasos y *input shaping*. |
| `parser` | Parser de G-code vectorizado. |
| `compression` | KSC (7 algoritmos según tipo de mensaje) y JLPP (DPCM + Golomb-Rice). |
| `integrity` | `ultracrc`: CRC-16/CCITT, CRC-32/MPEG-2, CRC-64/ECMA y **CRC-32C** con dispatch HW. |
| `accel` | Detección de ISA (`/proc/self/auxv`), runtime SIMD y HAL de aceleración. |
| `resilience` | KARES (9 fases de recuperación), USS (snapshot A/B con ECC BCH), WAL (11 tipos), Job Queue (SQLite) y TuxAtlas (rollback a Golden Image). |
| `transport` | Enlaces físicos USB/UART/CAN/SPI/I2C/Ethernet, cliente WebSocket asíncrono no bloqueante y sincronización de fase. |
| `protocol` | Selección EtherCAT/EtherTux y maestros de red. |
| `ipc` | Canal SPSC wait-free, memoria compartida zero-copy y framing V2. |

### Aceleración por hardware (`accel` + `integrity`)

`tuxrt_hwaccel.h` ofrece CRC-32C, GF(2^8) y FEC Reed-Solomon. El CRC-32C está
**despachado en una sola implementación** (`ultracrc_crc32c_raw`): instrucción
`crc32c*` en ARMv8.1, `crc32` de SSE4.2 en x86-64 y escalar *slice-by-8* como
respaldo (necesario en Cortex-A7, p. ej. Banana Pi M2 Zero). Se elige CRC-32C y
no CRC-32 IEEE porque es el único polinomio acelerado por instrucción en ambas
arquitecturas. Nunca se emite código SIMD sin comprobar antes la capacidad del
host (`tuxrt_hwdet_has_feature`), evitando `SIGILL`.

### Protocolos de red (`protocol`)

- `protocol.c` — selección con `TUXRT_PROTOCOL=ethercat|ethertux`
  (por defecto EtherCAT). También acepta configuración por API.
- `protocol/ethercat/` — maestro EtherCAT sobre `AF_PACKET`/`0x88A4` con
  integración del proveedor `ChronosRT` por carga dinámica.
- `protocol/ethertux/` — EtherTux v0.1.0 (`0x88B5`): cabecera de 26 B con
  `State` (ET-ESM), `ET-WKC` y `FlowCount`, CRC-32C, FEC Reed-Solomon
  RS(10,8)/RS(16,8), QoS P0–P5 con token buckets jerárquicos y LAPM,
  planificador *time-triggered* de superframe de 1 ms, socket con pacing
  `SO_TXTIME`, nodos por MAC y *heartbeat*. Módulos añadidos en esta versión:
  `et_esm` (ciclo de vida
  del nodo), `et_sync` (ET-Sync y Monitor de Ventana), `et_topology` (registro,
  ET-WKC, descubrimiento y Hot Connect), `et_mbx` (mailbox fiable),
  `et_flowagg` (agregación de flujos), `et_frm` (redundancia y deduplicación) y
  `et_aalid` (ID local determinista: MAC LAA `02:00:00:00:ID[15:8]:ID[7:0]`,
  filtro de recepción por ID, admisión con autoridad central, expiración por
  heartbeat y persistencia `{MAC -> ID}`).

### Transporte WebSocket (Cliente asíncrono)

`transport_websocket.c` — Cliente WebSocket no bloqueante basado en **libcurl**
(`websockets.h`). Uso típico con event loop:

```c
tuxrt_ws_client_t* ws = NULL;

/* 1. Iniciar conexión (no bloquea) */
tuxrt_core_ws_connect("ws://192.168.1.100:7125", 5000, &ws);

/* 2. Service loop hasta CONNECTED o error */
while (tuxrt_core_ws_get_state(ws) != TUXRT_WS_CONNECTED) {
    tuxrt_err_t err = tuxrt_core_ws_service(ws, 10);
    if (err != TUXRT_OK && err != TUXRT_ERR_AGAIN) {
        /* manejar error */ break;
    }
}

/* 3. Enviar/recibir frames binarios */
uint8_t payload[] = { /* datos */ };
tuxrt_core_ws_send(ws, payload, sizeof(payload));

tuxrt_ws_frame_t frame;
tuxrt_core_ws_recv(ws, &frame, 100);
/* frame.data, frame.len, frame.flags, frame.timestamp_ns */

/* 4. Limpiar al salir */
tuxrt_core_ws_disconnect(ws);
```

| Función | Propósito |
| --- | --- |
| `tuxrt_core_ws_connect(url, timeout_ms, &out)` | Inicia handshake sin bloquear |
| `tuxrt_core_ws_service(client, timeout_ms)` | Procesa eventos; `TUXRT_ERR_AGAIN` si nada disponible |
| `tuxrt_core_ws_send(client, data, len)` | Envío binary/texto |
| `tuxrt_core_ws_recv(client, &frame, timeout_ms)` | Recepción no bloqueante |
| `tuxrt_core_ws_get_state(client)` | `IDLE → CONNECTING → CONNECTED → CLOSING → CLOSED` |
| `tuxrt_core_ws_last_error(client)` | Último error ocurrido |
| `tuxrt_core_ws_disconnect(client)` | Cierra y libera recursos (`NULL`-safe) |

El struct `tuxrt_ws_frame_t` está alineado para consumo directo desde Mojo FFI
(sin copias intermedias). Los flags (`TUXRT_WS_FLAG_BINARY`, etc.) reflejan los
valores de `CURLWS_*`.

---

## Compilación

```bash
cmake -S libtuxrt_core -B libtuxrt_core/build -DCMAKE_BUILD_TYPE=Release
cmake --build libtuxrt_core/build -j"$(nproc)"
```

Dependencias: `liburing`, `openssl`, `zlib`, `libcurl >= 7.88` (con `websockets.h`),
`json-c` (vía `pkg-config`). Las rutas SIMD (`-mavx2`, `-mavx512*`, `-msse4.2`)
se habilitan solo si la CPU del host las declara, y el código acelerado se
compila con `__attribute__((target(...)))`.

Los objetivos `tuxrt_core_asan` y `tuxrt_core_tsan` generan variantes con
sanitizadores.

## Tests

```bash
ctest --test-dir libtuxrt_core/build --output-on-failure
```

22 suites más la de EtherTux v0.1.0: `scheduler`, `motion`, `wal`,
`transport`, `phsync`, `ultracrc`, `gcode_parser`, `channel`, `zckb_shm`,
`msgblock`, `trapq`, `stepcompress`, `steppersync`, `input_shaper`, `ksc`,
`jlpp`, `kares`, `kares_perf`, `ethertux`, `ethertux_v2`, `hwaccel`, `aalid`.

## ABI

- Prefijo `tuxrt_`, exportación explícita con `TUXRT_SDK_API`; el resto del
  árbol se compila con `-fvisibility=hidden`./mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/TuxedoRT-Mojo/libtuxrt_core
- Códigos de error `tuxrt_err_t` (negativos; `0 = TUXRT_OK`). Los nuevos
  estados de EtherTux son `TUXRT_ERR_BACKPRESSURE` (-1028) y
  `TUXRT_ERR_DROPPED` (-1029); el transporte EtherTux es
  `TUXRT_TRANSPORT_ETHERTUX`.
- La ABI es aditiva: solo se añade al final de `tuxrt_sdk.h`.
- El binario actual exporta 287+ símbolos `tuxrt_*`.
- Nuevos tipos en `tuxrt_ethertux.h`: `tuxrt_ws_client_t`, `tuxrt_ws_state_t`,
  `tuxrt_ws_flag_t`, `tuxrt_ws_frame_t`.
- Nuevas funciones SDK: `tuxrt_core_ws_*` (conect, disconnect, service, send,
  recv, get_state, last_error).

## Convenciones

- C17 estricto (`-Wpedantic -Wshadow -Wcast-align=strict -Wmissing-prototypes`,
  entre otras) y `-fno-common`.
- Los structs que se serializan (KARES, USS, tramas) van *packed* y con
  `_Static_assert` de tamaño.
- Un `.c` incluye sus cabeceras por nombre (`#include "trapq.h"`), nunca con
  rutas relativas al árbol: las rutas se resuelven con los
  `target_include_directories` de CMake.
  
