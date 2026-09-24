/* Sound: Intel AC'97 driver, software mixer, /dev/audio and system sounds */
#include <kernel.h>
#include <pci.h>
#include <mm.h>
#include <vfs.h>
#include <sched.h>
#include <syscall.h>
#include <claudeos/wav.h>
#include "drivers.h"

#define NBUF 32
#define BUF_FRAMES 1024                 /* 21.3 ms at 48 kHz */
#define BUF_BYTES (BUF_FRAMES * 4)
#define LEAD 4                          /* buffers queued ahead of the hardware */
#define MAX_STREAMS 8
#define RING_FRAMES (48000 / 2)         /* 0.5 s per user stream */

typedef struct { uint32_t addr; uint16_t samples; uint16_t flags; } __attribute__((packed)) bdl_t;

typedef struct stream {
    bool used, closing;
    /* ring stream (from /dev/audio) */
    int16_t *ring;                      /* stereo frames */
    uint32_t head, tail, count;         /* in frames */
    /* one-shot stream (system sound) */
    int16_t *data;
    uint32_t len, pos;
    int volume;                         /* 0..256 */
    waitq_t wq;
} stream_t;

static struct {
    bool present;
    uint16_t nam, nabm;
    bdl_t *bdl;
    uint64_t bdl_phys;
    int16_t *bufs;
    uint64_t bufs_phys;
    bool running;
    int next_fill;                      /* next buffer index to fill */
    int master;                         /* 0..100 */
    stream_t streams[MAX_STREAMS];
    waitq_t wq;
    mutex_t lock;
} snd;

/* ------------------------------------------------------------------ hardware */
#define NABM_PO_BDBAR 0x10
#define NABM_PO_CIV 0x14
#define NABM_PO_LVI 0x15
#define NABM_PO_SR 0x16
#define NABM_PO_CR 0x1B
#define NABM_GLOB_CNT 0x2C
#define NABM_GLOB_STA 0x30

static void hw_set_volume(int v) {
    if (!snd.present) return;
    /* software volume does the scaling; keep the codec at 0 dB and only mute at 0 */
    uint16_t reg = v == 0 ? 0x8000 : 0x0000;
    outw(snd.nam + 0x02, reg);          /* master */
    outw(snd.nam + 0x18, v == 0 ? 0x8808 : 0x0808);   /* PCM out */
}

static void hw_start(void) {
    if (snd.running) return;
    outb(snd.nabm + NABM_PO_CR, 0x02);  /* reset the PCM out box */
    for (int i = 0; i < 1000 && (inb(snd.nabm + NABM_PO_CR) & 0x02); i++) io_wait();
    outl(snd.nabm + NABM_PO_BDBAR, (uint32_t)snd.bdl_phys);
    memset(snd.bufs, 0, NBUF * BUF_BYTES);
    snd.next_fill = 0;
    outb(snd.nabm + NABM_PO_LVI, NBUF - 1);
    outb(snd.nabm + NABM_PO_CR, 0x01);  /* run, no interrupts: the audio thread polls */
    snd.running = true;
}

static void hw_stop(void) {
    if (!snd.running) return;
    outb(snd.nabm + NABM_PO_CR, 0x00);
    snd.running = false;
}

/* ------------------------------------------------------------------ mixing */
static bool any_active(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &snd.streams[i];
        if (!s->used) continue;
        if (s->ring && (s->count || !s->closing)) return true;
        if (s->data && s->pos < s->len) return true;
    }
    return false;
}

