// nv_power.c - NVIDIA GPU Power & Thermal Management Implementation
// Clock reading/setting, thermal monitoring, fan control, P-state management
#include "nv_power.h"
#include "gpu.h"

// ============================================================
// External State
// ============================================================

extern gpu_state_t gpu_state;

// Shorthand MMIO accessors using global gpu_state
#define GPU_RD32(reg)       nv_rd32(gpu_state.mmio, (reg))
#define GPU_WR32(reg, val)  nv_wr32(gpu_state.mmio, (reg), (val))

// ============================================================
// Global State
// ============================================================

nv_power_state_t nv_power_state;

// ============================================================
// Thermal Monitoring
// ============================================================

int nv_power_read_temp(void) {
    if (!gpu_state.initialized || !gpu_state.mmio) {
        nv_power_state.temp_celsius = 0;
        return 0;
    }

    if (gpu_state.arch >= 0x50) {
        // NV50+: Read PTHERM temperature sensor
        uint32_t val = GPU_RD32(NV_PTHERM_TEMP_STATUS);
        // Temperature is in bits [16:8] as integer, bits [7:0] as fraction
        int temp = (int)((val >> 8) & 0x1FF);
        // Some chips use different bit positions
        if (temp == 0 || temp > 150) {
            // Try alternate register
            val = GPU_RD32(NV_PTHERM_TEMP_SENSOR);
            temp = (int)((val >> 8) & 0x1FF);
        }
        nv_power_state.temp_celsius = temp;
    } else {
        // Pre-NV50: Temperature sensor at different location
        // NV40: 0x0015B4
        uint32_t val = GPU_RD32(0x0015B4);
        int temp = (int)(val & 0x1FF);
        nv_power_state.temp_celsius = temp;
    }

    if (nv_power_state.temp_celsius > nv_power_state.temp_max_seen) {
        nv_power_state.temp_max_seen = nv_power_state.temp_celsius;
    }

    return nv_power_state.temp_celsius;
}

void nv_power_set_thresholds(int thresh0, int thresh1, int thresh2) {
    nv_power_state.temp_threshold_0 = thresh0;
    nv_power_state.temp_threshold_1 = thresh1;
    nv_power_state.temp_threshold_2 = thresh2;

    if (!gpu_state.initialized || !gpu_state.mmio) return;

    if (gpu_state.arch >= 0x50) {
        GPU_WR32(NV_PTHERM_TEMP_THRESHOLD_0, (uint32_t)thresh0);
        GPU_WR32(NV_PTHERM_TEMP_THRESHOLD_1, (uint32_t)thresh1);
        GPU_WR32(NV_PTHERM_TEMP_THRESHOLD_2, (uint32_t)thresh2);
    }
}

// ============================================================
// Fan Control
// ============================================================

void nv_power_set_fan_mode(int mode) {
    nv_power_state.fan_mode = mode;
}

void nv_power_set_fan_duty(int duty) {
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;

    nv_power_state.fan_duty = duty;

    if (!gpu_state.initialized || !gpu_state.mmio) return;

    // Read PWM divider to calculate duty value
    uint32_t div = GPU_RD32(NV_PFAN_PWM_DIV);
    if (div == 0) div = 100;

    uint32_t duty_val = (uint32_t)duty * div / 100;

    // Set PWM duty cycle
    GPU_WR32(NV_PFAN_PWM_DUTY, duty_val);

    // Enable PWM if in manual mode
    if (nv_power_state.fan_mode == NV_FAN_MODE_MANUAL) {
        uint32_t ctrl = GPU_RD32(NV_PFAN_PWM_CTRL);
        ctrl |= 0x01; // Enable PWM
        GPU_WR32(NV_PFAN_PWM_CTRL, ctrl);
    }
}

int nv_power_get_fan_duty(void) {
    if (!gpu_state.initialized || !gpu_state.mmio) {
        return nv_power_state.fan_duty;
    }

    uint32_t div = GPU_RD32(NV_PFAN_PWM_DIV);
    if (div == 0) return 0;

    uint32_t duty = GPU_RD32(NV_PFAN_PWM_DUTY);
    nv_power_state.fan_duty = (int)(duty * 100 / div);

    return nv_power_state.fan_duty;
}

