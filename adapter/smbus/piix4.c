// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/pci.h>

#include "../debug.h"

/* SB800 constants */
#define SB800_PIIX4_SMB_IDX     0xcd6

/* PIIX4 SMBus address offsets */
#define SMBHSTSTS               (0 + smba)
#define SMBHSTCNT               (2 + smba)
#define SMBHSTCMD               (3 + smba)
#define SMBHSTADD               (4 + smba)
#define SMBHSTDAT0              (5 + smba)
#define SMBHSTDAT1              (6 + smba)
#define SMBBLKDAT               (7 + smba)

/* PIIX4 constants */
#define PIIX4_QUICK             0x00
#define PIIX4_BYTE              0x04
#define PIIX4_BYTE_DATA         0x08
#define PIIX4_WORD_DATA         0x0C
#define PIIX4_BLOCK_DATA        0x14

/* count for request_region */
#define SMBIOSIZE               7 // Changed from 9
#define MUXED_NAME              "sb800_piix4_smb"
#define MAX_TIMEOUT             500
#define ENABLE_INT9             0

/*
 * Data for PCI driver interface
 *
 * This data only exists for exporting the supported
 * PCI ids via MODULE_DEVICE_TABLE.  We do not actually
 * register a pci_driver, because someone else might
 * want to register another driver on the same PCI id.
 */
static struct pci_device_id const smbus_piix4_tbl[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_KERNCZ_SMBUS) }, // 0x1022 0x790b
    { 0, },            /* End of list */
};
// MODULE_DEVICE_TABLE(pci, smbus_piix4_tbl);

/**
 * struct smbus_context - Storage for smbus access
 *
 * @smbus_adapter: data expected by the read/write functions
 * @adapter:       access to the smbus
 * @smba:          address of the smbus
 */
struct smbus_context {
    struct i2c_adapter      adapter;
    uint16_t                smba;
};
#define ctx_from_adapter(ptr)( \
    container_of(ptr, struct smbus_context, adapter) \
)

static int smbus_piix4_transaction (
    struct i2c_adapter *adapter,
    uint16_t smba
){
    int temp;
    int result = 0;
    int timeout = 0;

    /* Make sure the SMBus host is ready to start transmitting */
    if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
        dev_dbg(&adapter->dev, "SMBus busy (%02x). Resetting...\n", temp);
        outb_p(temp, SMBHSTSTS);
        if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
            dev_err(&adapter->dev, "Failed! (%02x)\n", temp);
            return -EBUSY;
        } else {
            dev_dbg(&adapter->dev, "Successful!\n");
        }
    }

    /* start the transaction by setting bit 6 */
    outb_p(inb(SMBHSTCNT) | 0x040, SMBHSTCNT);

    /* We will always wait for a fraction of a second! (See PIIX4 docs errata) */
    usleep_range(25, 50);

    while ((++timeout < MAX_TIMEOUT) && ((temp = inb_p(SMBHSTSTS)) & 0x01))
        usleep_range(25, 50);

    /* If the SMBus is still busy, we give up */
    if (timeout == MAX_TIMEOUT) {
        dev_err(&adapter->dev, "SMBus Timeout!\n");
        result = -ETIMEDOUT;
    }

    if (temp & 0x10) {
        result = -EIO;
        dev_err(&adapter->dev, "Error: Failed bus transaction\n");
    }

    if (temp & 0x08) {
        result = -EIO;
        dev_dbg(&adapter->dev, "Bus collision! SMBus may be "
            "locked until next hard reset. (sorry!)\n");
        /* Clock stops and slave is stuck in mid-transmission */
    }

    if (temp & 0x04) {
        result = -ENXIO;
        dev_dbg(&adapter->dev, "Error: no response!\n");
    }

    if (inb_p(SMBHSTSTS) != 0x00)
        outb_p(inb(SMBHSTSTS), SMBHSTSTS);

    if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
        dev_err(&adapter->dev, "Failed reset at end of "
            "transaction (%02x)\n", temp);
    }

    return result;
}

static int smbus_piix4_transfer (
    struct i2c_adapter *adapter,
    uint16_t addr,
    uint16_t flags,
    char read_write,
    uint8_t command,
    int size,
    union i2c_smbus_data *data
){
    struct smbus_context *context;
    uint16_t smba;
    int i, len, status;

    if (IS_NULL(adapter))
        return -ENODEV;

    context = i2c_get_adapdata(adapter);
    smba = context->smba;

    switch (size) {
    case I2C_SMBUS_QUICK:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        size = PIIX4_QUICK;
        break;
    case I2C_SMBUS_BYTE:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        if (read_write == I2C_SMBUS_WRITE)
            outb_p(command, SMBHSTCMD);
        size = PIIX4_BYTE;
        break;
    case I2C_SMBUS_BYTE_DATA:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTCMD);
        if (read_write == I2C_SMBUS_WRITE)
            outb_p(data->byte, SMBHSTDAT0);
        size = PIIX4_BYTE_DATA;
        break;
    case I2C_SMBUS_WORD_DATA:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTCMD);
        if (read_write == I2C_SMBUS_WRITE) {
            outb_p(data->word & 0xff, SMBHSTDAT0);
            outb_p((data->word & 0xff00) >> 8, SMBHSTDAT1);
        }
        size = PIIX4_WORD_DATA;
        break;
    case I2C_SMBUS_BLOCK_DATA:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTCMD);
        if (read_write == I2C_SMBUS_WRITE) {
            len = data->block[0];
            if (len == 0 || len > I2C_SMBUS_BLOCK_MAX)
                return -EINVAL;
            outb_p(len, SMBHSTDAT0);
            inb_p(SMBHSTCNT);    /* Reset SMBBLKDAT */
            for (i = 1; i <= len; i++)
                outb_p(data->block[i], SMBBLKDAT);
        }
        size = PIIX4_BLOCK_DATA;
        break;
    default:
        dev_warn(&adapter->dev, "Unsupported transaction %d", size);
        return -EOPNOTSUPP;
    }

    outb_p((size & 0x1C) + (ENABLE_INT9 & 1), SMBHSTCNT);

    status = smbus_piix4_transaction(adapter, smba);
    if (status)
        return status;

    if ((read_write == I2C_SMBUS_WRITE) || (size == PIIX4_QUICK))
        return 0;

    switch (size) {
    case PIIX4_BYTE:
    case PIIX4_BYTE_DATA:
        data->byte = inb_p(SMBHSTDAT0);
        break;
    case PIIX4_WORD_DATA:
        data->word = inb_p(SMBHSTDAT0) + (inb_p(SMBHSTDAT1) << 8);
        break;
    case PIIX4_BLOCK_DATA:
        data->block[0] = inb_p(SMBHSTDAT0);
        if (data->block[0] == 0 || data->block[0] > I2C_SMBUS_BLOCK_MAX)
            return -EPROTO;
        inb_p(SMBHSTCNT);    /* Reset SMBBLKDAT */
        for (i = 1; i <= data->block[0]; i++)
            data->block[i] = inb_p(SMBBLKDAT);
        break;
    }

    return 0;
}

