/*
 *  QEMU model of the Milkymist System Controller.
 *
 *  Copyright (c) 2010 Michael Walle <michael@walle.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Specification available at:
 *   http://www.milkymist.org/socdoc/ac97.pdf
 */

#include "hw.h"
#include "sysbus.h"
#include "trace.h"
#include "audio/audio.h"
#include "qemu-error.h"

enum {
    R_AC97_CTRL = 0,
    R_AC97_ADDR,
    R_AC97_DATAOUT,
    R_AC97_DATAIN,
    R_D_CTRL,
    R_D_ADDR,
    R_D_REMAINING,
    R_RESERVED,
    R_U_CTRL,
    R_U_ADDR,
    R_U_REMAINING,
    R_MAX
};

enum {
    AC97_CTRL_RQEN  = (1<<0),
    AC97_CTRL_WRITE = (1<<1),
};

enum {
    CTRL_EN = (1<<0),
};

struct MilkymistAC97State {
    SysBusDevice busdev;

    QEMUSoundCard card;
    SWVoiceIn *voice_in;
    SWVoiceOut *voice_out;

    uint32_t regs[R_MAX];

    qemu_irq crrequest_irq;
    qemu_irq crreply_irq;
    qemu_irq dmar_irq;
    qemu_irq dmaw_irq;
};
typedef struct MilkymistAC97State MilkymistAC97State;

static void update_voices(MilkymistAC97State *s)
{
    if (s->regs[R_D_CTRL] & CTRL_EN) {
        AUD_set_active_out(s->voice_out, 1);
    } else {
        AUD_set_active_out(s->voice_out, 0);
    }

    if (s->regs[R_U_CTRL] & CTRL_EN) {
        AUD_set_active_in(s->voice_in, 1);
    } else {
        AUD_set_active_in(s->voice_in, 0);
    }
}

static uint32_t ac97_read(void *opaque, target_phys_addr_t addr)
{
    MilkymistAC97State *s = opaque;
    uint32_t r = 0;

    addr >>= 2;
    switch (addr) {
    case R_AC97_CTRL:
    case R_AC97_ADDR:
    case R_AC97_DATAOUT:
    case R_AC97_DATAIN:
    case R_D_CTRL:
    case R_D_ADDR:
    case R_D_REMAINING:
    case R_U_CTRL:
    case R_U_ADDR:
    case R_U_REMAINING:
        r = s->regs[addr];
        break;

    default:
        error_report("milkymist_ac97: read access to unknown register 0x"
                TARGET_FMT_plx, addr << 2);
        break;
    }

    trace_milkymist_ac97_memory_read(addr << 2, r);

    return r;
}

static void ac97_write(void *opaque, target_phys_addr_t addr, uint32_t value)
{
    MilkymistAC97State *s = opaque;

    trace_milkymist_ac97_memory_write(addr, value);

    addr >>= 2;
    switch (addr) {
    case R_AC97_CTRL:
        /* always raise an IRQ according to the direction */
        if (value & AC97_CTRL_RQEN) {
            if (value & AC97_CTRL_WRITE) {
                trace_milkymist_ac97_pulse_irq_crrequest();
                qemu_irq_pulse(s->crrequest_irq);
            } else {
                trace_milkymist_ac97_pulse_irq_crreply();
                qemu_irq_pulse(s->crreply_irq);
            }
        }

        /* RQEN is self clearing */
        s->regs[addr] = value & ~AC97_CTRL_RQEN;
        break;
    case R_D_CTRL:
    case R_U_CTRL:
        s->regs[addr] = value;
        update_voices(s);
        break;
    case R_AC97_ADDR:
    case R_AC97_DATAOUT:
    case R_AC97_DATAIN:
    case R_D_ADDR:
    case R_D_REMAINING:
    case R_U_ADDR:
    case R_U_REMAINING:
        s->regs[addr] = value;
        break;

    default:
        error_report("milkymist_ac97: write access to unknown register 0x"
                TARGET_FMT_plx, addr);
        break;
    }

}

static CPUReadMemoryFunc * const ac97_read_fn[] = {
    NULL,
    NULL,
    &ac97_read,
};

static CPUWriteMemoryFunc * const ac97_write_fn[] = {
    NULL,
    NULL,
    &ac97_write,
};

