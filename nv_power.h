// nv_power.h - NVIDIA GPU Power & Thermal Management for Alteo OS
// Clock gating, P-state management, thermal monitoring, fan control
#ifndef NV_POWER_H
#define NV_POWER_H

#include "stdint.h"

// ============================================================
// NVIDIA Clock Domains
// ============================================================

#define NV_CLK_CORE     0   // GPU core clock
#define NV_CLK_SHADER   1   // Shader clock (NV50+)
#define NV_CLK_MEMORY   2   // Memory clock
#define NV_CLK_UNK03    3   // Host interface clock
#define NV_CLK_MAX      4

// ============================================================
// P-States (Performance States)
// ============================================================

#define NV_PSTATE_MIN   0x00    // Lowest power state
#define NV_PSTATE_LOW   0x03    // Low power
#define NV_PSTATE_MID   0x07    // Medium power
#define NV_PSTATE_HIGH  0x0A    // High performance
#define NV_PSTATE_MAX   0x0F    // Maximum performance
#define NV_PSTATE_AC    0xFF    // AC power default
#define NV_PSTATE_COUNT 16

// ============================================================
// Thermal Registers (PTHERM)
// ============================================================

// NV50+ PTHERM block at 0x20000
#define NV_PTHERM_BASE              0x020000
#define NV_PTHERM_TEMP_STATUS       0x020014   // Current temperature
#define NV_PTHERM_TEMP_SENSOR       0x020008
#define NV_PTHERM_TEMP_THRESHOLD_0  0x020400   // Threshold 0 (fan speed up)
#define NV_PTHERM_TEMP_THRESHOLD_1  0x020404   // Threshold 1 (throttle)
#define NV_PTHERM_TEMP_THRESHOLD_2  0x020408   // Threshold 2 (shutdown)
#define NV_PTHERM_INTR_0            0x020100
#define NV_PTHERM_INTR_EN_0         0x020140

// ============================================================
// Fan Control (PWM)
// ============================================================

#define NV_PFAN_BASE        0x00E000
#define NV_PFAN_PWM_CTRL    0x00E110   // PWM controller
#define NV_PFAN_PWM_DIV     0x00E114   // PWM divider
#define NV_PFAN_PWM_DUTY    0x00E118   // PWM duty cycle

#define NV_FAN_MODE_AUTO    0
#define NV_FAN_MODE_MANUAL  1

// ============================================================
// Clock Gating
// ============================================================

// Engine clock gate registers
#define NV_PG_ELPG_0       0x020200   // Engine level power gating
#define NV_PG_CG_CTRL      0x020210   // Clock gating control

// Clock gating flags
#define NV_CG_IDLE_TIMEOUT(x)  ((x) & 0xFF)
#define NV_CG_ENABLE           (1 << 8)
#define NV_CG_AUTO             (1 << 9)

// ============================================================
// PLL Registers for Clock Control
// ============================================================

// NV50 clock PLLs
#define NV50_CORE_PLL       0x004028
#define NV50_CORE_PLL_COEF  0x00402C
#define NV50_SHADER_PLL     0x004020
#define NV50_SHADER_PLL_COEF 0x004024
#define NV50_MPLL_0         0x004008
#define NV50_MPLL_0_COEF    0x00400C

// PLL coefficient fields
#define NV_PLL_COEF_N(v)    (((v) >> 8) & 0xFF)
#define NV_PLL_COEF_M(v)    ((v) & 0xFF)
#define NV_PLL_COEF_P(v)    (((v) >> 16) & 0x3F)

// Reference clock (typical)
#define NV_REFCLK_HZ       27000000   // 27 MHz

// ============================================================
// Voltage Control
// ============================================================

#define NV_VOLTAGE_GPIO     0   // Voltage set via GPIO
#define NV_VOLTAGE_PWM      1   // Voltage set via PWM

typedef struct {
    int     type;       // NV_VOLTAGE_GPIO or NV_VOLTAGE_PWM
    int     min_mv;     // Minimum voltage in millivolts
    int     max_mv;     // Maximum voltage
    int     current_mv;
    int     step_mv;    // Voltage step
} nv_voltage_t;

// ============================================================
// Clock State
// ============================================================

typedef struct {
    uint32_t freq;      // Frequency in kHz
    uint32_t pll_n;
    uint32_t pll_m;
    uint32_t pll_p;
} nv_clock_t;

// ============================================================
// P-State Entry
// ============================================================

typedef struct {
    int         id;
    nv_clock_t  clocks[NV_CLK_MAX];
    int         voltage_mv;
    int         fan_duty;       // Fan duty cycle 0-100
    int         valid;
} nv_pstate_t;

// ============================================================
// Power State
// ============================================================

typedef struct {
    int         initialized;

    // Current clocks
    nv_clock_t  clocks[NV_CLK_MAX];

    // P-states
    nv_pstate_t pstates[NV_PSTATE_COUNT];
    int         current_pstate;
    int         pstate_count;

    // Thermal
    int         temp_celsius;       // Current GPU temp
    int         temp_threshold_0;   // Fan speed up
    int         temp_threshold_1;   // Throttle
    int         temp_threshold_2;   // Critical shutdown
    int         temp_max_seen;

    // Fan
    int         fan_mode;           // AUTO or MANUAL
    int         fan_duty;           // Current duty 0-100
    int         fan_rpm;            // Estimated RPM (if tachometer available)

    // Voltage
    nv_voltage_t voltage;

    // Clock gating
    int         cg_enabled;         // Clock gating enabled

    // Power limit
    uint32_t    power_limit_mw;     // TDP in milliwatts
    uint32_t    power_current_mw;   // Current power draw estimate
} nv_power_state_t;

// ============================================================
// API
// ============================================================

// ---- Core ----
int  nv_power_init(void);
void nv_power_shutdown(void);

// ---- Thermal ----
int  nv_power_read_temp(void);
void nv_power_set_thresholds(int thresh0, int thresh1, int thresh2);

// ---- Fan ----
void nv_power_set_fan_mode(int mode);
void nv_power_set_fan_duty(int duty);    // 0-100
int  nv_power_get_fan_duty(void);

// ---- Clocks ----
uint32_t nv_power_get_clock(int domain);    // Returns kHz
int      nv_power_set_clock(int domain, uint32_t freq_khz);

// ---- P-State ----
int  nv_power_set_pstate(int pstate_id);
int  nv_power_get_pstate(void);

// ---- Clock Gating ----
void nv_power_enable_cg(int enable);

// ---- Monitoring ----
void nv_power_update(void);    // Call periodically to update stats

#endif
