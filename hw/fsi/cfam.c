/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2023 IBM Corp.
 *
 * IBM Common FRU Access Macro
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "trace.h"

#include "hw/fsi/cfam.h"
#include "hw/fsi/fsi.h"

#include "hw/qdev-properties.h"

#define ENGINE_CONFIG_NEXT            BE_BIT(0)
#define ENGINE_CONFIG_TYPE_PEEK       (0x02 << 4)
#define ENGINE_CONFIG_TYPE_FSI        (0x03 << 4)
#define ENGINE_CONFIG_TYPE_SCRATCHPAD (0x06 << 4)

#define TO_REG(x)                          ((x) >> 2)

#define CFAM_ENGINE_CONFIG                 TO_REG(0x04)

#define CFAM_CONFIG_CHIP_ID                TO_REG(0x00)
#define CFAM_CONFIG_CHIP_ID_P9             0xc0022d15
#define CFAM_CONFIG_CHIP_ID_BREAK          0xc0de0000

static uint64_t fsi_cfam_config_read(void *opaque, hwaddr addr, unsigned size)
{
    FSICFAMState *cfam = FSI_CFAM(opaque);
    BusChild *kid;
    int i;

    trace_fsi_cfam_config_read(addr, size);

    switch (addr) {
    case 0x00:
        return CFAM_CONFIG_CHIP_ID_P9;
    case 0x04:
        return ENGINE_CONFIG_NEXT       |   /* valid */
               0x00010000               |   /* slots */
               0x00001000               |   /* version */
               ENGINE_CONFIG_TYPE_PEEK  |   /* type */
               0x0000000c;                  /* crc */
    case 0x08:
        return ENGINE_CONFIG_NEXT       |   /* valid */
               0x00010000               |   /* slots */
               0x00005000               |   /* version */
               ENGINE_CONFIG_TYPE_FSI   |   /* type */
               0x0000000a;                  /* crc */
        break;
    default:
        /* The config table contains different engines from 0xc onwards. */
        i = 0xc;
        QTAILQ_FOREACH(kid, &cfam->lbus.bus.children, sibling) {
            if (i == addr) {
                DeviceState *ds = kid->child;
                FSILBusDevice *dev = FSI_LBUS_DEVICE(ds);
                return FSI_LBUS_DEVICE_GET_CLASS(dev)->config;
            }
            i += size;
        }

        if (i == addr) {
            return 0;
        }

        /*
         * As per FSI specification, This is a magic value at address 0 of
         * given FSI port. This causes FSI master to send BREAK command for
         * initialization and recovery.
         */
        return CFAM_CONFIG_CHIP_ID_BREAK;
    }
}

static void fsi_cfam_config_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    FSICFAMState *cfam = FSI_CFAM(opaque);

    trace_fsi_cfam_config_write(addr, size, data);

    switch (TO_REG(addr)) {
    case CFAM_CONFIG_CHIP_ID:
    case CFAM_CONFIG_CHIP_ID + 4:
        if (data == CFAM_CONFIG_CHIP_ID_BREAK) {
            bus_cold_reset(BUS(&cfam->lbus));
        }
    break;
    default:
        trace_fsi_cfam_config_write_noaddr(addr, size, data);
    }
}

static const struct MemoryRegionOps cfam_config_ops = {
    .read = fsi_cfam_config_read,
    .write = fsi_cfam_config_write,
    .valid.max_access_size = 4,
    .valid.min_access_size = 4,
    .impl.max_access_size = 4,
    .impl.min_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t fsi_cfam_unimplemented_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    trace_fsi_cfam_unimplemented_read(addr, size);

    return 0;
}

static void fsi_cfam_unimplemented_write(void *opaque, hwaddr addr,
                                         uint64_t data, unsigned size)
{
    trace_fsi_cfam_unimplemented_write(addr, size, data);
}