static void mix_buffer(int16_t *out) {
    int32_t acc[BUF_FRAMES * 2];
    memset(acc, 0, sizeof(acc));
    int gain = snd.master * 256 / 100;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &snd.streams[i];
        if (!s->used) continue;
        int vol = s->volume * gain / 256;
        if (s->ring) {
            uint32_t n = MIN(s->count, (uint32_t)BUF_FRAMES);
            for (uint32_t f = 0; f < n; f++) {
                acc[f * 2] += s->ring[s->tail * 2] * vol / 256;
                acc[f * 2 + 1] += s->ring[s->tail * 2 + 1] * vol / 256;
                s->tail = (s->tail + 1) % RING_FRAMES;
            }
            s->count -= n;
            if (n) wq_wake_all(&s->wq);
            if (s->closing && !s->count) {
                vfree(s->ring);
                s->ring = 0;
                s->used = false;
            }
        } else if (s->data) {
            uint32_t n = MIN(s->len - s->pos, (uint32_t)BUF_FRAMES);
            for (uint32_t f = 0; f < n; f++) {
                acc[f * 2] += s->data[(s->pos + f) * 2] * vol / 256;
                acc[f * 2 + 1] += s->data[(s->pos + f) * 2 + 1] * vol / 256;
            }
            s->pos += n;
            if (s->pos >= s->len) {
                vfree(s->data);
                s->data = 0;
                s->used = false;
            }
        }
    }
    for (int i = 0; i < BUF_FRAMES * 2; i++) {
        int32_t v = acc[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static int audio_thread(void *arg) {
    UNUSED(arg);
    uint64_t idle_since = 0;
    for (;;) {
        mutex_lock(&snd.lock);
        bool active = any_active();
        if (active && !snd.running) hw_start();
        if (snd.running) {
            int civ = inb(snd.nabm + NABM_PO_CIV) & 31;
            /* fill buffers up to LEAD ahead of the current index */
            int target = (civ + LEAD) % NBUF;
            while (snd.next_fill != target) {
                /* never overwrite the buffer being played */
                if (snd.next_fill == civ) { snd.next_fill = (civ + 1) % NBUF; continue; }
                mix_buffer(snd.bufs + snd.next_fill * BUF_FRAMES * 2);
                snd.next_fill = (snd.next_fill + 1) % NBUF;
            }
            outb(snd.nabm + NABM_PO_LVI, (uint8_t)((civ + NBUF - 1) % NBUF));
            outw(snd.nabm + NABM_PO_SR, 0x1C);   /* clear status bits */
            if (!active) {
                if (!idle_since) idle_since = uptime_ms();
                else if (uptime_ms() - idle_since > 1500) hw_stop();
            } else {
                idle_since = 0;
            }
        }
        bool running = snd.running;
        mutex_unlock(&snd.lock);
        if (running) sleep_ms(8);
        else {
            uint64_t f = irq_save();
            if (!any_active()) wq_wait_timeout(&snd.wq, 1000);
            irq_restore(f);
        }
    }
    return 0;
}

static stream_t *stream_alloc(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &snd.streams[i];
        if (!s->used) {
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->volume = 256;
            return s;
        }
    }
    return 0;
}

static void kick(void) {
    uint64_t f = irq_save();
    wq_wake_all(&snd.wq);
    irq_restore(f);
}

/* play a WAV file asynchronously (system sounds) */
int snd_play_file_ret(const char *path) {
    if (!snd.present) return -ENODEV;
    size_t size;
    void *file = vfs_read_all(path, &size);
    if (!file) return -ENOENT;
    wav_info_t w;
    if (!wav_parse(file, size, &w)) { kfree(file); return -EINVAL; }
    uint32_t frames = wav_out_frames(&w);
    int16_t *pcm = vmalloc((size_t)frames * 4 + 16);
    wav_convert(&w, pcm);
    kfree(file);
    mutex_lock(&snd.lock);
    stream_t *s = stream_alloc();
    if (!s) { mutex_unlock(&snd.lock); vfree(pcm); return -EBUSY; }
    s->data = pcm;
    s->len = frames;
    s->pos = 0;
    mutex_unlock(&snd.lock);
    kick();
    return 0;
}

void snd_play_file(const char *path) { snd_play_file_ret(path); }

void snd_set_volume(int v) {
    snd.master = MAX(0, MIN(100, v));
    if (snd.present) {
        mutex_lock(&snd.lock);
        hw_set_volume(snd.master);
        mutex_unlock(&snd.lock);
    }
}

bool snd_present(void) { return snd.present; }

/* ------------------------------------------------------------------ /dev/audio */
static int a_open(vnode_t *vn, file_t *f) {
    if (!snd.present) return -ENODEV;
    mutex_lock(&snd.lock);
    stream_t *s = stream_alloc();
    if (s) {
        s->ring = vmalloc(RING_FRAMES * 4);
        if (!s->ring) { s->used = false; s = 0; }
    }
    mutex_unlock(&snd.lock);
    if (!s) return -EBUSY;
    f->priv = s;
    return 0;
}

static long a_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    stream_t *s = f->priv;
    const int16_t *in = buf;
    size_t frames = n / 4, done = 0;
    while (done < frames) {
        mutex_lock(&snd.lock);
        uint32_t space = RING_FRAMES - s->count;
        uint32_t k = (uint32_t)MIN(space, frames - done);
        for (uint32_t i = 0; i < k; i++) {
            s->ring[s->head * 2] = in[(done + i) * 2];
            s->ring[s->head * 2 + 1] = in[(done + i) * 2 + 1];
            s->head = (s->head + 1) % RING_FRAMES;
        }
        s->count += k;
        done += k;
        mutex_unlock(&snd.lock);
        kick();
        if (done < frames) {
            if (f->flags & O_NONBLOCK) break;
            if (current->killed) break;
            uint64_t fl = irq_save();
            if (s->count >= RING_FRAMES) wq_wait_timeout(&s->wq, 100);
            irq_restore(fl);
        }
    }
    if (done == 0 && frames) return -EAGAIN;
    return (long)(done * 4);
}

