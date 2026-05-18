/*
 * Sound Blaster 16 driver — 8-bit DMA, unsigned mono output.
 *
 * MOSS Kernel – i386
 *
 * Uses ISA DMA channel 1, IRQ 5, I/O base 0x220 (SB16 defaults).
 * Double-buffered: DMA hardware plays one half of the buffer while
 * the IRQ handler fills the other half from a software ring buffer.
 *
 * Userspace writes mixed audio into the ring buffer via a syscall.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/io.h>

#include "sb16.h"
#include "idt.h"
#include "pic.h"

/* ------------------------------------------------------------------ */
/*  SB16 I/O ports (default base 0x220)                               */
/* ------------------------------------------------------------------ */

#define SB_BASE          0x220
#define SB_MIXER_ADDR    (SB_BASE + 0x04)
#define SB_MIXER_DATA    (SB_BASE + 0x05)
#define SB_DSP_RESET     (SB_BASE + 0x06)
#define SB_DSP_READ      (SB_BASE + 0x0A)
#define SB_DSP_WRITE     (SB_BASE + 0x0C)
#define SB_DSP_RSTATUS   (SB_BASE + 0x0E)  /* read-status / 8-bit IRQ ack */

/* ------------------------------------------------------------------ */
/*  8237A DMA controller ports — channel 1 (8-bit)                    */
/* ------------------------------------------------------------------ */

#define DMA1_ADDR        0x02   /* address register (low/high byte) */
#define DMA1_COUNT       0x03   /* count   register (low/high byte) */
#define DMA1_PAGE        0x83   /* page    register */
#define DMA_MASK         0x0A   /* single-channel mask register     */
#define DMA_MODE         0x0B   /* mode    register                 */
#define DMA_FLIPFLOP     0x0C   /* clear byte-pointer flip-flop     */

/* ------------------------------------------------------------------ */
/*  DSP command bytes                                                 */
/* ------------------------------------------------------------------ */

#define DSP_SET_RATE     0x40   /* set time constant                */
#define DSP_SET_RATE16   0x41   /* set output sample rate (SB16)    */
#define DSP_DMA8_AI_OUT  0xC6   /* 8-bit auto-init D/A output      */
#define DSP_DMA8_EXIT    0xDA   /* exit 8-bit auto-init             */
#define DSP_SPEAKER_ON   0xD1
#define DSP_SPEAKER_OFF  0xD3
#define DSP_GET_VERSION  0xE1

#define SB_IRQ           5      /* IRQ line (default)               */
#define SB_IRQ_VECTOR    (32 + SB_IRQ)

/* ------------------------------------------------------------------ */
/*  DMA buffer — must reside in first 16 MiB and not cross 64 KiB    */
/*  boundary.  Aligned to its own size guarantees both.               */
/* ------------------------------------------------------------------ */

#define DMA_BUF_SIZE     8192
#define DMA_HALF_SIZE    (DMA_BUF_SIZE / 2)

static uint8_t dma_buffer[DMA_BUF_SIZE]
    __attribute__((aligned(DMA_BUF_SIZE)));

/* ------------------------------------------------------------------ */
/*  Software ring buffer (userspace → IRQ handler)                    */
/* ------------------------------------------------------------------ */

#define RING_SIZE        32768

static uint8_t ring_buf[RING_SIZE];
static volatile uint32_t ring_rd = 0;   /* consumer (IRQ handler)   */
static volatile uint32_t ring_wr = 0;   /* producer (syscall)       */

/* ------------------------------------------------------------------ */
/*  State                                                             */
/* ------------------------------------------------------------------ */

static int  sb16_found   = 0;
static int  sb16_playing = 0;

/* ------------------------------------------------------------------ */
/*  Low-level DSP helpers                                             */
/* ------------------------------------------------------------------ */

