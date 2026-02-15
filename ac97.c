// ac97.c - AC'97 Audio Codec Driver for Alteo OS
#include "ac97.h"
#include "heap.h"
#include "pci.h"

// Port I/O
static inline uint8_t ac97_inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void ac97_outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t ac97_inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void ac97_outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t ac97_inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void ac97_outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

// Global device
static ac97_device_t ac97_dev = {0};

static void ac97_memset(void* dst, uint8_t val, int n) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

// Scan PCI for AC97 controller (uses central PCI enumerator)
static int ac97_pci_scan(void) {
    // Find any audio device (class 0x04, subclass 0x01)
    pci_device_t* dev = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, (pci_device_t*)0);
    if (!dev) return 0;

    // BAR0 = mixer, BAR1 = busmaster
    if (dev->bars[0].present && dev->bars[0].type == 1)
        ac97_dev.mixer_base = (uint16_t)dev->bars[0].base;
    if (dev->bars[1].present && dev->bars[1].type == 1)
        ac97_dev.busmaster_base = (uint16_t)dev->bars[1].base;

    ac97_dev.irq = dev->irq_line;

    // Enable bus mastering + I/O space
    pci_enable_bus_master(dev);
    pci_enable_io_space(dev);

    return 1;
}

int ac97_init(void) {
    ac97_memset(&ac97_dev, 0, sizeof(ac97_device_t));
    ac97_dev.master_volume = 80;
    ac97_dev.pcm_volume = 80;
    ac97_dev.sample_rate = AUDIO_SAMPLE_RATE_48000;
    ac97_dev.channels = AUDIO_CHANNELS_STEREO;
    ac97_dev.bits_per_sample = AUDIO_BITS_16;

    if (!ac97_pci_scan()) {
        ac97_dev.available = 0;
        return -1;
    }

    // Cold reset
    ac97_outl(ac97_dev.busmaster_base + AC97_BM_GLB_CR, AC97_GCR_COLD_RST);
    for (volatile int i = 0; i < 100000; i++); // Wait

    // Enable global interrupts
    ac97_outl(ac97_dev.busmaster_base + AC97_BM_GLB_CR, AC97_GCR_GIE | AC97_GCR_COLD_RST);

    // Reset codec
    ac97_outw(ac97_dev.mixer_base + AC97_RESET, 0);
    for (volatile int i = 0; i < 100000; i++);

    // Set master volume
    ac97_set_master_volume(ac97_dev.master_volume);
    ac97_set_pcm_volume(ac97_dev.pcm_volume);

    // Enable variable rate audio if supported
    uint16_t ext_id = ac97_inw(ac97_dev.mixer_base + AC97_EXT_AUDIO_ID);
    if (ext_id & 1) {
        // VRA supported
        uint16_t ext_ctrl = ac97_inw(ac97_dev.mixer_base + AC97_EXT_AUDIO_CTRL);
        ext_ctrl |= 1; // Enable VRA
        ac97_outw(ac97_dev.mixer_base + AC97_EXT_AUDIO_CTRL, ext_ctrl);
        ac97_set_sample_rate(ac97_dev.sample_rate);
    }

    // Allocate BDL
    ac97_dev.play_bdl = (ac97_bdl_entry_t*)kmalloc(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES + 16);
    if (!ac97_dev.play_bdl) return -1;

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        ac97_dev.play_bufs[i] = (uint8_t*)kmalloc(AC97_BDL_BUF_SIZE + 16);
        if (!ac97_dev.play_bufs[i]) return -1;
        ac97_memset(ac97_dev.play_bufs[i], 0, AC97_BDL_BUF_SIZE);

        ac97_dev.play_bdl[i].addr = (uint32_t)(uintptr_t)ac97_dev.play_bufs[i];
        ac97_dev.play_bdl[i].length = AC97_BDL_BUF_SIZE / 2; // In samples
        ac97_dev.play_bdl[i].flags = AC97_BDL_IOC;
    }

    // Set BDL address for playback
    ac97_outl(ac97_dev.busmaster_base + AC97_BM_PO_BDBAR,
              (uint32_t)(uintptr_t)ac97_dev.play_bdl);

    ac97_dev.available = 1;
    ac97_dev.play_state = AUDIO_STOPPED;
    return 0;
}