static int a_ioctl(vnode_t *vn, file_t *f, unsigned long req, void *arg) {
    stream_t *s = f->priv;
    if (req == FIONREAD) { *(int *)arg = (int)(s->count * 4); return 0; }
    return -ENOTTY;
}

static int a_poll(vnode_t *vn, file_t *f) {
    stream_t *s = f->priv;
    return s->count < RING_FRAMES ? POLLOUT : 0;
}

static void a_close(vnode_t *vn, file_t *f) {
    stream_t *s = f->priv;
    if (!s) return;
    mutex_lock(&snd.lock);
    s->closing = true;
    if (!s->count) {
        vfree(s->ring);
        s->ring = 0;
        s->used = false;
    }
    mutex_unlock(&snd.lock);
}

static const vnode_ops_t audio_ops = { .open = a_open, .write = a_write, .ioctl = a_ioctl, .poll = a_poll, .close = a_close };

/* ------------------------------------------------------------------ syscalls */
SYSCALL_DEF(sys_sound_play) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN];
    if (strncpy_from_user(path, (const char *)a1, sizeof(path)) < 0) return -EFAULT;
    char abs[PATH_MAX_LEN];
    if (vfs_normalize(current->cwd, path, abs) < 0) return -EINVAL;
    return snd_play_file_ret(abs);
}

SYSCALL_DEF(sys_audio_volume) {
    SYSCALL_UNUSED_ARGS;
    if ((int64_t)a1 >= 0) snd_set_volume((int)a1);
    return snd.present ? snd.master : -ENODEV;
}

/* ------------------------------------------------------------------ init */
void audio_init(void) {
    syscall_register(SYS_SOUND_PLAY, sys_sound_play);
    syscall_register(SYS_AUDIO_VOLUME, sys_audio_volume);
    snd.master = 70;
    pci_dev_t *d = pci_find(0x8086, 0x2415);
    if (!d) d = pci_find_class(0x04, 0x01);
    if (!d || !(d->bar[0] & 1) || !(d->bar[1] & 1)) {
        klog("[snd] no AC'97 sound card found\n");
        return;
    }
    pci_enable_bus_master(d);
    snd.nam = d->bar[0] & ~3u;
    snd.nabm = d->bar[1] & ~3u;
    /* cold reset, then wait for the codec */
    outl(snd.nabm + NABM_GLOB_CNT, 0x00000002);
    for (int i = 0; i < 100 && !(inl(snd.nabm + NABM_GLOB_STA) & 0x100); i++) pit_delay_ms(1);
    outw(snd.nam + 0x00, 0);            /* codec reset */
    pit_delay_ms(1);
    /* variable rate: ask for 48 kHz */
    uint16_t ext = inw(snd.nam + 0x28);
    if (ext & 1) {
        outw(snd.nam + 0x2A, inw(snd.nam + 0x2A) | 1);
        outw(snd.nam + 0x2C, 48000);
    }
    uint64_t bdl = pmm_alloc_contig(1, 0x100000000ULL);
    uint64_t bufs = pmm_alloc_contig((NBUF * BUF_BYTES + 4095) / 4096, 0x100000000ULL);
    if (!bdl || !bufs) { klog("[snd] out of DMA memory\n"); return; }
    snd.bdl = P2V(bdl);
    snd.bdl_phys = bdl;
    snd.bufs = P2V(bufs);
    snd.bufs_phys = bufs;
    for (int i = 0; i < NBUF; i++) {
        snd.bdl[i].addr = (uint32_t)(bufs + i * BUF_BYTES);
        snd.bdl[i].samples = BUF_FRAMES * 2;
        snd.bdl[i].flags = 0;
    }
    snd.present = true;
    hw_set_volume(snd.master);
    devfs_register("audio", &audio_ops, 0);
    task_t *t = kthread_create("audio", audio_thread, 0);
    t->prio = 1;
    klog("[snd] AC'97 at %x/%x, %s\n", snd.nam, snd.nabm, (ext & 1) ? "variable rate" : "48 kHz fixed");
}
