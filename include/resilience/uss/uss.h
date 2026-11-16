/**
 * uss.h — Universal State Snapshot para KARES
 *
 * Snapshot atómico y consistente del estado completo de la máquina:
 *   - Joint space (posición de cada eje)
 *   - Task space (posición de cada herramienta)
 *   - Actuadores (husillo, láser, refrigerante)
 *   - Sistemas de coordenadas (WCS, G43, G92)
 *   - Contexto del job (cursor G-code multi-toolhead)
 *   - Cinemática (tipo, parámetros, singularidades)
 *
 * Protegido con CRC32 + ECC BCH(32768,32640) para corrección de hasta 16 bits.
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_USS_H
#define TUXRT_USS_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  Constantes
 * ============================================================================ */

/**
 * Número máximo de ejes soportados
 * X, Y, Z, A, B, C, E0-E5, S (spindle), L (laser)
 */
#define USS_MAX_AXES 12

/**
 * Número máximo de herramientas soportadas
 * IDEX, MMU, toolchanger
 */
#define USS_MAX_TOOLS 8

/**
 * Número máximo de sistemas de coordenadas
 * G54-G59 + G59.1-G59.3
 */
#define USS_MAX_WCS 9

/**
 * Magic number para identificación USS
 */
#define USS_MAGIC 0x55535332  /* "USS2" */

/**
 * Versión de estructura USS
 * 0x00020000 = versión 2.0
 */
#define USS_VERSION 0x00020000

/**
 * Tamaño de datos USS (sin ECC)
 * Aproximadamente 4080 bytes para 12 ejes y 8 herramientas
 */
#define USS_DATA_SIZE 4080

/**
 * Tamaño de paridad ECC
 * 16 bytes para BCH(32768,32640)
 */
#define USS_ECC_SIZE 16

/**
 * Tamaño total del bloque USS (datos + ECC)
 * 4096 bytes = 4 KB
 */
#define USS_BLOCK_SIZE 4096

/**
 * Número de secuencia inicial
 */
#define USS_SEQUENCE_INITIAL 0

/**
 * Flags USS
 */
#define USS_FLAG_VALID        0x00000001
#define USS_FLAG_ECC_APPLIED  0x00000002
#define USS_FLAG_CRC_VALID    0x00000004
#define USS_FLAG_HOMED        0x00000008
#define USS_FLAG_ACTIVE       0x00000010

/* ============================================================================
 *  Tipos de ejes y herramientas
 * ============================================================================ */

/**
 * Tipo de eje
 */
typedef enum {
    USS_AXIS_LINEAR = 0,    /* Eje lineal (X, Y, Z) */
    USS_AXIS_ROTARY = 1,    /* Eje rotativo (A, B, C) */
    USS_AXIS_EXTRUDER = 2,  /* Extrusor (E) */
    USS_AXIS_SPINDLE = 3,   /* Husillo (S) */
    USS_AXIS_LASER = 4      /* Láser (L) */
} uss_axis_type_t;

/**
 * Tipo de herramienta
 */
typedef enum {
    USS_TOOL_EXTRUDER = 0,  /* Extrusor FDM */
    USS_TOOL_SPINDLE = 1,   /* Husillo CNC */
    USS_TOOL_LASER = 2      /* Láser */
} uss_tool_type_t;

/**
 * Estado de herramienta
 */
typedef enum {
    USS_TOOL_IDLE = 0,      /* Inactiva */
    USS_TOOL_ACTIVE = 1,    /* Activa */
    USS_TOOL_PARKED = 2,    /* Estacionada */
    USS_TOOL_CHANGING = 3,  /* En cambio */
    USS_TOOL_FAULT = 4      /* Fallo */
} uss_tool_state_enum_t;

/**
 * Tipo de cinemática
 */