static void dsp_write(uint8_t val)
{
    /* Wait until the DSP is ready to accept a byte */
    for (int i = 0; i < 0xFFFF; i++) {
        if (!(inb(SB_DSP_WRITE) & 0x80))
            break;
    }
    outb(SB_DSP_WRITE, val);
}

static uint8_t dsp_read(void)
{
    for (int i = 0; i < 0xFFFF; i++) {
        if (inb(SB_DSP_RSTATUS) & 0x80)
            return inb(SB_DSP_READ);
    }
    return 0;
}

static int dsp_reset(void)
{
    outb(SB_DSP_RESET, 1);
    /* Delay ≥ 3 µs */
    for (volatile int i = 0; i < 1000; i++)
        ;
    outb(SB_DSP_RESET, 0);

    /* Wait for ready byte 0xAA */
    for (int i = 0; i < 0xFFFF; i++) {
        if (inb(SB_DSP_RSTATUS) & 0x80) {
            if (inb(SB_DSP_READ) == 0xAA)
                return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  DMA channel 1 programming                                        */
/* ------------------------------------------------------------------ */

static void dma_setup(uint32_t phys_addr, uint32_t length)
{
    uint32_t count = length - 1;

    /* Mask (disable) channel 1 */
    outb(DMA_MASK, 0x05);          /* bit 2 = mask, bits 0-1 = ch 1 */

    /* Clear byte-pointer flip-flop */
    outb(DMA_FLIPFLOP, 0x00);

    /* Mode: ch 1, auto-init, read-from-memory (playback), single */
    outb(DMA_MODE, 0x59);

    /* Low 16 bits of address */
    outb(DMA1_ADDR, (uint8_t)(phys_addr & 0xFF));
    outb(DMA1_ADDR, (uint8_t)((phys_addr >> 8) & 0xFF));

    /* Page (bits 16-23 of address) */
    outb(DMA1_PAGE, (uint8_t)((phys_addr >> 16) & 0xFF));

    /* Transfer count */
    outb(DMA1_COUNT, (uint8_t)(count & 0xFF));
    outb(DMA1_COUNT, (uint8_t)((count >> 8) & 0xFF));

    /* Unmask channel 1 */
    outb(DMA_MASK, 0x01);
}

/* ------------------------------------------------------------------ */
/*  Ring-buffer helpers                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t ring_data_avail(void)
{
    return (ring_wr - ring_rd + RING_SIZE) % RING_SIZE;
}

/* Copy up to `len` bytes from ring buf into `dst`.
 * Called from IRQ context — must be fast. */
static uint32_t ring_consume(uint8_t *dst, uint32_t len)
{
    uint32_t avail = ring_data_avail();
    if (len > avail)
        len = avail;
    if (len == 0)
        return 0;

    uint32_t first = RING_SIZE - ring_rd;
    if (first >= len) {
        memcpy(dst, ring_buf + ring_rd, len);
    } else {
        memcpy(dst, ring_buf + ring_rd, first);
        memcpy(dst + first, ring_buf, len - first);
    }
    ring_rd = (ring_rd + len) % RING_SIZE;
    return len;
}

/* ------------------------------------------------------------------ */
/*  IRQ handler — fires every DMA_HALF_SIZE samples                   */
/* ------------------------------------------------------------------ */

static volatile int dma_half = 0;   /* which half to fill (0 or 1)  */

static volatile uint32_t sb16_irq_count = 0;

static void sb16_irq_handler(struct isr_regs *regs)
{
    (void)regs;

    sb16_irq_count++;

    /* Acknowledge the SB16 interrupt */
    inb(SB_DSP_RSTATUS);

    /* Fill the half that just finished playing */
    uint8_t *half = dma_buffer + dma_half * DMA_HALF_SIZE;

    uint32_t got = ring_consume(half, DMA_HALF_SIZE);
    /* Pad with silence (128 for unsigned 8-bit) if underrun */
    if (got < DMA_HALF_SIZE)
        memset(half + got, 128, DMA_HALF_SIZE - got);

    dma_half ^= 1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int sb16_init(void)
{
    if (dsp_reset() < 0)
        return -1;

    /* Read DSP version — SB16 is version 4.xx+ */
    dsp_write(DSP_GET_VERSION);
    uint8_t major = dsp_read();
    uint8_t minor = dsp_read();
    (void)minor;

    if (major < 4)
        return -1;

    printf("[sb16] DSP version %d.%02d detected\n", major, minor);

    /* Register IRQ handler (EOI is sent by the IDT dispatcher) */
    isr_register_handler(SB_IRQ_VECTOR, sb16_irq_handler);
    /* IRQ is unmasked when playback starts */

    /* Configure mixer: use IRQ 5, DMA channel 1 */
    outb(SB_MIXER_ADDR, 0x80);         /* IRQ select register  */
    outb(SB_MIXER_DATA, 0x02);         /* bit 1 = IRQ 5        */

    outb(SB_MIXER_ADDR, 0x81);         /* DMA select register  */
    outb(SB_MIXER_DATA, 0x02);         /* bit 1 = DMA 1        */

    /* Master volume max */
    outb(SB_MIXER_ADDR, 0x22);
    outb(SB_MIXER_DATA, 0xFF);

    /* Voice (PCM) volume max */
    outb(SB_MIXER_ADDR, 0x04);
    outb(SB_MIXER_DATA, 0xFF);

    /* Silence the DMA buffer */
    memset(dma_buffer, 128, DMA_BUF_SIZE);
    ring_rd = ring_wr = 0;

    sb16_found = 1;
    printf("[sb16] init complete, sb16_found=1\n");
    return 0;
}

void sb16_start(uint32_t sample_rate)
{
    if (!sb16_found) {
        printf("[sb16] not found, skipping start\n");
        return;
    }

    dsp_write(DSP_SPEAKER_ON);
    pic_clear_mask(SB_IRQ);

    /* Set output sample rate (SB16 command 0x41) */
    dsp_write(DSP_SET_RATE16);
    dsp_write((uint8_t)((sample_rate >> 8) & 0xFF));
    dsp_write((uint8_t)(sample_rate & 0xFF));

    /* Program DMA for the full buffer in auto-init mode */
    dma_setup((uint32_t)dma_buffer, DMA_BUF_SIZE);

    /* Tell DSP to transfer half-buffer at a time (IRQ per half) */
    dsp_write(DSP_DMA8_AI_OUT);
    dsp_write(0x00);  /* mode: unsigned mono */
    uint32_t half_count = DMA_HALF_SIZE - 1;
    dsp_write((uint8_t)(half_count & 0xFF));
    dsp_write((uint8_t)((half_count >> 8) & 0xFF));

    dma_half = 0;
    sb16_playing = 1;

    printf("[sb16] Playback started at %u Hz\n", sample_rate);
}

void sb16_stop(void)
{
    if (!sb16_found || !sb16_playing)
        return;

    dsp_write(DSP_DMA8_EXIT);
    dsp_write(DSP_SPEAKER_OFF);

    sb16_playing = 0;
}

uint32_t sb16_write(const uint8_t *buf, uint32_t len)
{
    /* Free space = RING_SIZE - 1 - data_available */
    uint32_t used  = ring_data_avail();
    uint32_t space = RING_SIZE - 1 - used;
    if (len > space)
        len = space;
    if (len == 0)
        return 0;

    uint32_t first = RING_SIZE - ring_wr;
    if (first >= len) {
        memcpy(ring_buf + ring_wr, buf, len);
    } else {
        memcpy(ring_buf + ring_wr, buf, first);
        memcpy(ring_buf, buf + first, len - first);
    }
    ring_wr = (ring_wr + len) % RING_SIZE;
    return len;
}

uint32_t sb16_avail(void)
{
    uint32_t used = ring_data_avail();
    return RING_SIZE - 1 - used;
}