static void ac97_in_cb(void *opaque, int avail_b)
{
    MilkymistAC97State *s = opaque;
    uint8_t buf[4096];
    uint32_t remaining = s->regs[R_U_REMAINING];
    int temp = audio_MIN(remaining, avail_b);
    uint32_t addr = s->regs[R_U_ADDR];
    int transferred = 0;

    trace_milkymist_ac97_in_cb(avail_b, remaining);

    /* prevent from raising an IRQ */
    if (temp == 0) {
        return;
    }

    while (temp) {
        int acquired, to_copy;

        to_copy = audio_MIN(temp, sizeof(buf));
        acquired = AUD_read(s->voice_in, buf, to_copy);
        if (!acquired) {
            break;
        }

        cpu_physical_memory_write(addr, buf, acquired);

        temp -= acquired;
        addr += acquired;
        transferred += acquired;
    }

    trace_milkymist_ac97_in_cb_transferred(transferred);

    s->regs[R_U_ADDR] = addr;
    s->regs[R_U_REMAINING] -= transferred;

    if ((s->regs[R_U_CTRL] & CTRL_EN) && (s->regs[R_U_REMAINING] == 0)) {
        trace_milkymist_ac97_pulse_irq_dmaw();
        qemu_irq_pulse(s->dmaw_irq);
    }
}

static void ac97_out_cb(void *opaque, int free_b)
{
    MilkymistAC97State *s = opaque;
    uint8_t buf[4096];
    uint32_t remaining = s->regs[R_D_REMAINING];
    int temp = audio_MIN(remaining, free_b);
    uint32_t addr = s->regs[R_D_ADDR];
    int transferred = 0;

    trace_milkymist_ac97_out_cb(free_b, remaining);

    /* prevent from raising an IRQ */
    if (temp == 0) {
        return;
    }

    while (temp) {
        int copied, to_copy;

        to_copy = audio_MIN(temp, sizeof(buf));
        cpu_physical_memory_read(addr, buf, to_copy);
        copied = AUD_write(s->voice_out, buf, to_copy);
        if (!copied) {
            break;
        }
        temp -= copied;
        addr += copied;
        transferred += copied;
    }

    trace_milkymist_ac97_out_cb_transferred(transferred);

    s->regs[R_D_ADDR] = addr;
    s->regs[R_D_REMAINING] -= transferred;

    if ((s->regs[R_D_CTRL] & CTRL_EN) && (s->regs[R_D_REMAINING] == 0)) {
        trace_milkymist_ac97_pulse_irq_dmar();
        qemu_irq_pulse(s->dmar_irq);
    }
}

static void milkymist_ac97_reset(DeviceState *d)
{
    MilkymistAC97State *s = container_of(d, MilkymistAC97State, busdev.qdev);
    int i;

    for (i = 0; i < R_MAX; i++) {
        s->regs[i] = 0;
    }

    AUD_set_active_in(s->voice_in, 0);
    AUD_set_active_out(s->voice_out, 0);
}

static int ac97_post_load(void *opaque, int version_id)
{
    MilkymistAC97State *s = opaque;

    update_voices(s);

    return 0;
}

static int milkymist_ac97_init(SysBusDevice *dev)
{
    MilkymistAC97State *s = FROM_SYSBUS(typeof(*s), dev);
    int ac97_regs;

    struct audsettings as;
    sysbus_init_irq(dev, &s->crrequest_irq);
    sysbus_init_irq(dev, &s->crreply_irq);
    sysbus_init_irq(dev, &s->dmar_irq);
    sysbus_init_irq(dev, &s->dmaw_irq);

    AUD_register_card("Milkymist AC'97", &s->card);

    as.freq = 48000;
    as.nchannels = 2;
    as.fmt = AUD_FMT_S16;
    as.endianness = 1;

    s->voice_in = AUD_open_in(&s->card, s->voice_in,
            "mm_ac97.in", s, ac97_in_cb, &as);
    s->voice_out = AUD_open_out(&s->card, s->voice_out,
            "mm_ac97.out", s, ac97_out_cb, &as);

    ac97_regs = cpu_register_io_memory(ac97_read_fn, ac97_write_fn, s,
            DEVICE_NATIVE_ENDIAN);
    sysbus_init_mmio(dev, R_MAX * 4, ac97_regs);

    return 0;
}

static const VMStateDescription vmstate_milkymist_ac97 = {
    .name = "milkymist-ac97",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = ac97_post_load,
    .fields      = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MilkymistAC97State, R_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static SysBusDeviceInfo milkymist_ac97_info = {
    .init = milkymist_ac97_init,
    .qdev.name  = "milkymist-ac97",
    .qdev.size  = sizeof(MilkymistAC97State),
    .qdev.vmsd  = &vmstate_milkymist_ac97,
    .qdev.reset = milkymist_ac97_reset,
};

static void milkymist_ac97_register(void)
{
    sysbus_register_withprop(&milkymist_ac97_info);
}

device_init(milkymist_ac97_register)
