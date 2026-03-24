# High-Fidelity Full-Stack SSD Simulator (HFSSS) Low-Level Design Document

**Document Name**: Hardware Abstraction Layer (HAL) Low-Level Design
**Document Version**: V2.0
**Creation Date**: 2026-03-08
**Design Phase**: V2.0 (Enterprise Extended)
**Classification**: Internal

---

## Revision History

| Version | Date | Author | Description |
|---------|------|--------|-------------|
| V0.1 | 2026-03-08 | Architecture Team | Initial draft |
| V1.0 | 2026-03-08 | Architecture Team | Official release |
| V2.0 | 2026-03-23 | Architecture Team | English translation with enterprise SSD extensions (supercapacitor model, crypto engine API, thermal sensor API) |

---

## Table of Contents

1. [Overview](#1-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Detailed Design](#3-data-structure-detailed-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Detailed Design](#5-function-interface-detailed-design)
6. [Supercapacitor Model Detailed Design](#6-supercapacitor-model-detailed-design)
7. [Crypto Engine API](#7-crypto-engine-api)
8. [Thermal Sensor Read API](#8-thermal-sensor-read-api)
9. [Architecture Decision Records](#9-architecture-decision-records)
10. [Memory Budget Analysis](#10-memory-budget-analysis)
11. [Latency Budget Analysis](#11-latency-budget-analysis)
12. [References](#12-references)
13. [Appendix: Cross-References to HLD](#appendix-cross-references-to-hld)

---

## 1. Overview

### 1.1 Module Positioning and Responsibilities

The Hardware Abstraction Layer (HAL) provides abstract interfaces to the media layer, isolating the upper-layer FTL from differences in the underlying media implementation. It serves as the hardware driver layer for NAND, NOR, power management, and crypto engine access.

### 1.2 Relationships with Other Modules

- **Upstream**: FTL layer and Controller thread call HAL APIs
- **Downstream**: HAL dispatches commands to the Media Threads module
- **Lateral**: Common Services layer provides RTOS primitives used by HAL

### 1.3 Terminology

| Term | Definition |
|------|-----------|
| HAL | Hardware Abstraction Layer |
| NAND | NAND Flash memory |
| NOR | NOR Flash memory (for firmware/metadata storage) |
| Supercap | Supercapacitor for power-loss protection |
| DEK | Data Encryption Key |
| CSPRNG | Cryptographically Secure Pseudo-Random Number Generator |
| AES-XTS | AES in XEX-based tweaked-codebook mode with ciphertext stealing |
| AES-KW | AES Key Wrap (RFC 3394) |

---

## 2. Requirements Traceability

| REQ-ID | Requirement Description | Priority | Implementation | Test Case |
|--------|------------------------|----------|---------------|-----------|
| FR-HAL-001 | NAND driver API | P0 | hal_nand module | UT_HAL_001-003 |
| FR-HAL-002 | NOR driver API | P2 | hal_nor module | UT_HAL_004 |
| FR-HAL-003 | Power management | P1 | hal_power module | UT_HAL_005 |
| FR-HAL-004 | Command issue queue | P0 | hal_cmd_queue module | UT_HAL_006 |
| FR-HAL-005 | Supercapacitor model | P1 | hal_supercap module | UT_SCAP_001-005 |
| FR-HAL-006 | Crypto engine API | P1 | hal_crypto module | UT_CRYPTO_HAL_001-005 |
| FR-HAL-007 | Thermal sensor read API | P1 | hal_thermal module | UT_THERMAL_HAL_001-004 |

---

## 3. Data Structure Detailed Design

```c
#ifndef __HFSSS_HAL_NAND_H
#define __HFSSS_HAL_NAND_H

#include <stdint.h>
#include <stdbool.h>

/* HAL NAND Command */
struct hal_nand_cmd {
    uint32_t opcode;
    uint32_t ch;
    uint32_t chip;
    uint32_t die;
    uint32_t plane;
    uint32_t block;
    uint32_t page;
    void *data;
    void *spare;
    uint64_t timestamp;
    int (*callback)(void *ctx, int status);
    void *callback_ctx;
};

/* HAL NAND Device */
struct hal_nand_dev {
    uint32_t channel_count;
    uint32_t chips_per_channel;
    uint32_t dies_per_chip;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t spare_size;
    void *media_ctx;
};

#endif /* __HFSSS_HAL_NAND_H */
```

---

## 4. Header File Design

```c
#ifndef __HFSSS_HAL_H
#define __HFSSS_HAL_H

#include "hal_nand.h"
#include "hal_nor.h"
#include "hal_pci.h"
#include "hal_power.h"

/* HAL Context */
struct hal_ctx {
    struct hal_nand_dev *nand;
    struct hal_nor_dev *nor;
    void *pci_ctx;
    void *power_ctx;
};

/* Function Prototypes */
int hal_init(struct hal_ctx *ctx);
void hal_cleanup(struct hal_ctx *ctx);
int hal_nand_read(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_read_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);

#endif /* __HFSSS_HAL_H */
```

---

## 5. Function Interface Detailed Design

### 5.1 HAL NAND Read

**Declaration**:
```c
int hal_nand_read(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
```

**Parameter Description**:
- ctx: HAL context
- cmd: NAND command descriptor

**Return Values**:
- 0: Success
- -EIO: I/O error
- -EINVAL: Invalid parameters

### 5.2 HAL NAND Program

**Declaration**:
```c
int hal_nand_program(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
```

**Parameter Description**:
- ctx: HAL context
- cmd: NAND command descriptor containing data and spare pointers

**Return Values**:
- 0: Success
- -EIO: Program failure

### 5.3 HAL NAND Erase

**Declaration**:
```c
int hal_nand_erase(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
```

**Parameter Description**:
- ctx: HAL context
- cmd: NAND command descriptor (only ch/chip/die/plane/block fields used)

**Return Values**:
- 0: Success
- -EIO: Erase failure

---

## 6. Supercapacitor Model Detailed Design

### 6.1 Overview

Enterprise SSDs include supercapacitors to provide backup power during unexpected power loss. The supercapacitor model simulates energy storage, discharge behavior, and voltage threshold monitoring to enable realistic power-loss protection (PLP) testing.

### 6.2 Physics Model

**Stored Energy**:
```
E = 0.5 * C * V^2
```
Where:
- E = stored energy in Joules
- C = capacitance in Farads
- V = voltage across the supercapacitor in Volts

**Discharge Curve** (constant-resistance load):
```
V(t) = V0 * exp(-t / (R * C))
```
Where:
- V0 = initial voltage at the start of discharge
- R = equivalent load resistance in Ohms
- C = capacitance in Farads
- t = time since discharge began in seconds

**Drain Time Calculation** (time to discharge from V0 to V_min):
```
t_drain = -R * C * ln(V_min / V0)
```

**Available Energy during drain**:
```
E_available = 0.5 * C * (V0^2 - V_min^2)
```

### 6.3 Data Structures

```c
#ifndef __HFSSS_HAL_SUPERCAP_H
#define __HFSSS_HAL_SUPERCAP_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Supercapacitor state */
enum supercap_state {
    SCAP_CHARGING    = 0,  /* Connected to main power, charging */
    SCAP_CHARGED     = 1,  /* Fully charged, standby */
    SCAP_DISCHARGING = 2,  /* Power lost, discharging through load */
    SCAP_DEPLETED    = 3,  /* Below minimum voltage threshold */
    SCAP_FAULT       = 4,  /* Capacitor fault detected */
};

/* Supercapacitor configuration */
struct supercap_config {
    double capacitance_f;       /* Capacitance in Farads (e.g., 1.0F) */
    double esr_ohms;            /* Equivalent Series Resistance (Ohms) */
    double v_max;               /* Maximum charge voltage (V), e.g., 5.0V */
    double v_min;               /* Minimum operational voltage (V), e.g., 2.5V */
    double v_warning;           /* Warning voltage threshold (V), e.g., 3.5V */
    double v_critical;          /* Critical voltage threshold (V), e.g., 3.0V */
    double charge_current_a;    /* Charge current (A), e.g., 0.5A */
    double load_resistance_ohms;/* Equivalent load resistance during discharge (Ohms) */
    double leakage_current_a;   /* Self-discharge leakage current (A), e.g., 10uA */
    double aging_factor;        /* Capacitance degradation factor (0.0-1.0), 1.0 = new */
};

/* Supercapacitor state data */
struct supercap_state_data {
    enum supercap_state state;
    double voltage;             /* Current voltage (V) */
    double energy_j;            /* Current stored energy (J) */
    double drain_time_remaining_s; /* Estimated time remaining (seconds) */
    uint64_t discharge_start_ts;   /* Timestamp when discharge began (ns) */
    uint64_t last_update_ts;       /* Last model update timestamp (ns) */
};

/* Voltage threshold interrupt callback */
typedef void (*supercap_threshold_cb)(void *ctx, double voltage,
                                      enum supercap_state new_state);

/* Supercapacitor context */
struct hal_supercap_ctx {
    struct supercap_config   config;
    struct supercap_state_data state;

    /* Threshold interrupt callbacks */
    supercap_threshold_cb    warning_cb;
    void                     *warning_cb_ctx;
    supercap_threshold_cb    critical_cb;
    void                     *critical_cb_ctx;
    supercap_threshold_cb    depleted_cb;
    void                     *depleted_cb_ctx;

    /* Statistics */
    uint64_t                 charge_cycles;
    uint64_t                 discharge_events;
    double                   total_discharge_time_s;
    double                   min_voltage_recorded;

    spinlock_t               lock;
};

#endif /* __HFSSS_HAL_SUPERCAP_H */
```

### 6.4 Supercapacitor Model Functions

```c
/*
 * Initialize supercapacitor model.
 * Starts in CHARGED state at V_max.
 */
int hal_supercap_init(struct hal_supercap_ctx *ctx,
                      const struct supercap_config *config);

/*
 * Cleanup supercapacitor model.
 */
void hal_supercap_cleanup(struct hal_supercap_ctx *ctx);

/*
 * Simulate power loss event. Transitions from CHARGED to DISCHARGING.
 */
int hal_supercap_power_loss(struct hal_supercap_ctx *ctx, uint64_t now_ns);

/*
 * Simulate power restored event. Transitions back to CHARGING.
 */
int hal_supercap_power_restore(struct hal_supercap_ctx *ctx, uint64_t now_ns);

/*
 * Update supercapacitor voltage based on elapsed time.
 * During discharge: V(t) = V0 * exp(-t / (R*C))
 * During charge: V(t) = V_max * (1 - exp(-t / (R_charge * C)))
 *
 * Fires threshold callbacks when voltage crosses warning/critical/depleted levels.
 */
void hal_supercap_update(struct hal_supercap_ctx *ctx, uint64_t now_ns);

/*
 * Get current voltage.
 */
double hal_supercap_get_voltage(struct hal_supercap_ctx *ctx);

/*
 * Get estimated drain time remaining (seconds).
 * Returns time from current voltage to V_min.
 * t_remaining = -R * C * ln(V_min / V_current)
 */
double hal_supercap_get_drain_time(struct hal_supercap_ctx *ctx);

/*
 * Get available energy (Joules).
 * E = 0.5 * C * (V_current^2 - V_min^2)
 */
double hal_supercap_get_energy(struct hal_supercap_ctx *ctx);

/*
 * Register voltage threshold interrupt callbacks.
 */
int hal_supercap_register_warning_cb(struct hal_supercap_ctx *ctx,
                                      supercap_threshold_cb cb, void *cb_ctx);
int hal_supercap_register_critical_cb(struct hal_supercap_ctx *ctx,
                                       supercap_threshold_cb cb, void *cb_ctx);
int hal_supercap_register_depleted_cb(struct hal_supercap_ctx *ctx,
                                       supercap_threshold_cb cb, void *cb_ctx);

/*
 * Configure supercapacitor parameters (for aging simulation).
 */
int hal_supercap_set_aging(struct hal_supercap_ctx *ctx, double aging_factor);
```

### 6.5 Discharge Model Implementation

```c
void hal_supercap_update(struct hal_supercap_ctx *ctx, uint64_t now_ns)
{
    double dt_sec = (double)(now_ns - ctx->state.last_update_ts) / 1e9;
    if (dt_sec <= 0) return;

    double C_effective = ctx->config.capacitance_f * ctx->config.aging_factor;

    switch (ctx->state.state) {
    case SCAP_DISCHARGING: {
        double R = ctx->config.load_resistance_ohms;
        double tau = R * C_effective;
        double t_since_discharge = (double)(now_ns - ctx->state.discharge_start_ts) / 1e9;

        /* V(t) = V0 * exp(-t / tau) */
        double V0 = ctx->state.voltage; /* Approximate: use iterative update */
        ctx->state.voltage *= exp(-dt_sec / tau);

        /* Account for leakage */
        ctx->state.voltage -= (ctx->config.leakage_current_a * dt_sec) / C_effective;
        if (ctx->state.voltage < 0) ctx->state.voltage = 0;

        /* Update energy */
        ctx->state.energy_j = 0.5 * C_effective * ctx->state.voltage * ctx->state.voltage;

        /* Update drain time remaining */
        if (ctx->state.voltage > ctx->config.v_min) {
            ctx->state.drain_time_remaining_s =
                -ctx->config.load_resistance_ohms * C_effective *
                log(ctx->config.v_min / ctx->state.voltage);
        } else {
            ctx->state.drain_time_remaining_s = 0;
        }

        /* Check voltage thresholds */
        if (ctx->state.voltage <= ctx->config.v_min) {
            ctx->state.state = SCAP_DEPLETED;
            if (ctx->depleted_cb)
                ctx->depleted_cb(ctx->depleted_cb_ctx, ctx->state.voltage, SCAP_DEPLETED);
        } else if (ctx->state.voltage <= ctx->config.v_critical) {
            if (ctx->critical_cb)
                ctx->critical_cb(ctx->critical_cb_ctx, ctx->state.voltage, SCAP_DISCHARGING);
        } else if (ctx->state.voltage <= ctx->config.v_warning) {
            if (ctx->warning_cb)
                ctx->warning_cb(ctx->warning_cb_ctx, ctx->state.voltage, SCAP_DISCHARGING);
        }

        /* Track statistics */
        if (ctx->state.voltage < ctx->min_voltage_recorded)
            ctx->min_voltage_recorded = ctx->state.voltage;
        break;
    }
    case SCAP_CHARGING: {
        /* Simple linear charge model: V += (I_charge * dt) / C */
        ctx->state.voltage += (ctx->config.charge_current_a * dt_sec) / C_effective;
        if (ctx->state.voltage >= ctx->config.v_max) {
            ctx->state.voltage = ctx->config.v_max;
            ctx->state.state = SCAP_CHARGED;
            ctx->charge_cycles++;
        }
        ctx->state.energy_j = 0.5 * C_effective * ctx->state.voltage * ctx->state.voltage;
        break;
    }
    default:
        break;
    }

    ctx->state.last_update_ts = now_ns;
}
```

### 6.6 Supercapacitor Discharge Curve

```
Voltage (V)
  ^
5.0|____
   |    \
4.5|     \
   |      \
4.0|       \___
   |           \___
3.5|...............\...... V_warning
   |                \___
3.0|.....................\. V_critical
   |                     \___
2.5|.........................\ V_min (DEPLETED)
   |                         \___
2.0|                             \___
   +---+---+---+---+---+---+---+---+---> time (seconds)
   0  0.5  1.0  1.5  2.0  2.5  3.0  3.5
       |           |           |
       Power loss  Warning     Critical
```

### 6.7 Supercapacitor Test Cases

| ID | Test Item | Expected Result |
|----|----------|----------------|
| UT_SCAP_001 | Init at full charge | V = V_max, state = CHARGED |
| UT_SCAP_002 | Discharge voltage curve | V follows exponential decay |
| UT_SCAP_003 | Warning callback fires | Callback at V_warning crossing |
| UT_SCAP_004 | Critical callback fires | Callback at V_critical crossing |
| UT_SCAP_005 | Drain time calculation | Matches -RC*ln(V_min/V0) |

---

## 7. Crypto Engine API

### 7.1 Overview

The HAL crypto engine API provides a hardware-abstraction interface for cryptographic operations. The FTL and media layers use these APIs for data-at-rest encryption without directly managing crypto implementation details.

### 7.2 Data Structures

```c
#ifndef __HFSSS_HAL_CRYPTO_H
#define __HFSSS_HAL_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>

#define HAL_CRYPTO_MAX_KEY_SLOTS  64
#define HAL_CRYPTO_KEY_SIZE_128   16
#define HAL_CRYPTO_KEY_SIZE_256   32
#define HAL_CRYPTO_XTS_KEY_SIZE   64  /* data_key + tweak_key */

/* Crypto engine state */
enum hal_crypto_state {
    HAL_CRYPTO_UNINIT  = 0,
    HAL_CRYPTO_READY   = 1,
    HAL_CRYPTO_BUSY    = 2,
    HAL_CRYPTO_ERROR   = 3,
};

/* Crypto engine context */
struct hal_crypto_ctx {
    enum hal_crypto_state state;
    uint32_t              key_slot_count;
    bool                  hw_accel;        /* Use AES-NI if available */
    void                  *engine_private; /* Opaque crypto engine handle */

    /* Performance simulation */
    uint64_t              encrypt_latency_ns; /* Simulated per-page encrypt latency */
    uint64_t              decrypt_latency_ns; /* Simulated per-page decrypt latency */

    /* Statistics */
    uint64_t              total_encrypts;
    uint64_t              total_decrypts;
    uint64_t              total_key_loads;
    uint64_t              total_key_clears;
};

#endif /* __HFSSS_HAL_CRYPTO_H */
```

### 7.3 API Functions

```c
/*
 * hal_crypto_init - Initialize the crypto engine.
 *
 * Allocates key slot table, initializes AES engine (software or AES-NI).
 *
 * @ctx:           Crypto engine context to initialize
 * @key_slots:     Number of key slots to allocate
 * @hw_accel:      Enable hardware acceleration (AES-NI) if available
 *
 * Returns: 0 on success, -ENOMEM on allocation failure
 */
int hal_crypto_init(struct hal_crypto_ctx *ctx, uint32_t key_slots, bool hw_accel);

/*
 * hal_crypto_encrypt_page - Encrypt a NAND page using AES-XTS.
 *
 * Encrypts page_size bytes from src to dst using the key loaded in
 * the specified key slot. XTS tweak is derived from base_lba.
 *
 * @ctx:        Crypto engine context
 * @key_slot:   Key slot index containing the active DEK
 * @base_lba:   Base LBA for XTS tweak derivation
 * @src:        Source plaintext buffer (page_size bytes)
 * @dst:        Destination ciphertext buffer (page_size bytes)
 * @page_size:  Page size in bytes
 *
 * Returns: 0 on success, -ENOKEY if key slot is empty, -EINVAL on bad params
 */
int hal_crypto_encrypt_page(struct hal_crypto_ctx *ctx,
                             uint32_t key_slot,
                             uint64_t base_lba,
                             const uint8_t *src,
                             uint8_t *dst,
                             uint32_t page_size);

/*
 * hal_crypto_decrypt_page - Decrypt a NAND page using AES-XTS.
 *
 * Symmetric to encrypt_page.
 *
 * @ctx:        Crypto engine context
 * @key_slot:   Key slot index
 * @base_lba:   Base LBA for XTS tweak derivation
 * @src:        Source ciphertext buffer
 * @dst:        Destination plaintext buffer
 * @page_size:  Page size in bytes
 *
 * Returns: 0 on success, -ENOKEY if key slot is empty
 */
int hal_crypto_decrypt_page(struct hal_crypto_ctx *ctx,
                             uint32_t key_slot,
                             uint64_t base_lba,
                             const uint8_t *src,
                             uint8_t *dst,
                             uint32_t page_size);

/*
 * hal_crypto_load_key - Load a DEK into a key slot.
 *
 * Loads the AES-XTS key pair (data key + tweak key) into the specified slot.
 *
 * @ctx:        Crypto engine context
 * @key_slot:   Target key slot index
 * @key_data:   Key material (data_key || tweak_key)
 * @key_size:   Size of each key half (16 for AES-128, 32 for AES-256)
 *
 * Returns: 0 on success, -EINVAL if slot out of range, -EBUSY if slot active
 */
int hal_crypto_load_key(struct hal_crypto_ctx *ctx,
                         uint32_t key_slot,
                         const uint8_t *key_data,
                         uint32_t key_size);

/*
 * hal_crypto_clear_key - Zeroize and clear a key slot.
 *
 * Securely erases the key material from the specified slot.
 * After clearing, any encrypt/decrypt using this slot will fail with -ENOKEY.
 * This is the mechanism for crypto erase.
 *
 * @ctx:        Crypto engine context
 * @key_slot:   Key slot to clear
 *
 * Returns: 0 on success
 */
int hal_crypto_clear_key(struct hal_crypto_ctx *ctx, uint32_t key_slot);
```

### 7.4 Crypto Engine Test Cases

| ID | Test Item | Expected Result |
|----|----------|----------------|
| UT_CRYPTO_HAL_001 | Init crypto engine | State = READY, slots allocated |
| UT_CRYPTO_HAL_002 | Load key + encrypt + decrypt | Roundtrip produces original data |
| UT_CRYPTO_HAL_003 | Encrypt with unloaded slot | Returns -ENOKEY |
| UT_CRYPTO_HAL_004 | Clear key then decrypt | Returns -ENOKEY |
| UT_CRYPTO_HAL_005 | Multiple key slots | Independent keys produce different ciphertext |

---

## 8. Thermal Sensor Read API

### 8.1 Overview

The HAL thermal sensor API provides a hardware-abstraction interface for reading die temperatures, setting threshold alerts, and registering callbacks. This API bridges the thermal simulation model in the media layer with the thermal management service in common services.

### 8.2 Data Structures

```c
#ifndef __HFSSS_HAL_THERMAL_H
#define __HFSSS_HAL_THERMAL_H

#include <stdint.h>
#include <stdbool.h>

/* Thermal sensor location */
struct thermal_sensor_id {
    uint32_t channel;
    uint32_t chip;
    uint32_t die;
};

/* Thermal threshold type */
enum thermal_threshold_type {
    THERMAL_THRESH_WARNING  = 0,
    THERMAL_THRESH_CRITICAL = 1,
    THERMAL_THRESH_SHUTDOWN = 2,
};

/* Thermal event callback */
typedef void (*hal_thermal_callback_t)(void *ctx,
                                        struct thermal_sensor_id sensor,
                                        double temperature,
                                        enum thermal_threshold_type type);

/* HAL Thermal context */
struct hal_thermal_ctx {
    void *thermal_sim;  /* Pointer to thermal_sim_ctx in media layer */

    /* Registered callbacks */
    hal_thermal_callback_t callbacks[3];  /* One per threshold type */
    void *callback_ctx[3];

    /* Thresholds */
    double threshold[3];  /* Warning, Critical, Shutdown temperatures */
    double hysteresis;    /* Temperature hysteresis for threshold clearing */

    /* Polling */
    uint64_t poll_interval_ns;
    uint64_t last_poll_ts;
};

#endif /* __HFSSS_HAL_THERMAL_H */
```

### 8.3 API Functions

```c
/*
 * hal_thermal_read_die_temp - Read the junction temperature of a specific die.
 *
 * Queries the thermal simulation model for the current temperature
 * of the specified die.
 *
 * @ctx:       HAL thermal context
 * @channel:   Channel ID
 * @chip:      Chip ID
 * @die:       Die ID
 * @temp_out:  Output: temperature in degrees Celsius
 *
 * Returns: 0 on success, -EINVAL if die ID is out of range
 */
int hal_thermal_read_die_temp(struct hal_thermal_ctx *ctx,
                               uint32_t channel, uint32_t chip, uint32_t die,
                               double *temp_out);

/*
 * hal_thermal_read_composite_temp - Read the composite (maximum) temperature.
 *
 * Returns the maximum junction temperature across all dies.
 *
 * @ctx:       HAL thermal context
 * @temp_out:  Output: composite temperature in degrees Celsius
 *
 * Returns: 0 on success
 */
int hal_thermal_read_composite_temp(struct hal_thermal_ctx *ctx, double *temp_out);

/*
 * hal_thermal_set_threshold - Set a temperature threshold for alerts.
 *
 * When the composite temperature crosses the threshold, the registered
 * callback (if any) will be invoked.
 *
 * @ctx:        HAL thermal context
 * @type:       Threshold type (WARNING, CRITICAL, SHUTDOWN)
 * @temp_c:     Threshold temperature in degrees Celsius
 *
 * Returns: 0 on success
 */
int hal_thermal_set_threshold(struct hal_thermal_ctx *ctx,
                               enum thermal_threshold_type type,
                               double temp_c);

/*
 * hal_thermal_register_callback - Register a callback for thermal events.
 *
 * The callback is invoked when the composite temperature crosses the
 * specified threshold type (with hysteresis applied on clearing).
 *
 * @ctx:        HAL thermal context
 * @type:       Threshold type
 * @cb:         Callback function
 * @cb_ctx:     User context passed to callback
 *
 * Returns: 0 on success
 */
int hal_thermal_register_callback(struct hal_thermal_ctx *ctx,
                                   enum thermal_threshold_type type,
                                   hal_thermal_callback_t cb,
                                   void *cb_ctx);

/*
 * hal_thermal_poll - Periodic thermal polling.
 *
 * Called by the thermal management service at the configured poll interval.
 * Updates thermal simulation, checks thresholds, fires callbacks.
 *
 * @ctx:     HAL thermal context
 * @now_ns:  Current timestamp in nanoseconds
 *
 * Returns: 0 on success
 */
int hal_thermal_poll(struct hal_thermal_ctx *ctx, uint64_t now_ns);
```

### 8.4 Thermal Sensor Test Cases

| ID | Test Item | Expected Result |
|----|----------|----------------|
| UT_THERMAL_HAL_001 | Read ambient temperature at init | Returns T_ambient |
| UT_THERMAL_HAL_002 | Read die temp after operations | Returns elevated temperature |
| UT_THERMAL_HAL_003 | Threshold callback fires | Callback invoked when temp > threshold |
| UT_THERMAL_HAL_004 | Hysteresis prevents oscillation | Callback not re-fired until temp drops by hysteresis amount |

---

## 9. Architecture Decision Records

### ADR-001: Supercapacitor as Continuous Model vs Discrete Events

**Context**: Supercapacitor voltage can be modeled as a continuous exponential function or as discrete voltage steps.

**Decision**: Continuous exponential model with periodic sampling.

**Rationale**: The exponential discharge curve is the physically accurate model. Discrete steps would introduce quantization error. Sampling at 1ms intervals provides sufficient accuracy for firmware testing while keeping computational cost low.

### ADR-002: HAL Crypto API Abstracts Implementation

**Context**: Crypto operations may use software AES, AES-NI, or simulated hardware engine.

**Decision**: HAL crypto API is implementation-agnostic; the backend is selected at init time.

**Rationale**: FTL code should not change based on whether crypto runs in software simulation or via hardware acceleration. The HAL API provides a stable interface while allowing backend substitution.

### ADR-003: Thermal Sensor Polling vs Interrupt Model

**Context**: Thermal data can be push (interrupt on threshold) or pull (periodic poll).

**Decision**: Hybrid: periodic polling with callback registration for threshold crossing.

**Rationale**: Polling at 100ms intervals is sufficient for thermal management (thermal time constants are seconds). Callbacks provide timely notification for critical events without requiring constant polling.

---

## 10. Memory Budget Analysis

| Component | Per-Instance Size | Max Instances | Total |
|-----------|------------------|---------------|-------|
| hal_nand_dev | 64 B | 1 | 64 B |
| hal_nand_cmd | 96 B | 256 (outstanding) | 24 KB |
| hal_supercap_ctx | 256 B | 1 | 256 B |
| hal_crypto_ctx | 128 B + key_slots | 1 + 64 slots | 4 KB |
| hal_thermal_ctx | 128 B | 1 | 128 B |
| **Total HAL memory** | | | **~29 KB** |

---

## 11. Latency Budget Analysis

| Operation | Target Latency | Notes |
|-----------|---------------|-------|
| hal_nand_read (sync) | tR (40-80 us) | Passes through to media thread |
| hal_nand_program (sync) | tPROG (700 us) | Passes through to media thread |
| hal_nand_erase (sync) | tERS (3 ms) | Passes through to media thread |
| hal_crypto_encrypt_page | 2-5 us | Software AES-XTS for 16KB page |
| hal_crypto_load_key | < 1 us | Memory copy + validation |
| hal_crypto_clear_key | < 1 us | memset + barrier |
| hal_supercap_update | < 0.1 us | Exponential calculation |
| hal_thermal_read_die_temp | < 0.1 us | Direct read from simulation state |
| hal_thermal_poll | < 100 us | Update all dies + threshold check |

---

## 12. References

1. Supercapacitor Principles and Applications (Maxwell Technologies)
2. IEEE 1619-2018: AES-XTS Standard
3. NIST SP 800-38E: Recommendation for Block Cipher Modes of Operation: The XTS-AES Mode
4. JEDEC JESD218B: SSD Requirements
5. Thermal Management in Enterprise SSDs (Flash Memory Summit 2023)

---

## Appendix: Cross-References to HLD

| HLD Section | LLD Section | Notes |
|-------------|-------------|-------|
| HLD 3.4 HAL | LLD_04 Sections 3-5 | NAND/NOR driver abstractions |
| HLD 4.9 Power Loss Protection | LLD_04 Section 6 | Supercapacitor model (enterprise) |
| HLD 4.8 Data-at-Rest Encryption | LLD_04 Section 7 | Crypto engine HAL API (enterprise) |
| HLD 4.7 Thermal Management | LLD_04 Section 8 | Thermal sensor HAL API (enterprise) |

---

**Document Statistics**:
- Total sections: 13 (including appendix)
- Function interfaces: 8 (base) + 18 (enterprise extensions)
- Data structures: 3 (base) + 6 (enterprise extensions)
- Test cases: 6 (base) + 14 (enterprise extensions)
