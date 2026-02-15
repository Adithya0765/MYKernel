// nv_fifo.h - NVIDIA PFIFO Command Submission Engine for Alteo OS
// PFIFO manages GPU command channels - the bridge between CPU and GPU engines
// Pre-NV50: DMA push buffer channels with subchannel object binding
// NV50+: Indirect push buffer with semaphore synchronization
#ifndef NV_FIFO_H
#define NV_FIFO_H

#include "stdint.h"
#include "gpu.h"

// ============================================================
// PFIFO Register Offsets (BAR0)
// ============================================================

// Core PFIFO registers
#define NV_PFIFO_DELAY_0         0x002040    // DMA fetch delay
#define NV_PFIFO_DMA_TIMESLICE   0x002044    // DMA timeslice
#define NV_PFIFO_NEXT_CHANNEL    0x002050    // Next channel to service
#define NV_PFIFO_INTR_0          0x002100    // PFIFO interrupt status
#define NV_PFIFO_INTR_EN_0       0x002140    // PFIFO interrupt enable

// PFIFO modes
#define NV_PFIFO_MODE            0x002504    // Channel mode register (PIO vs DMA per channel)
#define NV_PFIFO_DMA             0x002508    // DMA channel enable
#define NV_PFIFO_CACHE1_PUSH0    0x003200    // Cache1 push access enable
#define NV_PFIFO_CACHE1_PUSH1    0x003204    // Cache1 push channel ID
#define NV_PFIFO_CACHE1_DMAS     0x003220    // Cache1 DMA state
#define NV_PFIFO_CACHE1_DMA_PUT  0x003240    // DMA push buffer PUT offset
#define NV_PFIFO_CACHE1_DMA_GET  0x003244    // DMA push buffer GET offset
#define NV_PFIFO_CACHE1_ENGINE   0x003280    // Engine binding per subchannel
#define NV_PFIFO_CACHE1_DMA_CTL  0x003228    // DMA control
#define NV_PFIFO_CACHE1_DMA_LEN  0x003230    // DMA push buffer length
#define NV_PFIFO_CACHE1_STATUS   0x003214    // Cache1 status

// PFIFO reassign
#define NV_PFIFO_REASSIGN        0x002500    // 0=enabled, 1=disabled

// NV50+ PFIFO registers
#define NV50_PFIFO_PLAYLIST_0    0x002070    // Playlist table address (low)
#define NV50_PFIFO_PLAYLIST_1    0x002074    // Playlist table address (high) + count
#define NV50_PFIFO_CHAN_TABLE     0x002600    // Channel table base

// ============================================================
// FIFO Channel Configuration
// ============================================================

#define NV_FIFO_MAX_CHANNELS     32
#define NV_FIFO_PUSHBUF_SIZE     (64 * 1024)    // 64 KB push buffer per channel
#define NV_FIFO_MAX_SUBCHANNELS  8              // 8 subchannels per channel

// Push buffer command encoding
// NVIDIA push buffer format:
//   Method header: bits[28:29] = type (0=non-inc, 1=inc, 2=jump)
//                  bits[15:13] = subchannel (0-7)
//                  bits[12:2]  = method (offset / 4)
//                  bits[1:0]   = always 0
//                  bits[28:18] = count (for multi-method writes)
#define NV_FIFO_MTHD(subchan, method, count) \
    (0x00000000 | ((uint32_t)(count) << 18) | ((uint32_t)(subchan) << 13) | ((uint32_t)(method) & 0x1FFC))

// Incrementing method header (writes to method, method+4, method+8, ...)
#define NV_FIFO_MTHD_INC(subchan, method, count) \
    (0x20000000 | ((uint32_t)(count) << 18) | ((uint32_t)(subchan) << 13) | ((uint32_t)(method) & 0x1FFC))

// Non-incrementing header (all data goes to same method)
#define NV_FIFO_MTHD_NI(subchan, method, count) \
    (0x40000000 | ((uint32_t)(count) << 18) | ((uint32_t)(subchan) << 13) | ((uint32_t)(method) & 0x1FFC))

// Subchannel assignments (convention)
#define NV_FIFO_SUBCHAN_NULL     0   // Null/NOP object
#define NV_FIFO_SUBCHAN_2D       1   // 2D engine
#define NV_FIFO_SUBCHAN_M2MF     2   // Memory-to-memory copy engine
#define NV_FIFO_SUBCHAN_3D       3   // 3D engine
#define NV_FIFO_SUBCHAN_COMPUTE  4   // Compute engine (NV50+)

// ============================================================
// GPU Engine IDs
// ============================================================

#define NV_ENGINE_SW             0x00    // Software engine (CPU)
#define NV_ENGINE_GR             0x01    // Graphics engine (PGRAPH)
#define NV_ENGINE_MPEG           0x02    // MPEG decoder
#define NV_ENGINE_ME             0x03    // Motion estimation
#define NV_ENGINE_VP             0x04    // Video processor
#define NV_ENGINE_COPY           0x05    // DMA copy engine
#define NV_ENGINE_BSP            0x06    // Bitstream processor

// ============================================================
// Channel State
// ============================================================

typedef struct {
    int      active;
    int      channel_id;

    // Push buffer
    uint32_t* pushbuf;          // Virtual address of push buffer (kernel-mapped)
    uint64_t  pushbuf_phys;     // Physical address of push buffer
    uint32_t  pushbuf_size;     // Size in bytes
    uint32_t  pushbuf_put;      // Current PUT position (dwords offset)
    uint32_t  pushbuf_get;      // Last known GET position

    // Subchannel object bindings
    uint32_t  subchan_class[NV_FIFO_MAX_SUBCHANNELS];

    // Fence / synchronization
    uint32_t  fence_sequence;   // Monotonically increasing fence value
    volatile uint32_t* fence_mem;  // Fence completion memory (GPU writes here)
    uint64_t  fence_mem_phys;
} nv_fifo_channel_t;

// FIFO global state
typedef struct {
    int      initialized;
    int      num_channels;
    int      active_channel;
    nv_fifo_channel_t channels[NV_FIFO_MAX_CHANNELS];
} nv_fifo_state_t;

// ---- FIFO Initialization ----
int  nv_fifo_init(void);
void nv_fifo_shutdown(void);
nv_fifo_state_t* nv_fifo_get_state(void);

// ---- Channel Management ----
int  nv_fifo_channel_alloc(int channel_id);     // Allocate and initialize a FIFO channel
void nv_fifo_channel_free(int channel_id);      // Free a FIFO channel
int  nv_fifo_channel_bind_object(int channel_id, int subchan, uint32_t obj_class);

// ---- Push Buffer Operations ----
void nv_fifo_push(int channel_id, uint32_t data);      // Push one dword
void nv_fifo_push_method(int channel_id, int subchan, uint32_t method, uint32_t data);
void nv_fifo_push_method_n(int channel_id, int subchan, uint32_t method, uint32_t* data, int count);
void nv_fifo_kick(int channel_id);                      // Flush push buffer to GPU

// ---- Synchronization ----
void nv_fifo_emit_fence(int channel_id);                // Insert fence in command stream
int  nv_fifo_fence_completed(int channel_id, uint32_t seq);  // Check if fence was reached
void nv_fifo_wait_fence(int channel_id, uint32_t seq);  // Busy-wait for fence

// ---- Utility ----
void nv_fifo_wait_idle(int channel_id);                 // Wait for channel to drain
uint32_t nv_fifo_space_available(int channel_id);       // Dwords free in push buffer

#endif