int ac97_is_available(void) {
    return ac97_dev.available;
}

void ac97_set_master_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    ac97_dev.master_volume = volume;

    if (!ac97_dev.available) return;

    // AC97 volume: 0 = max, 63 = min (or mute at bit 15)
    int attenuation = 63 - (volume * 63) / 100;
    uint16_t val = (uint16_t)((attenuation << 8) | attenuation);
    if (ac97_dev.muted) val |= 0x8000;
    ac97_outw(ac97_dev.mixer_base + AC97_MASTER_VOL, val);
}

void ac97_set_pcm_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    ac97_dev.pcm_volume = volume;

    if (!ac97_dev.available) return;

    int attenuation = 31 - (volume * 31) / 100;
    uint16_t val = (uint16_t)((attenuation << 8) | attenuation);
    if (ac97_dev.muted) val |= 0x8000;
    ac97_outw(ac97_dev.mixer_base + AC97_PCM_OUT_VOL, val);
}

int ac97_get_master_volume(void) {
    return ac97_dev.master_volume;
}

int ac97_get_pcm_volume(void) {
    return ac97_dev.pcm_volume;
}

void ac97_mute(int mute) {
    ac97_dev.muted = mute;
    ac97_set_master_volume(ac97_dev.master_volume);
    ac97_set_pcm_volume(ac97_dev.pcm_volume);
}

int ac97_is_muted(void) {
    return ac97_dev.muted;
}

int ac97_play(const uint8_t* samples, uint32_t num_samples) {
    if (!ac97_dev.available || !samples || num_samples == 0)
        return -1;

    // Fill buffers
    uint32_t bytes = num_samples * (ac97_dev.bits_per_sample / 8) * ac97_dev.channels;
    uint32_t offset = 0;
    int buf_idx = 0;

    while (offset < bytes && buf_idx < AC97_BDL_ENTRIES) {
        uint32_t chunk = bytes - offset;
        if (chunk > AC97_BDL_BUF_SIZE) chunk = AC97_BDL_BUF_SIZE;

        uint8_t* dst = ac97_dev.play_bufs[buf_idx];
        const uint8_t* src = samples + offset;
        for (uint32_t i = 0; i < chunk; i++) dst[i] = src[i];

        ac97_dev.play_bdl[buf_idx].length = (uint16_t)(chunk / 2);
        ac97_dev.play_bdl[buf_idx].flags = AC97_BDL_IOC;

        offset += chunk;
        buf_idx++;
    }

    if (buf_idx == 0) return -1;

    // Set last valid index
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_LVI, (uint8_t)(buf_idx - 1));

    // Enable playback
    uint8_t cr = AC97_CR_RPBM | AC97_CR_LVBIE | AC97_CR_IOCE;
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, cr);

    ac97_dev.play_state = AUDIO_PLAYING;
    return 0;
}

void ac97_stop(void) {
    if (!ac97_dev.available) return;

    // Stop DMA
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, 0);

    // Reset
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, AC97_CR_RR);
    for (volatile int i = 0; i < 10000; i++);
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, 0);

    ac97_dev.play_state = AUDIO_STOPPED;
}

void ac97_pause(void) {
    if (!ac97_dev.available || ac97_dev.play_state != AUDIO_PLAYING) return;

    // Clear run bit
    uint8_t cr = ac97_inb(ac97_dev.busmaster_base + AC97_BM_PO_CR);
    cr &= (uint8_t)~AC97_CR_RPBM;
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, cr);

    ac97_dev.play_state = AUDIO_PAUSED;
}

void ac97_resume(void) {
    if (!ac97_dev.available || ac97_dev.play_state != AUDIO_PAUSED) return;

    uint8_t cr = ac97_inb(ac97_dev.busmaster_base + AC97_BM_PO_CR);
    cr |= AC97_CR_RPBM;
    ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, cr);

    ac97_dev.play_state = AUDIO_PLAYING;
}