typedef enum {
    USS_KIN_CARTESIAN = 0,      /* Cartesian (CoreXY, H-Bot, etc.) */
    USS_KIN_DELTA = 1,          /* Delta */
    USS_KIN_SCARA = 2,          /* SCARA */
    USS_KIN_COREXY = 3,         /* CoreXY */
    USS_KIN_HBOT = 4,           /* H-Bot */
    USS_KIN_I3 = 5,             /* Prusa i3 */
    USS_KIN_5AXIS_TABLE = 6,    /* 5-axis table-table */
    USS_KIN_5AXIS_HEAD = 7,     /* 5-axis head-table */
    USS_KIN_5AXIS_HEADHEAD = 8, /* 5-axis head-head */
    USS_KIN_HEXAPOD = 9,        /* Hexapod/Stewart */
    USS_KIN_POLAR = 10,         /* Polar */
    USS_KIN_CYLINDRICAL = 11    /* Cylindrical */
} uss_kinematics_type_t;

/**
 * Sub-tipo de cinemática
 */
typedef enum {
    USS_KIN_SUB_DEFAULT = 0,        /* Default */
    USS_KIN_SUB_TABLE_TABLE = 0,    /* 5-axis: mesa rotativa en mesa */
    USS_KIN_SUB_HEAD_TABLE = 1,     /* 5-axis: cabezal rotativo en mesa */
    USS_KIN_SUB_HEAD_HEAD = 2,      /* 5-axis: cabezal rotativo en cabezal */
    USS_KIN_SUB_ELBOW_UP = 3,       /* SCARA: codo arriba */
    USS_KIN_SUB_ELBOW_DOWN = 4      /* SCARA: codo abajo */
} uss_kinematics_sub_type_t;

/**
 * Estado de singularidad
 */
typedef enum {
    USS_SINGULARITY_OK = 0,     /* Sin singularidad */
    USS_SINGULARITY_SOFT = 1,   /* Singularidad suave (cerca) */
    USS_SINGULARITY_HARD = 2,   /* Singularidad dura (en) */
    USS_SINGULARITY_DEVIATE = 3 /* Desviación detectada */
} uss_singularity_state_t;

/* ============================================================================
 *  Estructuras de datos
 * ============================================================================ */

/**
 * Cabecera USS (48 bytes)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* "USS2" */
    uint32_t version;            /* 0x00020000 */
    uint64_t sequence;           /* Monotónico */
    uint64_t timestamp_ns;       /* Timestamp en nanosegundos */
    uint32_t config_hash;        /* CRC32 de MachineConfig */
    uint32_t kinematics_hash;    /* CRC32 de parámetros del solver */
    uint32_t axis_count;         /* Número de ejes */
    uint32_t tool_count;         /* Número de herramientas */
    uint32_t active_tool;        /* Índice de herramienta activa */
    uint32_t flags;              /* USS_FLAG_* */
    uint32_t payload_len;        /* Longitud útil */
    uint32_t crc32_header;       /* CRC32 de la cabecera */
} uss_header_t;

/**
 * Sección de cinemática (80 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t type;                /* uss_kinematics_type_t */
    uint8_t sub_type;            /* uss_kinematics_sub_type_t */
    uint8_t singularity_state;   /* uss_singularity_state_t */
    uint8_t reserved;
    double params[16];           /* Parámetros: f,e,rf,re,L1,L2,d,pivot[3],tool_length... */
    double eps_hard;             /* Umbral singularidad dura */
    double eps_soft;             /* Umbral singularidad suave */
} uss_kinematics_t;

/**
 * Estado de eje (96 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t axis_type;           /* uss_axis_type_t */
    uint8_t homed;               /* 1 si está homed */
    uint8_t wrap_mode;           /* 0=bounded, 1=continuous (rotativos) */
    uint8_t fault;               /* Bitmask: step_loss, following_error, overtemp */
    
    double joint_position;       /* mm / grados / mm filamento / RPM / % */
    double joint_velocity;
    double joint_accel;
    double joint_jerk;
    
    double backlash_comp;        /* Compensación de backlash */
    double screw_comp;           /* Compensación de tornillo */
    double thermal_comp;         /* Compensación térmica */
    
    double min_limit;            /* Límite mínimo */
    double max_limit;            /* Límite máximo */
    
    double actuator_setpoint;    /* RPM objetivo o % potencia */
    double actuator_ramp;        /* 0.0..1.0 */
    uint8_t actuator_dir;        /* 0=CW, 1=CCW, 2=off */
    uint8_t coolant;             /* 0=off, 1=flood, 2=mist */
    uint16_t reserved16;
} uss_axis_state_t;