// ============================================================
// Clock Reading
// ============================================================

static uint32_t pll_to_freq(uint32_t pll_ctrl, uint32_t pll_coef) {
    if (!(pll_ctrl & 0x01)) return 0; // PLL disabled

    uint32_t n = NV_PLL_COEF_N(pll_coef);
    uint32_t m = NV_PLL_COEF_M(pll_coef);
    uint32_t p = NV_PLL_COEF_P(pll_coef);

    if (m == 0) return 0;

    // freq = refclk * N / M / (2^P)
    uint64_t freq = (uint64_t)NV_REFCLK_HZ * n / m;
    freq >>= p;

    return (uint32_t)(freq / 1000); // Return kHz
}

uint32_t nv_power_get_clock(int domain) {
    if (!gpu_state.initialized || !gpu_state.mmio) return 0;

    if (gpu_state.arch >= 0x50) {
        uint32_t ctrl, coef;
        switch (domain) {
        case NV_CLK_CORE:
            ctrl = GPU_RD32(NV50_CORE_PLL);
            coef = GPU_RD32(NV50_CORE_PLL_COEF);
            break;
        case NV_CLK_SHADER:
            ctrl = GPU_RD32(NV50_SHADER_PLL);
            coef = GPU_RD32(NV50_SHADER_PLL_COEF);
            break;
        case NV_CLK_MEMORY:
            ctrl = GPU_RD32(NV50_MPLL_0);
            coef = GPU_RD32(NV50_MPLL_0_COEF);
            break;
        default:
            return 0;
        }
        return pll_to_freq(ctrl, coef);
    } else {
        // Pre-NV50: Different PLL locations
        // NV40: core PLL at 0x004000
        uint32_t ctrl = GPU_RD32(0x004000 + domain * 0x10);
        uint32_t coef = GPU_RD32(0x004004 + domain * 0x10);
        return pll_to_freq(ctrl, coef);
    }
}

// ============================================================
// Clock Setting
// ============================================================

static void calc_pll(uint32_t target_khz, uint32_t* out_n, uint32_t* out_m, uint32_t* out_p) {
    uint32_t refclk_khz = NV_REFCLK_HZ / 1000; // 27000 kHz
    uint32_t best_n = 1, best_m = 1, best_p = 0;
    uint32_t best_diff = 0xFFFFFFFF;

    for (uint32_t p = 0; p < 8; p++) {
        uint32_t target_vco = target_khz << p;
        // VCO range: 128-700 MHz typically
        if (target_vco < 128000 || target_vco > 700000) continue;

        for (uint32_t m = 1; m <= 13; m++) {
            uint32_t n = target_vco * m / refclk_khz;
            if (n < 2 || n > 255) continue;

            uint32_t actual = refclk_khz * n / m;
            actual >>= p;

            uint32_t diff = (actual > target_khz) ? (actual - target_khz) : (target_khz - actual);
            if (diff < best_diff) {
                best_diff = diff;
                best_n = n;
                best_m = m;
                best_p = p;
            }
        }
    }

    *out_n = best_n;
    *out_m = best_m;
    *out_p = best_p;
}

int nv_power_set_clock(int domain, uint32_t freq_khz) {
    if (!gpu_state.initialized || !gpu_state.mmio) return -1;

    uint32_t n, m, p;
    calc_pll(freq_khz, &n, &m, &p);

    uint32_t coef = m | (n << 8) | (p << 16);

    if (gpu_state.arch >= 0x50) {
        uint32_t pll_reg, coef_reg;
        switch (domain) {
        case NV_CLK_CORE:
            pll_reg = NV50_CORE_PLL;
            coef_reg = NV50_CORE_PLL_COEF;
            break;
        case NV_CLK_SHADER:
            pll_reg = NV50_SHADER_PLL;
            coef_reg = NV50_SHADER_PLL_COEF;
            break;
        case NV_CLK_MEMORY:
            pll_reg = NV50_MPLL_0;
            coef_reg = NV50_MPLL_0_COEF;
            break;
        default:
            return -1;
        }

        // Disable PLL
        uint32_t ctrl = GPU_RD32(pll_reg);
        GPU_WR32(pll_reg, ctrl & ~0x01);

        // Set coefficients
        GPU_WR32(coef_reg, coef);

        // Re-enable PLL
        GPU_WR32(pll_reg, ctrl | 0x01);

        // Wait for PLL lock (simple delay)
        for (volatile int i = 0; i < 10000; i++) {}
    } else {
        // Pre-NV50
        uint32_t pll_reg = 0x004000 + (uint32_t)domain * 0x10;
        uint32_t coef_reg = pll_reg + 4;

        uint32_t ctrl = GPU_RD32(pll_reg);
        GPU_WR32(pll_reg, ctrl & ~0x01);
        GPU_WR32(coef_reg, coef);
        GPU_WR32(pll_reg, ctrl | 0x01);
        for (volatile int i = 0; i < 10000; i++) {}
    }

    // Update state
    nv_power_state.clocks[domain].freq = freq_khz;
    nv_power_state.clocks[domain].pll_n = n;
    nv_power_state.clocks[domain].pll_m = m;
    nv_power_state.clocks[domain].pll_p = p;

    return 0;
}

