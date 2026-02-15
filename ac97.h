// ac97.h - AC'97 Audio Codec Driver for Alteo OS
#ifndef AC97_H
#define AC97_H

#include "stdint.h"

// PCI IDs for AC97 controllers
#define AC97_VENDOR_INTEL       0x8086
#define AC97_DEVICE_ICH         0x2415  // 82801AA
#define AC97_DEVICE_ICH4        0x24C5  // ICH4
#define AC97_VENDOR_REALTEK     0x10EC

// AC97 Native Audio Mixer registers (BAR0)
#define AC97_RESET              0x00
#define AC97_MASTER_VOL         0x02
#define AC97_AUX_OUT_VOL        0x04
#define AC97_MONO_VOL           0x06
#define AC97_MASTER_TONE        0x08
#define AC97_PC_BEEP_VOL        0x0A
#define AC97_PHONE_VOL          0x0C
#define AC97_MIC_VOL            0x0E
#define AC97_LINE_IN_VOL        0x10
#define AC97_CD_VOL             0x12
#define AC97_VIDEO_VOL          0x14
#define AC97_AUX_IN_VOL         0x16
#define AC97_PCM_OUT_VOL        0x18
#define AC97_RECORD_SELECT      0x1A
#define AC97_RECORD_GAIN        0x1C
#define AC97_GENERAL_PURPOSE    0x20
#define AC97_3D_CONTROL         0x22
#define AC97_POWERDOWN          0x26
#define AC97_EXT_AUDIO_ID       0x28
#define AC97_EXT_AUDIO_CTRL     0x2A
#define AC97_PCM_FRONT_RATE     0x2C
#define AC97_PCM_SURR_RATE      0x2E
#define AC97_PCM_LFE_RATE       0x30
#define AC97_VENDOR_ID1         0x7C
#define AC97_VENDOR_ID2         0x7E

// AC97 Bus Master registers (BAR1)
#define AC97_BM_PI_BDBAR        0x00    // PCM In Buffer Desc Base Addr
#define AC97_BM_PI_CIV          0x04    // PCM In Current Index Value
#define AC97_BM_PI_LVI          0x05    // PCM In Last Valid Index
#define AC97_BM_PI_SR           0x06    // PCM In Status
#define AC97_BM_PI_CR           0x0B    // PCM In Control
#define AC97_BM_PO_BDBAR        0x10    // PCM Out Buffer Desc Base Addr
#define AC97_BM_PO_CIV          0x14    // PCM Out Current Index Value
#define AC97_BM_PO_LVI          0x15    // PCM Out Last Valid Index
#define AC97_BM_PO_SR           0x16    // PCM Out Status
#define AC97_BM_PO_CR           0x1B    // PCM Out Control
#define AC97_BM_MC_BDBAR        0x20    // Mic In Buffer Desc Base Addr
#define AC97_BM_GLB_CR          0x2C    // Global Control
#define AC97_BM_GLB_SR          0x30    // Global Status

// Control register bits
#define AC97_CR_RPBM            (1 << 0)  // Run/Pause Bus Master
#define AC97_CR_RR              (1 << 1)  // Reset Registers
#define AC97_CR_LVBIE           (1 << 2)  // Last Valid Buf Interrupt Enable
#define AC97_CR_FEIE            (1 << 3)  // FIFO Error Interrupt Enable
#define AC97_CR_IOCE            (1 << 4)  // Interrupt On Completion Enable

// Global control bits
#define AC97_GCR_GIE            (1 << 0)  // Global Interrupt Enable
#define AC97_GCR_COLD_RST       (1 << 1)  // Cold Reset

// Status register bits
#define AC97_SR_DCH             (1 << 0)  // DMA Controller Halted
#define AC97_SR_CELV            (1 << 1)  // Current Equals Last Valid
#define AC97_SR_LVBCI           (1 << 2)  // Last Valid Buffer Completion Interrupt
#define AC97_SR_BCIS            (1 << 3)  // Buffer Completion Interrupt Status
#define AC97_SR_FIFOE           (1 << 4)  // FIFO Error

// Buffer Descriptor Entry
#define AC97_BDL_ENTRIES        32
#define AC97_BDL_BUF_SIZE       4096  // Per-entry buffer size

// Audio format constants
#define AUDIO_SAMPLE_RATE_44100 44100
#define AUDIO_SAMPLE_RATE_48000 48000
#define AUDIO_CHANNELS_MONO     1
#define AUDIO_CHANNELS_STEREO   2
#define AUDIO_BITS_8            8
#define AUDIO_BITS_16           16

// Buffer Descriptor List entry
typedef struct __attribute__((packed)) {
    uint32_t addr;      // Physical address of buffer
    uint16_t length;    // Number of samples (not bytes)
    uint16_t flags;     // BUP (bit 14) and IOC (bit 15)
} ac97_bdl_entry_t;

#define AC97_BDL_IOC    (1 << 15)  // Interrupt on Completion
#define AC97_BDL_BUP    (1 << 14)  // Buffer Underrun Policy

// Audio stream state
typedef enum {
    AUDIO_STOPPED = 0,
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_RECORDING
} audio_state_t;

// AC97 device structure
typedef struct {
    uint16_t mixer_base;    // BAR0 - Mixer registers
    uint16_t busmaster_base; // BAR1 - Bus master registers
    int      available;     // Device found and initialized
    int      irq;           // IRQ number

    // Audio state
    audio_state_t play_state;
    audio_state_t rec_state;

    // Playback
    ac97_bdl_entry_t* play_bdl;
    uint8_t* play_bufs[AC97_BDL_ENTRIES];
    int play_cur_buf;

    // Volume (0-100)
    int master_volume;
    int pcm_volume;
    int mic_volume;
    int muted;

    // Format
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
} ac97_device_t;

// Initialize AC97 audio driver
int  ac97_init(void);

// Check if audio hardware is available
int  ac97_is_available(void);

// Volume control
void ac97_set_master_volume(int volume);  // 0-100
void ac97_set_pcm_volume(int volume);     // 0-100
int  ac97_get_master_volume(void);
int  ac97_get_pcm_volume(void);
void ac97_mute(int mute);  // 1=mute, 0=unmute
int  ac97_is_muted(void);

// Playback
int  ac97_play(const uint8_t* samples, uint32_t num_samples);
void ac97_stop(void);
void ac97_pause(void);
void ac97_resume(void);
audio_state_t ac97_get_state(void);

// Set audio format
int  ac97_set_sample_rate(uint32_t rate);
uint32_t ac97_get_sample_rate(void);

// Simple tone generation (for system sounds)
void ac97_beep(uint32_t frequency, uint32_t duration_ms);
void ac97_play_tone(uint32_t freq, uint32_t duration_ms, int volume);

// IRQ handler
void ac97_irq_handler(void);

#endif