/**
 * Estado de herramienta - Extrusor (48 bytes)
 */
typedef struct __attribute__((packed)) {
    double e_position;           /* Posición de filamento (mm) */
    double pressure_adv;         /* Advance de presión */
    double flow_rate;            /* Tasa de flujo */
    double retract_state;        /* Estado de retracción */
    double filament_diam;        /* Diámetro de filamento (mm) */
    double nozzle_diam;          /* Diámetro de boquilla (mm) */
} uss_extruder_state_t;

/**
 * Estado de herramienta - Husillo (24 bytes)
 */
typedef struct __attribute__((packed)) {
    double rpm_target;           /* RPM objetivo */
    double rpm_actual;           /* RPM actual */
    double coolant_state;        /* Estado de refrigerante */
} uss_spindle_state_t;

/**
 * Estado de herramienta - Láser (16 bytes)
 */
typedef struct __attribute__((packed)) {
    double power_pct;            /* Potencia en % */
    double pwm_freq;             /* Frecuencia PWM (Hz) */
    uint8_t mode;                /* Modo: 0=continuous, 1=pulse */
    uint8_t reserved[7];
} uss_laser_state_t;

/**
 * Estado de herramienta (200 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t tool_type;           /* uss_tool_type_t */
    uint8_t state;               /* uss_tool_state_enum_t */
    uint8_t active_axis_mask;    /* Máscara de ejes activos */
    uint8_t reserved_hdr;        /* Padding header */
    
    double tool_offset[3];       /* Offset de herramienta (G43) */
    
    uint32_t line_number;        /* Línea actual de G-code */
    uint32_t layer_number;       /* Capa actual */
    double param_offset;         /* Fracción del movimiento (0.0..1.0) */
    
    /* Estado específico por tipo */
    union {
        uss_extruder_state_t extruder;
        uss_spindle_state_t spindle;
        uss_laser_state_t laser;
    } u;
    
    double task_position[6];     /* x, y, z, a, b, c */
    double task_velocity[6];
    
    uint32_t crc32_tool;         /* CRC32 de esta herramienta */
    
    /* Espacio para expansión */
    uint8_t reserved_exp[8];
} uss_tool_state_t;

/**
 * Sistemas de coordenadas (184 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t active_wcs;          /* WCS activo (0-8) */
    uint8_t g92_active;          /* G92 activo */
    uint8_t plane;               /* G17=XY, G18=XZ, G19=YZ */
    uint8_t units;               /* 0=mm, 1=inch */
    uint8_t feed_mode;           /* G93,G94,G95 */
    uint8_t reserved[3];
    
    double wcs[USS_MAX_WCS][6];  /* G54-G59 + G59.1-G59.3 */
    double g92_offset[6];        /* Offset G92 */
    double machine_offset[6];    /* Offset de máquina */
    
    uint32_t crc32_coord;        /* CRC32 de coordenadas */
} uss_coord_systems_t;

/**
 * Contexto del job (32 bytes)
 */
typedef struct __attribute__((packed)) {
    uint64_t job_id;             /* ID del job */
    uint32_t global_line;        /* Línea global */
    uint32_t global_layer;       /* Capa global */
    uint32_t gcode_checksum;     /* CRC32 del archivo G-code */
    uint32_t gcode_total_lines;  /* Total de líneas */
    uint32_t flags;
    uint32_t crc32_job;          /* CRC32 del job */
} uss_job_context_t;

/**
 * Snapshot USS completo (~4080 bytes)
 */