// ============================================================
// P-State Management
// ============================================================

static void init_default_pstates(void) {
    // Define some reasonable default P-states
    // Actual values would come from VBIOS parsing

    // P0 - Idle
    nv_power_state.pstates[0].id = 0;
    nv_power_state.pstates[0].clocks[NV_CLK_CORE].freq = 169000;   // 169 MHz
    nv_power_state.pstates[0].clocks[NV_CLK_SHADER].freq = 338000; // 338 MHz
    nv_power_state.pstates[0].clocks[NV_CLK_MEMORY].freq = 100000; // 100 MHz
    nv_power_state.pstates[0].voltage_mv = 950;
    nv_power_state.pstates[0].fan_duty = 30;
    nv_power_state.pstates[0].valid = 1;

    // P5 - Medium
    nv_power_state.pstates[1].id = 5;
    nv_power_state.pstates[1].clocks[NV_CLK_CORE].freq = 400000;   // 400 MHz
    nv_power_state.pstates[1].clocks[NV_CLK_SHADER].freq = 800000; // 800 MHz
    nv_power_state.pstates[1].clocks[NV_CLK_MEMORY].freq = 500000; // 500 MHz
    nv_power_state.pstates[1].voltage_mv = 1000;
    nv_power_state.pstates[1].fan_duty = 50;
    nv_power_state.pstates[1].valid = 1;

    // P10 - High
    nv_power_state.pstates[2].id = 10;
    nv_power_state.pstates[2].clocks[NV_CLK_CORE].freq = 600000;   // 600 MHz
    nv_power_state.pstates[2].clocks[NV_CLK_SHADER].freq = 1200000; // 1200 MHz
    nv_power_state.pstates[2].clocks[NV_CLK_MEMORY].freq = 800000; // 800 MHz
    nv_power_state.pstates[2].voltage_mv = 1050;
    nv_power_state.pstates[2].fan_duty = 70;
    nv_power_state.pstates[2].valid = 1;

    // P15 - Maximum
    nv_power_state.pstates[3].id = 15;
    nv_power_state.pstates[3].clocks[NV_CLK_CORE].freq = 700000;   // 700 MHz
    nv_power_state.pstates[3].clocks[NV_CLK_SHADER].freq = 1400000; // 1400 MHz
    nv_power_state.pstates[3].clocks[NV_CLK_MEMORY].freq = 900000; // 900 MHz
    nv_power_state.pstates[3].voltage_mv = 1100;
    nv_power_state.pstates[3].fan_duty = 100;
    nv_power_state.pstates[3].valid = 1;

    nv_power_state.pstate_count = 4;
}

int nv_power_set_pstate(int pstate_id) {
    // Find matching P-state
    nv_pstate_t* target = 0;
    for (int i = 0; i < NV_PSTATE_COUNT; i++) {
        if (nv_power_state.pstates[i].valid &&
            nv_power_state.pstates[i].id == pstate_id) {
            target = &nv_power_state.pstates[i];
            break;
        }
    }

    if (!target) return -1;

    // Set clocks
    for (int d = 0; d < NV_CLK_MAX; d++) {
        if (target->clocks[d].freq > 0) {
            nv_power_set_clock(d, target->clocks[d].freq);
        }
    }

    // Set fan duty (if auto mode)
    if (nv_power_state.fan_mode == NV_FAN_MODE_AUTO) {
        nv_power_set_fan_duty(target->fan_duty);
    }

    nv_power_state.current_pstate = pstate_id;
    return 0;
}