audio_state_t ac97_get_state(void) {
    return ac97_dev.play_state;
}

int ac97_set_sample_rate(uint32_t rate) {
    if (!ac97_dev.available) return -1;

    ac97_outw(ac97_dev.mixer_base + AC97_PCM_FRONT_RATE, (uint16_t)rate);
    ac97_dev.sample_rate = rate;

    // Verify
    uint16_t actual = ac97_inw(ac97_dev.mixer_base + AC97_PCM_FRONT_RATE);
    if (actual != (uint16_t)rate) {
        ac97_dev.sample_rate = actual;
        return -1;
    }
    return 0;
}

uint32_t ac97_get_sample_rate(void) {
    return ac97_dev.sample_rate;
}

void ac97_beep(uint32_t frequency, uint32_t duration_ms) {
    ac97_play_tone(frequency, duration_ms, 50);
}

void ac97_play_tone(uint32_t freq, uint32_t duration_ms, int volume) {
    if (!ac97_dev.available || freq == 0) return;

    uint32_t sample_rate = ac97_dev.sample_rate;
    uint32_t num_samples = (sample_rate * duration_ms) / 1000;
    if (num_samples > AC97_BDL_BUF_SIZE * AC97_BDL_ENTRIES / 4)
        num_samples = AC97_BDL_BUF_SIZE * AC97_BDL_ENTRIES / 4;

    // Generate simple square wave
    int16_t amplitude = (int16_t)((32767 * volume) / 100);
    uint32_t period = sample_rate / freq;
    if (period == 0) period = 1;

    uint32_t buf_idx = 0;
    uint32_t buf_offset = 0;

    for (uint32_t i = 0; i < num_samples && buf_idx < (uint32_t)AC97_BDL_ENTRIES; i++) {
        int16_t sample = ((i % period) < (period / 2)) ? amplitude : -amplitude;

        // Left channel
        ac97_dev.play_bufs[buf_idx][buf_offset] = (uint8_t)(sample & 0xFF);
        ac97_dev.play_bufs[buf_idx][buf_offset + 1] = (uint8_t)((sample >> 8) & 0xFF);
        // Right channel
        ac97_dev.play_bufs[buf_idx][buf_offset + 2] = (uint8_t)(sample & 0xFF);
        ac97_dev.play_bufs[buf_idx][buf_offset + 3] = (uint8_t)((sample >> 8) & 0xFF);

        buf_offset += 4;
        if (buf_offset >= AC97_BDL_BUF_SIZE) {
            ac97_dev.play_bdl[buf_idx].length = (uint16_t)(buf_offset / 2);
            ac97_dev.play_bdl[buf_idx].flags = AC97_BDL_IOC;
            buf_idx++;
            buf_offset = 0;
        }
    }

    if (buf_offset > 0 && buf_idx < (uint32_t)AC97_BDL_ENTRIES) {
        ac97_dev.play_bdl[buf_idx].length = (uint16_t)(buf_offset / 2);
        ac97_dev.play_bdl[buf_idx].flags = AC97_BDL_IOC;
        buf_idx++;
    }

    if (buf_idx > 0) {
        ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_LVI, (uint8_t)(buf_idx - 1));
        ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR,
                  AC97_CR_RPBM | AC97_CR_LVBIE | AC97_CR_IOCE);
        ac97_dev.play_state = AUDIO_PLAYING;
    }
}

void ac97_irq_handler(void) {
    if (!ac97_dev.available) return;

    uint16_t sr = ac97_inw(ac97_dev.busmaster_base + AC97_BM_PO_SR);

    if (sr & AC97_SR_LVBCI) {
        // Last buffer completed
        ac97_dev.play_state = AUDIO_STOPPED;
        ac97_outb(ac97_dev.busmaster_base + AC97_BM_PO_CR, 0);
    }

    // Clear status
    ac97_outw(ac97_dev.busmaster_base + AC97_BM_PO_SR, sr);
}