typedef struct __attribute__((packed)) {
    uss_header_t header;                    /* 56 bytes */
    uss_kinematics_t kinematics;            /* 148 bytes */
    uss_coord_systems_t coord;              /* 540 bytes */
    uss_job_context_t job;                  /* 32 bytes */
    uss_axis_state_t axes[USS_MAX_AXES];    /* 96 × 12 = 1152 bytes */
    uss_tool_state_t tools[USS_MAX_TOOLS];  /* 200 × 8 = 1600 bytes */
    uint8_t padding[552];                   /* Relleno hasta USS_DATA_SIZE (4080) */
} uss_snapshot_t;

/* El snapshot debe caber exactamente en el bloque de datos USS (ECC 4 KB) */
_Static_assert(sizeof(uss_snapshot_t) == USS_DATA_SIZE,
               "uss_snapshot_t debe medir USS_DATA_SIZE bytes");

/**
 * Bloque USS completo con ECC (4096 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t data[USS_DATA_SIZE];   /* Datos USS (4080 bytes) */
    uint8_t ecc[USS_ECC_SIZE];     /* ECC BCH (16 bytes) */
} uss_block_t;

/**
 * Contexto USS
 */
typedef struct uss_ctx {
    char* checkpoint_path_a;       /* Ruta buffer A */
    char* checkpoint_path_b;       /* Ruta buffer B */
    uint32_t checkpoint_interval;  /* Intervalo en segundos */
    uint64_t last_sequence;        /* Última secuencia escrita */
    uint32_t active_buffer;        /* 0=A, 1=B */
    bool initialized;
    
    /* Contexto ECC */
    void* ecc_ctx;                 /* uss_ecc_ctx_t* */
} uss_ctx_t;

/* ============================================================================
 *  API Pública
 * ============================================================================ */

/**
 * Crear contexto USS
 *
 * @param checkpoint_path_a Ruta para buffer A
 * @param checkpoint_path_b Ruta para buffer B
 * @param interval_secs Intervalo entre checkpoints (0=on-demand)
 * @param out_ctx Contexto resultante
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_create(
    const char* checkpoint_path_a,
    const char* checkpoint_path_b,
    uint32_t interval_secs,
    uss_ctx_t** out_ctx
);

/**
 * Inicializar ECC para USS
 *
 * @param ctx Contexto USS
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_init_ecc(uss_ctx_t* ctx);

/**
 * Escribir snapshot USS a disco
 *
 * @param ctx Contexto USS
 * @param snapshot Snapshot a escribir
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_write(
    uss_ctx_t* ctx,
    const uss_snapshot_t* snapshot
);

/**
 * Leer snapshot USS desde disco
 *
 * @param ctx Contexto USS
 * @param out_snapshot Snapshot leído
 * @param out_corrected Salida: true si se corrigieron errores con ECC
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_read(
    uss_ctx_t* ctx,
    uss_snapshot_t* out_snapshot,
    bool* out_corrected
);

/**
 * Cargar snapshot anterior (buffer alternativo)
 *
 * @param ctx Contexto USS
 * @param out_snapshot Snapshot anterior
 * @param out_corrected Salida: true si se corrigieron errores
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_load_previous(
    uss_ctx_t* ctx,
    uss_snapshot_t* out_snapshot,
    bool* out_corrected
);

/**
 * Verificar si hay snapshot disponible
 *
 * @param ctx Contexto USS
 * @param out_available Salida: true si hay snapshot
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_exists(
    uss_ctx_t* ctx,
    bool* out_available
);

/**
 * Obtener información del snapshot
 *
 * @param ctx Contexto USS
 * @param out_sequence Secuencia del último snapshot
 * @param out_timestamp Timestamp del último snapshot
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_get_info(
    const uss_ctx_t* ctx,
    uint64_t* out_sequence,
    uint64_t* out_timestamp_ns
);

/**
 * Destruir contexto USS
 *
 * @param ctx Contexto USS
 */
void tuxrt_uss_destroy(uss_ctx_t* ctx);

/**
 * Calcular CRC32 de datos
 *
 * @param data Datos
 * @param len Longitud
 * @return CRC32 calculado
 */
uint32_t tuxrt_uss_crc32(const void* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_USS_H */