int nv_power_get_pstate(void) {
    return nv_power_state.current_pstate;
}

// ============================================================
// Clock Gating
// ============================================================

void nv_power_enable_cg(int enable) {
    nv_power_state.cg_enabled = enable;

    if (!gpu_state.initialized || !gpu_state.mmio) return;

    if (enable) {
        // Enable automatic clock gating
        uint32_t val = NV_CG_ENABLE | NV_CG_AUTO | NV_CG_IDLE_TIMEOUT(128);
        GPU_WR32(NV_PG_CG_CTRL, val);
    } else {
        GPU_WR32(NV_PG_CG_CTRL, 0);
    }
}

// ============================================================
// Periodic Update
// ============================================================

void nv_power_update(void) {
    if (!nv_power_state.initialized) return;

    // Read current temperature
    nv_power_read_temp();

    // Read current clocks
    for (int d = 0; d < NV_CLK_MAX; d++) {
        nv_power_state.clocks[d].freq = nv_power_get_clock(d);
    }

    // Read current fan duty
    nv_power_get_fan_duty();

    // Auto fan control based on temperature
    if (nv_power_state.fan_mode == NV_FAN_MODE_AUTO) {
        int temp = nv_power_state.temp_celsius;
        int duty = nv_power_state.fan_duty;

        if (temp >= nv_power_state.temp_threshold_2) {
            // Critical! Max fan + would need to shut down GPU
            nv_power_set_fan_duty(100);
        } else if (temp >= nv_power_state.temp_threshold_1) {
            // Throttle: high fan speed
            nv_power_set_fan_duty(90);
        } else if (temp >= nv_power_state.temp_threshold_0) {
            // Warm: ramp up fan
            int target = 30 + (temp - nv_power_state.temp_threshold_0) * 70 /
                         (nv_power_state.temp_threshold_1 - nv_power_state.temp_threshold_0);
            if (target > duty + 5) {
                nv_power_set_fan_duty(target);
            }
        } else if (temp < nv_power_state.temp_threshold_0 - 10) {
            // Cool: lower fan
            if (duty > 30) {
                nv_power_set_fan_duty(duty - 5);
            }
        }
    }
}

// ============================================================
// Initialization
// ============================================================

int nv_power_init(void) {
    // Zero state
    for (int i = 0; i < (int)sizeof(nv_power_state); i++) {
        ((char*)&nv_power_state)[i] = 0;
    }

    // Set default thresholds
    nv_power_state.temp_threshold_0 = 60;   // Fan starts ramping at 60°C
    nv_power_state.temp_threshold_1 = 85;   // Throttle at 85°C
    nv_power_state.temp_threshold_2 = 100;  // Critical at 100°C

    nv_power_state.fan_mode = NV_FAN_MODE_AUTO;
    nv_power_state.power_limit_mw = 150000; // 150W default TDP

    // Initialize default P-states
    init_default_pstates();

    // Read current clocks
    if (gpu_state.initialized && gpu_state.mmio) {
        for (int d = 0; d < NV_CLK_MAX; d++) {
            nv_power_state.clocks[d].freq = nv_power_get_clock(d);
        }
        nv_power_read_temp();
        nv_power_get_fan_duty();

        // Enable clock gating by default
        nv_power_enable_cg(1);

        // Program thermal thresholds
        nv_power_set_thresholds(
            nv_power_state.temp_threshold_0,
            nv_power_state.temp_threshold_1,
            nv_power_state.temp_threshold_2);

        // Start at idle P-state
        nv_power_set_pstate(0);
    }

    nv_power_state.initialized = 1;
    return 0;
}

void nv_power_shutdown(void) {
    if (gpu_state.initialized && gpu_state.mmio) {
        // Set minimum clocks
        nv_power_set_pstate(0);
        // Disable clock gating
        nv_power_enable_cg(0);
        // Max fan for safety during shutdown
        nv_power_set_fan_duty(100);
    }
    nv_power_state.initialized = 0;
}