static uint32_t smbus_piix4_func (
    struct i2c_adapter *adapter
){
    return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
        I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
        I2C_FUNC_SMBUS_BLOCK_DATA;
}

static struct i2c_algorithm const smbus_piix4_algorithm = {
    .smbus_xfer    = smbus_piix4_transfer,
    .functionality = smbus_piix4_func,
};

static struct smbus_context *smbus_piix4_context_create (
    struct pci_dev *pci_dev
){
    struct smbus_context *context;
    uint16_t smba;
    uint8_t  smba_en_lo, smba_en_hi, smb_en, smb_en_status;

    /* Determine the address of the SMBus areas */
    if (pci_dev->vendor == PCI_VENDOR_ID_AMD &&
        pci_dev->device == PCI_DEVICE_ID_AMD_KERNCZ_SMBUS &&
        pci_dev->revision >= 0x49)
        smb_en = 0x00;
    else
        smb_en = 0x28;

    if (!request_muxed_region(SB800_PIIX4_SMB_IDX, 2, MUXED_NAME)) {
        LIGHTS_ERR("SMB base address index region 0x%x already in use.", SB800_PIIX4_SMB_IDX);
        return ERR_PTR(-EBUSY);
    }

    outb_p(smb_en, SB800_PIIX4_SMB_IDX);
    smba_en_lo = inb_p(SB800_PIIX4_SMB_IDX + 1);
    outb_p(smb_en + 1, SB800_PIIX4_SMB_IDX);
    smba_en_hi = inb_p(SB800_PIIX4_SMB_IDX + 1);

    release_region(SB800_PIIX4_SMB_IDX, 2);

    if (!smb_en) {
        smb_en_status = smba_en_lo & 0x10;
        smba = (smba_en_hi << 8) | 0x20;
    } else {
        smb_en_status = smba_en_lo & 0x01;
        smba = ((smba_en_hi << 8) | smba_en_lo) & 0xffe0;
    }

    if (!smb_en_status) {
        LIGHTS_ERR("SMBus Host Controller not enabled!");
        return ERR_PTR(-ENODEV);
    }

    if (acpi_check_region(smba, SMBIOSIZE, "piix4_smbus"))
        return ERR_PTR(-ENODEV);

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return ERR_PTR(-ENOMEM);

    context->smba = smba;

    return context;
}


#ifndef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL_NS_GPL(_sym, _ns) EXPORT_SYMBOL_GPL(_sym)
#endif

void piix4_adapter_destroy (
    struct i2c_adapter *adap
){
    struct smbus_context *context = ctx_from_adapter(adap);

    if (IS_NULL(adap))
        return;

    LIGHTS_DBG("Releasing I2C adapter '%s'", context->adapter.name);

    i2c_set_adapdata(&context->adapter, NULL);
    i2c_del_adapter(&context->adapter);

    kfree(context);
}
EXPORT_SYMBOL_NS_GPL(piix4_adapter_destroy, LIGHTS);

struct i2c_adapter *piix4_adapter_create (
    void
){
    struct smbus_context *context;
    struct pci_dev *pci_dev = NULL;
    bool found = false;
    int err;

    /* Match the PCI device */
    for_each_pci_dev(pci_dev) {
        if (pci_match_id(smbus_piix4_tbl, pci_dev) != NULL) {
            found = true;
            break;
        }
    }

    if (!found)
        return ERR_PTR(-ENODEV);

    context = smbus_piix4_context_create(pci_dev);
    if (IS_ERR(context))
        return ERR_CAST(context);

    context->adapter.owner = THIS_MODULE;
    context->adapter.class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
    context->adapter.algo  = &smbus_piix4_algorithm;
    context->adapter.dev.parent = &pci_dev->dev;

    snprintf(context->adapter.name, sizeof(context->adapter.name),
        "AURA MB adapter (piix4) at %04x", context->smba);

    i2c_set_adapdata(&context->adapter, context);

    err = i2c_add_adapter(&context->adapter);
    if (err) {
        kfree(context);
        return ERR_PTR(err);
    }

    LIGHTS_INFO("Created I2C adapter '%s'", context->adapter.name);

    return &context->adapter;
}
EXPORT_SYMBOL_NS_GPL(piix4_adapter_create, LIGHTS);