static const struct MemoryRegionOps fsi_cfam_unimplemented_ops = {
    .read = fsi_cfam_unimplemented_read,
    .write = fsi_cfam_unimplemented_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void fsi_cfam_instance_init(Object *obj)
{
    FSICFAMState *s = FSI_CFAM(obj);

    object_initialize_child(obj, "scratchpad", &s->scratchpad,
                            TYPE_FSI_SCRATCHPAD);
}

static void fsi_cfam_realize(DeviceState *dev, Error **errp)
{
    FSICFAMState *cfam = FSI_CFAM(dev);
    FSISlaveState *slave = FSI_SLAVE(dev);

    /* Each slave has a 2MiB address space */
    memory_region_init_io(&cfam->mr, OBJECT(cfam), &fsi_cfam_unimplemented_ops,
                          cfam, TYPE_FSI_CFAM, 2 * 1024 * 1024);
    address_space_init(&cfam->as, &cfam->mr, TYPE_FSI_CFAM);

    qbus_init(&cfam->lbus, sizeof(cfam->lbus), TYPE_FSI_LBUS, DEVICE(cfam),
              NULL);

    memory_region_init_io(&cfam->config_iomem, OBJECT(cfam), &cfam_config_ops,
                          cfam, TYPE_FSI_CFAM ".config", 0x400);

    memory_region_add_subregion(&cfam->mr, 0, &cfam->config_iomem);
    memory_region_add_subregion(&cfam->mr, 0x800, &slave->iomem);
    memory_region_add_subregion(&cfam->mr, 0xc00, &cfam->lbus.mr);

    /* Add scratchpad engine */
    if (!qdev_realize_and_unref(DEVICE(&cfam->scratchpad), BUS(&cfam->lbus),
                                errp)) {
        return;
    }

    /* TODO: clarify scratchpad mapping */
    FSILBusDevice *fsi_dev = FSI_LBUS_DEVICE(&cfam->scratchpad);
    memory_region_add_subregion(&cfam->lbus.mr, 0, &fsi_dev->iomem);
}

static void fsi_cfam_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_FSI_BUS;
    dc->realize = fsi_cfam_realize;
}

static const TypeInfo fsi_cfam_info = {
    .name = TYPE_FSI_CFAM,
    .parent = TYPE_FSI_SLAVE,
    .instance_init = fsi_cfam_instance_init,
    .instance_size = sizeof(FSICFAMState),
    .class_init = fsi_cfam_class_init,
};

static uint64_t fsi_scratchpad_read(void *opaque, hwaddr addr, unsigned size)
{
    FSIScratchPad *s = SCRATCHPAD(opaque);

    trace_fsi_scratchpad_read(addr, size);

    if (addr) {
        return 0;
    }

    return s->reg;
}

static void fsi_scratchpad_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    FSIScratchPad *s = SCRATCHPAD(opaque);

    trace_fsi_scratchpad_write(addr, size, data);

    if (addr) {
        return;
    }

    s->reg = data;
}

static const struct MemoryRegionOps scratchpad_ops = {
    .read = fsi_scratchpad_read,
    .write = fsi_scratchpad_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void fsi_scratchpad_realize(DeviceState *dev, Error **errp)
{
    FSILBusDevice *ldev = FSI_LBUS_DEVICE(dev);

    memory_region_init_io(&ldev->iomem, OBJECT(ldev), &scratchpad_ops,
                          ldev, TYPE_FSI_SCRATCHPAD, 0x400);
}

static void fsi_scratchpad_reset(DeviceState *dev)
{
    FSIScratchPad *s = SCRATCHPAD(dev);

    s->reg = 0;
}

static void fsi_scratchpad_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    FSILBusDeviceClass *ldc = FSI_LBUS_DEVICE_CLASS(klass);

    dc->realize = fsi_scratchpad_realize;
    dc->reset = fsi_scratchpad_reset;

    ldc->config =
          ENGINE_CONFIG_NEXT            | /* valid */
          0x00010000                    | /* slots */
          0x00001000                    | /* version */
          ENGINE_CONFIG_TYPE_SCRATCHPAD | /* type */
          0x00000007;                     /* crc */
}

static const TypeInfo fsi_scratchpad_info = {
    .name = TYPE_FSI_SCRATCHPAD,
    .parent = TYPE_FSI_LBUS_DEVICE,
    .instance_size = sizeof(FSIScratchPad),
    .class_init = fsi_scratchpad_class_init,
    .class_size = sizeof(FSILBusDeviceClass),
};

static void fsi_cfam_register_types(void)
{
    type_register_static(&fsi_scratchpad_info);
    type_register_static(&fsi_cfam_info);
}

type_init(fsi_cfam_register_types);
