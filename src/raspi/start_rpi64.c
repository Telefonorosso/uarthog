/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdarg.h>
#include <stdint.h>
#include "A64.h"
#include "config.h"
#include "support.h"
#include "tlsf.h"
#include "mmu.h"
#include "devicetree.h"
#include "M68k.h"
#include "HunkLoader.h"
#include "DuffCopy.h"
#include "EmuLogo.h"
#include "EmuFeatures.h"
#include "RegisterAllocator.h"
#include "version.h"
#include "logo/logo_emu68.h"
#include "logo/logo_ppc.h"
#include "logo/logo_pistorm.h"
#ifdef PISTORM_ANY_MODEL
#include "ps_protocol.h"
#endif

void _start();
void _boot();
void move_kernel(intptr_t from, intptr_t to);
extern uint64_t mmu_user_L1[512];
extern uint64_t mmu_user_L2[4*512];

void M68K_StartEmu(void *addr);

void *framebuffer;
uint32_t pitch;
uint32_t fb_width;
uint32_t fb_height;

extern const uint32_t topaz8_charloc[];
extern const uint8_t topaz8_chardata[];

extern void *firmware_file;
extern uint32_t firmware_size;

uint32_t text_x = 0;
uint32_t text_y = 0;
const int modulo = 192;
int purple = 0;
int black = 0;

void put_char(uint8_t c)
{
    if (framebuffer && pitch)
    {
        uint16_t *pos_in_image = (uint16_t*)((uintptr_t)framebuffer + (text_y * 16 + 5)* pitch);
        pos_in_image += 4 + text_x * 8;

        if (c == 10) {
            text_x = 0;
            text_y++;
        }
        else if (c >= 32) {
            uint32_t loc = (topaz8_charloc[c - 32] >> 16) >> 3;
            const uint8_t *data = &topaz8_chardata[loc];

            for (int y = 0; y < 16; y++) {
                const uint8_t byte = *data;

                for (int x=0; x < 8; x++) {
                    if (byte & (0x80 >> x)) {
                        if (purple) {
                            pos_in_image[x] = LE16(0xed51);
                        }
                        else if (black) {
                            pos_in_image[x] = LE16(0x630c);
                        }
                        else {
                            pos_in_image[x] = 0;
                        }
                    }
                }

                if (y & 1)
                    data += modulo;
                pos_in_image += pitch / 2;
            }
            text_x++;
        }
    }
}

static void __putc(void *data, char c)
{
    (void)data;
    put_char(c);
}

struct EmuLogo *rle_decode(uint8_t *logo_rle, uint32_t length)
{
    uint32_t w = 0;
    uint32_t h = 0;

    while (1) {
        w = (w << 7) | (*logo_rle & 0x7f);
        if ((*logo_rle++ & 0x80) == 0)
            break;
    }

    while (1) {
        h = (h << 7) | (*logo_rle & 0x7f);
        if ((*logo_rle++ & 0x80) == 0)
            break;
    }

    struct EmuLogo *logo = (struct EmuLogo *)tlsf_malloc(tlsf, sizeof(struct EmuLogo) + w * h);
    logo->el_Width = w;
    logo->el_Height = h;
    logo->el_Data = (uint8_t *)((uintptr_t)logo + sizeof(struct EmuLogo));

    uint32_t pos = 0;
    uint32_t outpos = 0;
    while(pos < length) {
        int count = 0;
        int diff = 0;
        unsigned char c = logo_rle[pos++];
        if (c & 0x40) diff = 1;
        count = c & 0x3f;
        while (c & 0x80) {
            count <<= 7;
            c = logo_rle[pos++];
            count |= c & 0x7f;
        }
        if (diff) {
            for (int i = 0; i < count; i++) {
                logo->el_Data[outpos] = logo_rle[pos++];
                outpos++;
            }
        } else {
            uint8_t pix = logo_rle[pos++];

            for (int i=0; i < count; i++) {
                logo->el_Data[outpos] = pix;
                outpos++;
            }
        }
    }

    return logo;
}

void draw_logo(struct EmuLogo *logo, uint32_t start_x, uint32_t start_y)
{
    uint32_t pix_cnt = logo->el_Width * logo->el_Height;
    uint32_t x = 0;
    uint8_t *data = logo->el_Data;
    uint16_t *buff = (uint16_t *)((uintptr_t)framebuffer + pitch*start_y);
    buff += start_x;

    while(pix_cnt > 0) {
        uint8_t gray = *data++;
        uint16_t color;

        if (purple)
        {
            gray = 240 - gray;
            int r=-330,g=-343,b=-91;
            r += (gray * 848) >> 8;
            g += (gray * 768) >> 8;
            b += (gray * 341) >> 8;

            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            color = (b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11);
        }
        else if (black)
        {
            int g = ((120 - (int)gray) * 5) / 4;
            if (g < 0)
                g = 0;
            if (g > 255)
                g = 255;

            color = (g >> 3) | ((g >> 2) << 5) | ((g >> 3) << 11);
        }
        else
        {
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }

        pix_cnt--;
        buff[x++] = LE16(color);
        /* If new line, advance the buffer by pitch and reset x counter */
        if (x >= logo->el_Width) {
            buff += pitch / 2;
            x = 0;
        }
    }
}

void display_logo()
{
    uint16_t *fb;
    struct Size sz = get_display_size();
    uint32_t start_x, start_y;
    of_node_t *e = NULL;

    e = dt_find_node("/chosen");
    if (e)
    {
        of_property_t * prop = dt_find_property(e, "bootargs");
        if (prop)
        {
            const char *tok;
            if ((tok = find_token(prop->op_value, "logo=")))
            {
                tok += 5;

                if (strncmp(tok, "purple", 6) == 0)
                    purple = 1;
                else if (strncmp(tok, "black", 5) == 0)
                    black = 1;
            }
        }
    }

    kprintf("[BOOT] Display size is %dx%d\n", sz.width, sz.height);
    fb_width = sz.width;
    fb_height = sz.height;
    init_display(sz, (void**)&framebuffer, &pitch);
    kprintf("[BOOT] Framebuffer @ %08x\n", framebuffer);
    fb = framebuffer;

    struct EmuLogo *emu68logo = rle_decode(logo_emu68, logo_emu68_len);
    uint8_t *data = emu68logo->el_Data;
    start_x = (sz.width - emu68logo->el_Width) / 2;
    start_y = (sz.height - emu68logo->el_Height) / 2;

    kprintf("[BOOT] Logo start coordinate: %dx%d, size: %dx%d\n", start_x, start_y, emu68logo->el_Width, emu68logo->el_Height);

    /* Calculate text coordinate for version string */
    text_y = (fb_height - 16 - 5) / 16;
    text_x = (fb_width - strlen(&VERSION_STRING[6]) * 8 - 1) / 8;

#if defined(PISTORM_ANY_MODEL)
    struct EmuLogo *pistormlogo = rle_decode(logo_pistorm, logo_pistorm_len);
    start_y -= (pistormlogo->el_Height + 10) / 2;
#if defined(PISTORM)
    const uint8_t pistorm_model = pistorm_get_model();
    switch(pistorm_model)
    {
        case PISTORM_MODEL_16:
            text_x -= strlen("PiStorm16, ");
            break;
        case PISTORM_MODEL_32:
            text_x -= strlen("PiStorm32lite, ");
            break;
    }
#elif defined(PISTORM_CLASSIC)
    text_x -= strlen("PiStorm Classic, ");
#endif
#endif

    /* First clear the screen. Use color in top left corner of RLE image for that */
    {
        uint8_t gray = data[0];
        uint16_t color;

        if (purple)
        {
            gray = 240 - gray;
            int r=-330,g=-343,b=-91;
            r += (gray * 848) >> 8;
            g += (gray * 768) >> 8;
            b += (gray * 341) >> 8;

            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            color = (b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11);
        }
        else if (black)
        {
            gray = 0;
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }
        else
        {
            color = (gray >> 3) | ((gray >> 2) << 5) | ((gray >> 3) << 11);
        }

        for (int i=0; i < sz.width * sz.height; i++)
            fb[i] = LE16(color);
    }

    /* Then draw logos according to current settings */
    draw_logo(emu68logo, start_x, start_y);

    if (dt_find_property(dt_find_node("/emu68"), "ppc-enable"))
    {
        struct EmuLogo *ppclogo = rle_decode(logo_ppc, logo_ppc_len);
        draw_logo(ppclogo, start_x + emu68logo->el_Width, start_y + 49);
        tlsf_free(tlsf, ppclogo);
    }
    start_y += emu68logo->el_Height + 10;
    tlsf_free(tlsf, emu68logo);

#if defined(PISTORM_ANY_MODEL)
    draw_logo(pistormlogo, (sz.width - pistormlogo->el_Width) / 2, start_y);
    tlsf_free(tlsf, pistormlogo);
#if defined(PISTORM)
    switch(pistorm_model)
    {
        case PISTORM_MODEL_16:
            kprintf_pc(__putc, NULL, "PiStorm16, ");
            break;
        case PISTORM_MODEL_32:
            kprintf_pc(__putc, NULL, "PiStorm32lite, ");
            break;
    }
#elif defined(PISTORM_CLASSIC)
    kprintf_pc(__putc, NULL, "PiStorm Classic, ");
#endif
#endif
    /* Print EMu68 version number and git sha. */
    kprintf_pc(__putc, NULL, &VERSION_STRING[6]);
    
    /* Reset test coordinates for further text printing (e.g. buptest) */
    text_x = 0;
    text_y = 0;

    #if 0
extern unsigned char pistorm_get_model();
    kprintf_pc(__putc, NULL, "PiStorm model: %d\n", pistorm_get_model());
    kprintf_pc(__putc, NULL, "Firmware file: %p\n", firmware_file);
    kprintf_pc(__putc, NULL, "Firmware size: %d\n", firmware_size);
    #endif
}

uintptr_t top_of_ram;

#ifdef PISTORM_ANY_MODEL
#include "ps_protocol.h"

extern int block_c0;
#endif

// Helper function to map peripheral ranges
void map_peripheral_ranges(char *node_name, uint32_t *start_map)
{
    int is_scb = strcmp(node_name, "/scb") == 0;

    of_node_t *e = dt_find_node(node_name);
    if (!e)
        return;

    of_property_t *p = dt_find_property(e, "ranges");
    if (!p)
        return;

    uint32_t *ranges = p->op_value;
    int32_t len = p->op_length;

    int addr_cpu_len = dt_get_property_value_u32(e->on_parent, "#address-cells", 1, FALSE);
    int addr_bus_len = dt_get_property_value_u32(e, "#address-cells", 1, TRUE);
    int size_bus_len = dt_get_property_value_u32(e, "#size-cells", 1, TRUE);

    int pos_abus = addr_bus_len - 1;
    int pos_acpu = pos_abus + addr_cpu_len;
    int pos_sbus = pos_acpu + size_bus_len;

    while (len > 0)
    {
        uint32_t addr_bus, addr_cpu;
        uint32_t addr_len;

        addr_bus = BE32(ranges[pos_abus]);
        addr_cpu = BE32(ranges[pos_acpu]);
        addr_len = BE32(ranges[pos_sbus]);

        /* Ignore large identity mappings in /scb branch */
        if(is_scb && addr_bus == addr_cpu)
        {
            len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
            ranges += addr_bus_len + addr_cpu_len + size_bus_len;
            continue;
        }

        mmu_map(addr_cpu, *start_map << 21, addr_len,
                MMU_ACCESS | MMU_ALLOW_EL0 | MMU_ATTR_DEVICE, 0);

        kprintf("bus: %08x, cpu: %08x, len: %08x\n", addr_bus, addr_cpu, addr_len);

        ranges[pos_acpu] = BE32(*start_map << 21);

        if (addr_bus == 0x40000000) {
            extern uintptr_t local_intc_base;
            local_intc_base = *start_map << 21;
        }

        // Round up length to nearest 2 MiB and calculate addend for next mapping
        uint32_t addend = (addr_len + (1 << 21) - 1) >> 21;
        
        *start_map += addend;

        len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
        ranges += addr_bus_len + addr_cpu_len + size_bus_len;
    }
}

/* Helper to clamp and map PCIe MMIO window and publish DT properties */
static void map_pcie_mmio_window(uint32_t *start_map)
{
    /* Resolve PCIe node via alias pcie0 */
    of_node_t *pcie = NULL;
    of_node_t *aliases = dt_find_node("/aliases");
    if (aliases)
    {
        of_property_t *ap = dt_find_property(aliases, "pcie0");
        if (ap && ap->op_value && ap->op_length > 0)
        {
            pcie = dt_find_node((char *)ap->op_value);
        }
    }

    if (!pcie)
    {
        kprintf("[PCIE] No PCIe node, skipping\n");
        return;
    }

    of_property_t *rp = dt_find_property(pcie, "ranges");
    if (!rp)
    {
        kprintf("[PCIE] FATAL: Unable to find PCIe ranges property\n");
        return;
    }

    uint32_t *ranges = (uint32_t *)rp->op_value;
    int len = rp->op_length;

    int addr_cpu_len = dt_get_property_value_u32(pcie->on_parent, "#address-cells", 2, FALSE);
    int addr_bus_len = dt_get_property_value_u32(pcie, "#address-cells", 3, TRUE);
    int size_bus_len = dt_get_property_value_u32(pcie, "#size-cells", 2, TRUE);

    while (len > 0)
    {
        /* Decode the child (PCI) address space code from the first cell */
        uint32_t space_code = BE32(ranges[0]);
        int is_mem32 = ((space_code & 0x03000000u) == 0x02000000u);
        int is_prefetch = ((space_code & 0x40000000u) != 0);
        kprintf("[PCIE] space_code=%08x mem32=%d prefetch=%d\n", space_code, is_mem32, is_prefetch);

        if (is_mem32 && !is_prefetch)
        {
            uint64_t phys;
            if (addr_cpu_len >= 2)
            {
                phys = BE32(ranges[addr_bus_len + addr_cpu_len - 2]);
                phys = (phys << 32) | BE32(ranges[addr_bus_len + addr_cpu_len - 1]);
            } else if (addr_cpu_len == 1) {
                phys = BE32(ranges[addr_bus_len + addr_cpu_len - 1]);
            } else {
                kprintf("[PCIE] Unsupported addr_cpu_len %d in PCIe ranges\n", addr_cpu_len);
                break;
            }

            /* Clamp size to 64 MiB */
            const uint32_t map_size = 0x04000000u;
            if (size_bus_len >= 2) {
                ranges[addr_bus_len + addr_cpu_len + size_bus_len - 2] = 0;
            }
            ranges[addr_bus_len + addr_cpu_len + size_bus_len - 1] = BE32(map_size);

            /* Map into low virtual 0xF2xxxxxx area - it must fit below 0xff00xxxx which is Zorro3 config space */
            if (*start_map + (map_size >> 21) >= 0xff0 / 2)
            {
                kprintf("[PCIE] FATAL: Out of low-virt address space for PCIe MMIO!\n");
                break;
            }

            uintptr_t virt = (uintptr_t)(*start_map << 21);
            *start_map += map_size >> 21;
            kprintf("[PCIE] Next low-virt addr: %08x\n", (*start_map) << 21);

            mmu_map(phys, virt, (uintptr_t)map_size,
                    MMU_ACCESS | MMU_ALLOW_EL0 | MMU_ATTR_DEVICE, 0);

            kprintf("[PCIE] MMIO: phys=%08x%08x virt=%08x size=%08x\n",
                    (uint32_t)(phys >> 32), (uint32_t)phys, (uint32_t)virt, (uint32_t)map_size);

            /* Expose phys/virt/size as emu68-specific properties for the guest */
            uint32_t buf64[2];
            buf64[0] = BE32((uint32_t)(phys >> 32));
            buf64[1] = BE32((uint32_t)(phys & 0xffffffffu));
            dt_add_property(pcie, "emu68,pci-mmio-phys", buf64, sizeof(buf64));

            buf64[0] = BE32(virt);
            dt_add_property(pcie, "emu68,pci-mmio-virt", buf64, sizeof(buf64[0]));

            buf64[0] = BE32(map_size);
            dt_add_property(pcie, "emu68,pci-mmio-size", buf64, sizeof(buf64[0]));

            /* Only adjust/map the first non-prefetchable mem32 window */
            break;
        }

        len -= sizeof(int32_t) * (addr_bus_len + addr_cpu_len + size_bus_len);
        ranges += addr_bus_len + addr_cpu_len + size_bus_len;
    }
}

void platform_init()
{
    uint32_t start_map = 0xf20 / 2;

    /*
        Prepare mapping for peripherals. Use and update the data from device tree here
        All peripherals are mapped in the lower 4G address space so that they can be
        accessed from m68k.
    */
    map_peripheral_ranges("/soc", &start_map);
    map_peripheral_ranges("/scb", &start_map);

    /* 
        Map PCIe MMIO window if PCIe is present.
        Reduces the window size to 64 MiB to fit into the low-virt area.
        Exposes the phys/virt/size as emu68,pci-mmio-phys/virt/size properties.
        The mapping in ranges remains using physical address.
        The PCIe driver needs both the physical and virtual address.
    */
    map_pcie_mmio_window(&start_map);
}



#if defined(PISTORM_CLASSIC)
/*
 * PiStorm-classic / Pi 3A+ USB CDC-ACM POC3-EP0DIAG
 * -------------------------------------------
 * Polling-only Synopsys DWC2 USB device implementation for Emu68.
 *
 * Goals:
 *   - enumerate on Windows/Linux as a standards-based CDC ACM serial device
 *   - keep the DWC2 device state alive after initial enumeration
 *   - provide a persistent BULK-IN path for Emu68 kernel logging
 *
 * The implementation intentionally uses PIO/polling only: no DMA and no
 * DWC2 IRQ handler are introduced in this POC.  emu68_usb_putc(), called by
 * support_rpi.c's global kprintf mux, opportunistically services EP0 and the
 * data endpoints on every emitted character.  An initial polling window gives
 * Windows enough time to enumerate even before normal Emu68 logging begins.
 *
 * Experimental identity: Linux Gadget Serial's well-known POC VID/PID
 * 0525:a4a7 is used so Windows/VirtualBox exercise the same class-driver path
 * already verified with Raspberry Pi OS.  Do NOT use this VID/PID for a final
 * public release; assign a project VID/PID before distribution.
 */

extern uint32_t set_power_state(uint32_t device_id, uint32_t state);

#define USB2_BASE               0xf2980000UL

#define USB_GAHBCFG             0x008
#define USB_GUSBCFG             0x00c
#define USB_GRSTCTL             0x010
#define USB_GINTSTS             0x014
#define USB_GINTMSK             0x018
#define USB_GRXSTSP             0x020
#define USB_GRXFSIZ             0x024
#define USB_GNPTXFSIZ           0x028
#define USB_GSNPSID             0x040
#define USB_DPTXFSIZ(n)         (0x100 + ((n) * 4))

#define USB_DCFG                0x800
#define USB_DCTL                0x804
#define USB_DSTS                0x808
#define USB_DIEPMSK             0x810
#define USB_DOEPMSK             0x814
#define USB_DAINT               0x818
#define USB_DAINTMSK            0x81c

#define USB_DIEPCTL(n)          (0x900 + ((n) * 0x20))
#define USB_DIEPINT(n)          (0x908 + ((n) * 0x20))
#define USB_DIEPTSIZ(n)         (0x910 + ((n) * 0x20))
#define USB_DTXFSTS(n)          (0x918 + ((n) * 0x20))
#define USB_DOEPCTL(n)          (0xb00 + ((n) * 0x20))
#define USB_DOEPINT(n)          (0xb08 + ((n) * 0x20))
#define USB_DOEPTSIZ(n)         (0xb10 + ((n) * 0x20))
#define USB_FIFO(n)             (0x1000 + ((n) * 0x1000))

#define USB_GAHBCFG_GLBL_INTR_EN        (1U << 0)
#define USB_GAHBCFG_DMA_EN              (1U << 5)

#define USB_GUSBCFG_FORCEDEVMODE        (1U << 30)
#define USB_GUSBCFG_FORCEHOSTMODE       (1U << 29)
#define USB_GUSBCFG_HNPCAP              (1U << 9)
#define USB_GUSBCFG_SRPCAP              (1U << 8)
#define USB_GUSBCFG_TOUTCAL_MASK        0x7U

#define USB_GRSTCTL_AHBIDLE             (1U << 31)
#define USB_GRSTCTL_TXFNUM_ALL          (0x10U << 6)
#define USB_GRSTCTL_TXFFLSH             (1U << 5)
#define USB_GRSTCTL_RXFFLSH             (1U << 4)
#define USB_GRSTCTL_CSFTRST             (1U << 0)

#define USB_GINTSTS_OEPINT              (1U << 19)
#define USB_GINTSTS_IEPINT              (1U << 18)
#define USB_GINTSTS_ENUMDONE            (1U << 13)
#define USB_GINTSTS_USBRST              (1U << 12)
#define USB_GINTSTS_RXFLVL              (1U << 4)
#define USB_GINTSTS_CURMODE_HOST        (1U << 0)

#define USB_DCFG_DEVADDR_MASK           (0x7fU << 4)
#define USB_DCFG_DEVADDR(a)             (((uint32_t)(a) & 0x7fU) << 4)
#define USB_DCFG_DEVSPD_MASK            3U
#define USB_DCFG_DEVSPD_HS              0U

#define USB_DCTL_SFTDISCON              (1U << 1)

#define USB_DSTS_ENUMSPD_MASK           (3U << 1)
#define USB_DSTS_ENUMSPD_HS             (0U << 1)

#define USB_DAINT_INEP(n)               (1U << (n))
#define USB_DAINT_OUTEP(n)              (1U << ((n) + 16))

#define USB_DXEPCTL_EPENA               (1U << 31)
#define USB_DXEPCTL_SETD0PID            (1U << 28)
#define USB_DXEPCTL_CNAK                (1U << 26)
#define USB_DXEPCTL_TXFNUM(n)           (((uint32_t)(n) & 0xfU) << 22)
#define USB_DXEPCTL_STALL               (1U << 21)
#define USB_DXEPCTL_EPTYPE_BULK         (2U << 18)
#define USB_DXEPCTL_EPTYPE_INTR         (3U << 18)
#define USB_DXEPCTL_USBACTEP            (1U << 15)
#define USB_DXEPCTL_MPS(n)              ((uint32_t)(n) & 0x7ffU)

#define USB_DXEPINT_SETUP               (1U << 3)
#define USB_DXEPINT_XFERCOMPL           (1U << 0)

#define USB_DXEPTSIZ_PKTCNT(n)          (((uint32_t)(n) & 0x3ffU) << 19)
#define USB_DXEPTSIZ_XFERSIZE(n)        ((uint32_t)(n) & 0x7ffffU)
#define USB_DIEPTSIZ0_PKTCNT(n)         (((uint32_t)(n) & 3U) << 19)
#define USB_DIEPTSIZ0_XFERSIZE(n)       ((uint32_t)(n) & 0x7fU)
#define USB_DOEPTSIZ0_SUPCNT(n)         (((uint32_t)(n) & 3U) << 29)
#define USB_DOEPTSIZ0_PKTCNT            (1U << 19)

#define USB_GRXSTS_PKTSTS(v)            (((v) >> 17) & 0xfU)
#define USB_GRXSTS_BYTECNT(v)           (((v) >> 4) & 0x7ffU)
#define USB_GRXSTS_EPNUM(v)             ((v) & 0xfU)
#define USB_PKTSTS_OUTRX                 2U
#define USB_PKTSTS_SETUPRX               6U

#define USB_REQ_GET_STATUS               0x00
#define USB_REQ_CLEAR_FEATURE            0x01
#define USB_REQ_SET_ADDRESS              0x05
#define USB_REQ_GET_DESCRIPTOR           0x06
#define USB_REQ_GET_CONFIGURATION        0x08
#define USB_REQ_SET_CONFIGURATION        0x09
#define USB_REQ_GET_INTERFACE            0x0a
#define USB_REQ_SET_INTERFACE            0x0b

#define USB_DT_DEVICE                    1
#define USB_DT_CONFIG                    2
#define USB_DT_STRING                    3

#define CDC_REQ_SET_LINE_CODING          0x20
#define CDC_REQ_GET_LINE_CODING          0x21
#define CDC_REQ_SET_CONTROL_LINE_STATE   0x22
#define CDC_REQ_SEND_BREAK               0x23

#define USB2_EP_NOTIFY                   1U
#define USB2_EP_DATA_OUT                 2U
#define USB2_EP_DATA_IN                  3U

#define USB2_LOG_RING_SIZE               8192U
#define USB2_ECHO_RING_SIZE              1024U
#define USB2_CMD_LINE_SIZE               64U
#define USB2_INITIAL_ENUM_MS             20000U

struct usb2_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

static volatile uint8_t usb2_hw_up;
static volatile uint8_t usb2_housekeeper_enabled;
static volatile uint8_t usb2_configured;
static volatile uint8_t usb2_pending_address;
static volatile uint8_t usb2_apply_address;
static volatile uint8_t usb2_bulk_busy;
static volatile uint8_t usb2_ep0_out_kind;

enum {
    USB2_EP0_IDLE = 0,
    USB2_EP0_IN_DATA,
    USB2_EP0_OUT_STATUS,
    USB2_EP0_OUT_DATA,
    USB2_EP0_IN_STATUS
};
static volatile uint8_t usb2_ep0_state;

struct usb2_diag {
    uint32_t polls;
    uint32_t usbrst;
    uint32_t enumdone;
    uint32_t rxflvl;
    uint32_t rx_entries;
    uint32_t setup_rx;
    uint32_t out_rx;
    uint32_t in0_done;
    uint32_t out0_done;
    uint32_t get_device;
    uint32_t get_config;
    uint32_t get_string;
    uint32_t set_address;
    uint32_t set_configuration;
    uint32_t stalls;
    uint32_t last_gintsts;
    uint32_t gint_or;
    uint32_t last_grxstsp;
    uint8_t  last_bmRequestType;
    uint8_t  last_bRequest;
    uint16_t last_wValue;
    uint16_t last_wIndex;
    uint16_t last_wLength;
};
static struct usb2_diag usb2_diag;

static uint16_t usb2_bulk_mps = 64;
static uint8_t usb2_line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 }; /* 115200 8N1 */

static uint8_t usb2_log_ring[USB2_LOG_RING_SIZE];
static volatile uint32_t usb2_log_head;
static volatile uint32_t usb2_log_tail;

/* POC8-PING: private CPU2-only console TX ring.  Keep it separate from the
 * system-wide kprintf ring so receiving USB data does not add another writer
 * to usb2_log_head/usb2_log_tail. */
static uint8_t usb2_echo_ring[USB2_ECHO_RING_SIZE];
static uint32_t usb2_echo_head;
static uint32_t usb2_echo_tail;

/* POC8-PING: line-oriented command parser state.  This state is private to
 * CPU2, exactly like the EP2 OUT receive path and the echo/reply ring. */
static uint8_t usb2_cmd_line[USB2_CMD_LINE_SIZE];
static uint32_t usb2_cmd_len;
static uint8_t usb2_cmd_last_was_cr;

/*
 * POC12-REBOOT: delayed full-Pi reboot.
 * The parser only arms a timer and returns, so CDC keeps being serviced and
 * the confirmation text is really transmitted before the Pi resets.
 */
static uint8_t usb2_reboot_pending;
static uint64_t usb2_reboot_deadline;

static uint8_t usb2_tx_stage[512];

static const uint8_t usb2_device_desc[] = {
    18, USB_DT_DEVICE,
    0x00, 0x02,             /* USB 2.00 */
    0x02, 0x00, 0x00,       /* Communications device */
    64,                     /* EP0 MPS */
    0x25, 0x05,             /* VID 0x0525 -- POC only */
    0xa7, 0xa4,             /* PID 0xa4a7 -- Gadget Serial v2.4 */
    0x02, 0x00,             /* bcdDevice 0.02 */
    1, 2, 3,
    1
};

/*
 * CDC ACM configuration, 2 interfaces:
 *   IF0 Communications / ACM, EP1 IN interrupt
 *   IF1 CDC Data, EP2 OUT bulk + EP3 IN bulk
 */
static const uint8_t usb2_config_desc[] = {
    9, USB_DT_CONFIG,
    67, 0,
    2, 1, 0,
    0x80, 50,

    /* Interface 0: CDC Communications / ACM */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,

    /* CDC Header Functional Descriptor, CDC 1.10 */
    5, 0x24, 0x00, 0x10, 0x01,

    /* CDC Call Management Functional Descriptor */
    5, 0x24, 0x01, 0x00, 0x01,

    /* CDC ACM Functional Descriptor */
    4, 0x24, 0x02, 0x02,

    /* CDC Union Functional Descriptor: master IF0, slave IF1 */
    5, 0x24, 0x06, 0x00, 0x01,

    /* EP1 IN interrupt notification */
    7, 5, 0x81, 0x03, 16, 0, 16,

    /* Interface 1: CDC Data */
    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,

    /* EP2 OUT bulk; MPS patched at reply time */
    7, 5, 0x02, 0x02, 64, 0, 0,

    /* EP3 IN bulk; MPS patched at reply time */
    7, 5, 0x83, 0x02, 64, 0, 0
};

static const uint8_t usb2_str0[] = { 4, USB_DT_STRING, 0x09, 0x04 };
static const uint8_t usb2_str1[] = {
    12, USB_DT_STRING, 'E',0,'m',0,'u',0,'6',0,'8',0
};
static const uint8_t usb2_str2[] = {
    48, USB_DT_STRING,
    'E',0,'m',0,'u',0,'6',0,'8',0,' ',0,
    'U',0,'S',0,'B',0,' ',0,'D',0,'e',0,'b',0,'u',0,'g',0,' ',0,
    'C',0,'o',0,'n',0,'s',0,'o',0,'l',0,'e',0
};
static const uint8_t usb2_str3[] = {
    20, USB_DT_STRING,
    'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0,'6',0,'8',0
};

static inline uint32_t usb2_rd(uint32_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(USB2_BASE + off);
    uint32_t v = *p;
    dsb();
    return LE32(v);
}

static inline void usb2_wr(uint32_t off, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(USB2_BASE + off);
    *p = LE32(v);
    dsb();
}

static void usb2_delay_ms(uint32_t ms)
{
    uint64_t start, now, freq, ticks;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
    ticks = (freq * ms) / 1000U;
    do {
        asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    } while ((now - start) < ticks);
}

static int usb2_wait_mask(uint32_t off, uint32_t mask, uint32_t wanted,
                          uint32_t loops)
{
    while (loops--)
        if ((usb2_rd(off) & mask) == wanted)
            return 1;
    return 0;
}

static void usb2_fifo_write(unsigned ep, const uint8_t *buf, uint32_t len)
{
    volatile uint32_t *fifo =
        (volatile uint32_t *)(uintptr_t)(USB2_BASE + USB_FIFO(ep));

    while (len) {
        uint32_t w = 0;
        uint32_t n = len > 4U ? 4U : len;
        for (uint32_t i = 0; i < n; ++i)
            w |= ((uint32_t)buf[i]) << (8U * i);
        *fifo = LE32(w);
        dsb();
        buf += n;
        len -= n;
    }
}

static void usb2_fifo_read(uint8_t *buf, uint32_t len)
{
    volatile uint32_t *fifo =
        (volatile uint32_t *)(uintptr_t)(USB2_BASE + USB_FIFO(0));

    while (len) {
        uint32_t w = LE32(*fifo);
        uint32_t n = len > 4U ? 4U : len;
        dsb();
        for (uint32_t i = 0; i < n; ++i)
            *buf++ = (uint8_t)(w >> (8U * i));
        len -= n;
    }
}

static void usb2_drain_fifo(uint32_t len)
{
    uint8_t sink[64];
    while (len) {
        uint32_t n = len > sizeof(sink) ? sizeof(sink) : len;
        usb2_fifo_read(sink, n);
        len -= n;
    }
}

/* POC8-PING: EP2 OUT is serviced only by the USB housekeeper on CPU2, so
 * this console TX ring has one producer and one consumer on the same core.
 * It remains separate from the system-wide kprintf ring. */
static void usb2_echo_ring_put(uint8_t c)
{
    uint32_t next = (usb2_echo_head + 1U) & (USB2_ECHO_RING_SIZE - 1U);

    if (next == usb2_echo_tail)
        usb2_echo_tail = (usb2_echo_tail + 1U) & (USB2_ECHO_RING_SIZE - 1U);

    usb2_echo_ring[usb2_echo_head] = c;
    usb2_echo_head = next;
}

static void usb2_console_puts(const char *s)
{
    while (*s)
        usb2_echo_ring_put((uint8_t)*s++);
}

/* POC13-DEBUG-RANGE-REBOOT: small ASCII-only, case-insensitive command layer.
 * Commands are normalized only for comparison: terminal echo preserves what
 * the user actually typed.  Leading/trailing spaces are ignored. */
static uint8_t usb2_ascii_upper(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        c = (uint8_t)(c - ('a' - 'A'));
    return c;
}

static int usb2_cmd_equals_ci(const char *word)
{
    uint32_t first = 0;
    uint32_t last = usb2_cmd_len;
    uint32_t i = 0;

    while (first < last && (usb2_cmd_line[first] == ' ' || usb2_cmd_line[first] == '\t'))
        first++;
    while (last > first && (usb2_cmd_line[last - 1U] == ' ' || usb2_cmd_line[last - 1U] == '\t'))
        last--;

    while (word[i] != 0 && (first + i) < last) {
        if (usb2_ascii_upper(usb2_cmd_line[first + i]) !=
            usb2_ascii_upper((uint8_t)word[i]))
            return 0;
        i++;
    }

    return word[i] == 0 && (first + i) == last;
}

static void usb2_console_putc_cb(void *data, char c)
{
    (void)data;
    usb2_echo_ring_put((uint8_t)c);
}

/*
 * POC11-DEBUG-ONOFF
 * -----------------
 * EmuControl's Debug toggle operates on Emu68 DBGCTRL (MOVEC #0xed):
 *
 *   reg &= ~3;
 *   if (enabled)
 *       reg |= 1;
 *
 * Bits 0..1 are the debug verbosity, while bit 2 is the independent
 * Disassemble switch.  Inside Emu68 those DBGCTRL bits map directly to the
 * global 'debug' and 'disasm' variables.  Therefore changing only 'debug'
 * from 0 to 1 (or back to 0) is exactly the same state change as clicking
 * the GUI Debug toggle, while leaving Disassemble untouched.
 *
 * No JIT flush is performed here, matching EmuControl's Debug button.
 */
extern int debug;

/*
 * Emu68's translator uses these directly when deciding whether a newly
 * translated block falls inside the debug/disassembly address window.
 */
extern uint32_t debug_range_min;
extern uint32_t debug_range_max;

static int usb2_hex_nibble(uint8_t c, uint32_t *v)
{
    if (c >= '0' && c <= '9') {
        *v = (uint32_t)(c - '0');
        return 1;
    }

    c = usb2_ascii_upper(c);
    if (c >= 'A' && c <= 'F') {
        *v = 10U + (uint32_t)(c - 'A');
        return 1;
    }

    return 0;
}

static int usb2_parse_hex32(uint32_t first, uint32_t last, uint32_t *value)
{
    uint32_t i = first;
    uint32_t v = 0;
    uint32_t digits = 0;

    if ((last - i) >= 2 &&
        usb2_cmd_line[i] == '0' &&
        usb2_ascii_upper(usb2_cmd_line[i + 1]) == 'X')
        i += 2;

    while (i < last) {
        uint32_t n;

        if (!usb2_hex_nibble(usb2_cmd_line[i], &n))
            return 0;

        if (digits >= 8)
            return 0;

        v = (v << 4) | n;
        digits++;
        i++;
    }

    if (!digits)
        return 0;

    *value = v;
    return 1;
}

static void usb2_console_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char one[2];
    int shift;

    one[1] = '\0';

    for (shift = 28; shift >= 0; shift -= 4) {
        one[0] = hex[(v >> shift) & 0x0f];
        usb2_console_puts(one);
    }
}

static int usb2_cmd_debug_range(void)
{
    uint32_t first = 0;
    uint32_t last = usb2_cmd_len;
    uint32_t p, a0, a1, b0, b1;
    uint32_t lo, hi;

    while (first < last &&
           (usb2_cmd_line[first] == ' ' || usb2_cmd_line[first] == '\t'))
        first++;
    while (last > first &&
           (usb2_cmd_line[last - 1] == ' ' || usb2_cmd_line[last - 1] == '\t'))
        last--;

    /* Explicit syntax only: DEBUG RANGE ... */
    p = first;

    if ((last - p) < 5 ||
        usb2_ascii_upper(usb2_cmd_line[p + 0]) != 'D' ||
        usb2_ascii_upper(usb2_cmd_line[p + 1]) != 'E' ||
        usb2_ascii_upper(usb2_cmd_line[p + 2]) != 'B' ||
        usb2_ascii_upper(usb2_cmd_line[p + 3]) != 'U' ||
        usb2_ascii_upper(usb2_cmd_line[p + 4]) != 'G')
        return 0;

    p += 5;
    if (p >= last || (usb2_cmd_line[p] != ' ' && usb2_cmd_line[p] != '\t'))
        return 0;

    while (p < last &&
           (usb2_cmd_line[p] == ' ' || usb2_cmd_line[p] == '\t'))
        p++;

    if ((last - p) < 5 ||
        usb2_ascii_upper(usb2_cmd_line[p + 0]) != 'R' ||
        usb2_ascii_upper(usb2_cmd_line[p + 1]) != 'A' ||
        usb2_ascii_upper(usb2_cmd_line[p + 2]) != 'N' ||
        usb2_ascii_upper(usb2_cmd_line[p + 3]) != 'G' ||
        usb2_ascii_upper(usb2_cmd_line[p + 4]) != 'E')
        return 0;

    p += 5;
    if (p < last && usb2_cmd_line[p] != ' ' && usb2_cmd_line[p] != '\t')
        return 0;

    while (p < last &&
           (usb2_cmd_line[p] == ' ' || usb2_cmd_line[p] == '\t'))
        p++;

    /* DEBUG RANGE OFF -> restore Emu68 default unrestricted range. */
    if ((last - p) == 3 &&
        usb2_ascii_upper(usb2_cmd_line[p + 0]) == 'O' &&
        usb2_ascii_upper(usb2_cmd_line[p + 1]) == 'F' &&
        usb2_ascii_upper(usb2_cmd_line[p + 2]) == 'F') {
        asm volatile("dmb sy" ::: "memory");
        debug_range_min = 0x00000000U;
        debug_range_max = 0xffffffffU;
        asm volatile("dmb sy" ::: "memory");

        usb2_console_puts("OK DEBUG RANGE OFF\r\n");
        return 1;
    }

    /* DEBUG RANGE <hex-min> <hex-max> */
    a0 = p;
    while (p < last && usb2_cmd_line[p] != ' ' && usb2_cmd_line[p] != '\t')
        p++;
    a1 = p;

    while (p < last &&
           (usb2_cmd_line[p] == ' ' || usb2_cmd_line[p] == '\t'))
        p++;

    b0 = p;
    while (p < last && usb2_cmd_line[p] != ' ' && usb2_cmd_line[p] != '\t')
        p++;
    b1 = p;

    while (p < last &&
           (usb2_cmd_line[p] == ' ' || usb2_cmd_line[p] == '\t'))
        p++;

    if (p != last ||
        !usb2_parse_hex32(a0, a1, &lo) ||
        !usb2_parse_hex32(b0, b1, &hi) ||
        lo > hi) {
        usb2_console_puts("ERR DEBUG RANGE syntax: DEBUG RANGE <hex-min> <hex-max> | DEBUG RANGE OFF\r\n");
        return 1;
    }

    asm volatile("dmb sy" ::: "memory");
    debug_range_min = lo;
    debug_range_max = hi;
    asm volatile("dmb sy" ::: "memory");

    usb2_console_puts("OK DEBUG RANGE ");
    usb2_console_put_hex32(lo);
    usb2_console_puts("-");
    usb2_console_put_hex32(hi);
    usb2_console_puts("\r\n");

    return 1;
}

static void usb2_cmd_debug_status(void)
{
    uint32_t lo, hi;
    int enabled;

    asm volatile("dmb sy" ::: "memory");
    enabled = debug ? 1 : 0;
    lo = debug_range_min;
    hi = debug_range_max;
    asm volatile("dmb sy" ::: "memory");

    usb2_console_puts("DEBUG ");
    usb2_console_puts(enabled ? "ON" : "OFF");
    usb2_console_puts("\r\nRANGE ");

    if (lo == 0x00000000U && hi == 0xffffffffU) {
        usb2_console_puts("OFF\r\n");
    } else {
        usb2_console_put_hex32(lo);
        usb2_console_puts("-");
        usb2_console_put_hex32(hi);
        usb2_console_puts("\r\n");
    }
}

static void usb2_cmd_debug_set(int enable)
{
    asm volatile("dmb sy" ::: "memory");
    debug = enable ? 1 : 0;
    asm volatile("dmb sy" ::: "memory");

    if (enable)
        usb2_console_puts("OK DEBUG ON\r\n");
    else
        usb2_console_puts("OK DEBUG OFF\r\n");
}

/* Exact PiStorm Classic watchdog/FULLRST path, in ps_classic_protocol.c. */
extern void ps_full_reboot(void);

static void usb2_cmd_reboot(void)
{
    uint64_t now;
    uint64_t freq;

    if (usb2_reboot_pending) {
        usb2_console_puts("reboot already pending\r\n");
        return;
    }

    usb2_console_puts("rebooting in 3 seconds\r\n");

    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));

    usb2_reboot_deadline = now + freq * 3ULL;
    asm volatile("dmb sy" ::: "memory");
    usb2_reboot_pending = 1;
}

extern uint32_t rpi_get_core_temperature(void); /* millidegrees Celsius */
extern uint32_t rpi_get_core_voltage(void);     /* microvolts */

/*
 * POC10-STATUS-JIT
 * ----------------
 * EmuControl obtains the following values through Emu68 private MOVEC
 * registers:
 *
 *   0xe7 -> M68KState.JIT_CACHE_TOTAL
 *   0xe8 -> M68KState.JIT_CACHE_FREE
 *   0xe9 -> M68KState.JIT_UNIT_COUNT
 *   0xec -> M68KState.JIT_CACHE_MISS
 *
 * We are already inside Emu68, so CPU2 does not need to synthesize MOVEC
 * instructions or execute anything on the emulated 68k.  Those MOVEC
 * registers are only an Amiga-side interface to these fields.
 *
 * STATUS is read-only.  CPU2 takes a best-effort snapshot of four naturally
 * aligned 32-bit fields while the emulation core may update them.  No lock is
 * taken: momentarily mixing values from two adjacent JIT events is harmless
 * for a diagnostic display and, importantly, cannot stall the emulation core.
 */
extern struct M68KState *__m68k_state;

struct usb2_jit_status {
    uint32_t total;
    uint32_t free;
    uint32_t units;
    uint32_t misses;
};

static int usb2_get_jit_status(struct usb2_jit_status *st)
{
    struct M68KState *m68k;

    if (!st)
        return 0;

    asm volatile("dmb sy" ::: "memory");
    m68k = __m68k_state;
    if (!m68k)
        return 0;

    st->total  = m68k->JIT_CACHE_TOTAL;
    st->free   = m68k->JIT_CACHE_FREE;
    st->units  = m68k->JIT_UNIT_COUNT;
    st->misses = m68k->JIT_CACHE_MISS;
    asm volatile("dmb sy" ::: "memory");

    return 1;
}

static void usb2_cmd_status(void)
{
    uint32_t temp_mc = rpi_get_core_temperature();
    uint32_t volt_uv = rpi_get_core_voltage();
    uint32_t log_pending = (usb2_log_head - usb2_log_tail) &
                           (USB2_LOG_RING_SIZE - 1U);
    struct usb2_jit_status jit;

    usb2_console_puts("EMU68 STATUS\r\n");

    kprintf_pc(usb2_console_putc_cb, NULL,
               "Temperature: %u.%u C\r\n",
               temp_mc / 1000U, (temp_mc % 1000U) / 100U);
    kprintf_pc(usb2_console_putc_cb, NULL,
               "Core voltage: %u mV\r\n", volt_uv / 1000U);

    if (usb2_get_jit_status(&jit)) {
        uint32_t used;
        uint32_t pct_x10;

        /*
         * A concurrent allocator update can theoretically make FREE appear
         * briefly larger than TOTAL in our unlocked snapshot.  Clamp that
         * diagnostic-only case rather than underflowing the calculation.
         */
        if (jit.free <= jit.total)
            used = jit.total - jit.free;
        else
            used = 0;

        if (jit.total) {
            uint64_t scaled = (uint64_t)used * 1000ULL;
            pct_x10 = (uint32_t)(scaled / jit.total);
        } else {
            pct_x10 = 0;
        }

        kprintf_pc(usb2_console_putc_cb, NULL,
                   "JIT cache: %u.%u%% used (%u / %u bytes)\r\n",
                   pct_x10 / 10U, pct_x10 % 10U,
                   used, jit.total);
        kprintf_pc(usb2_console_putc_cb, NULL,
                   "JIT units: %u\r\n", jit.units);
        kprintf_pc(usb2_console_putc_cb, NULL,
                   "Cache misses: %u total\r\n", jit.misses);
    } else {
        usb2_console_puts("JIT: not initialized\r\n");
    }

    kprintf_pc(usb2_console_putc_cb, NULL,
               "USB CDC: %s, %s, MPS %u\r\n",
               usb2_configured ? "configured" : "not configured",
               (usb2_bulk_mps == 512U) ? "high-speed" : "full-speed",
               (uint32_t)usb2_bulk_mps);
    kprintf_pc(usb2_console_putc_cb, NULL,
               "USB log backlog: %u bytes\r\n", log_pending);
}

static void usb2_cmd_execute(void)
{
    if (usb2_cmd_len == 0U)
        return;

    if (usb2_cmd_equals_ci("PING"))
        usb2_console_puts("PONG\r\n");
    else if (usb2_cmd_equals_ci("STATUS"))
        usb2_cmd_status();
    else if (usb2_cmd_equals_ci("DEBUG"))
        usb2_cmd_debug_status();
    else if (usb2_cmd_equals_ci("DEBUG ON"))
        usb2_cmd_debug_set(1);
    else if (usb2_cmd_equals_ci("DEBUG OFF"))
        usb2_cmd_debug_set(0);
    else if (usb2_cmd_debug_range())
        ;
    else if (usb2_cmd_equals_ci("REBOOT"))
        usb2_cmd_reboot();
    else
        usb2_console_puts("ERR UNKNOWN\r\n");
}

static void usb2_cmd_rx_byte(uint8_t c)
{
    /* Accept CR, LF, or CRLF as one Enter.  PuTTY commonly sends CR. */
    if (c == '\r') {
        usb2_echo_ring_put('\r');
        usb2_echo_ring_put('\n');
        usb2_cmd_execute();
        usb2_cmd_len = 0;
        usb2_cmd_last_was_cr = 1;
        return;
    }

    if (c == '\n') {
        if (usb2_cmd_last_was_cr) {
            usb2_cmd_last_was_cr = 0;
            return;
        }
        usb2_echo_ring_put('\r');
        usb2_echo_ring_put('\n');
        usb2_cmd_execute();
        usb2_cmd_len = 0;
        return;
    }

    usb2_cmd_last_was_cr = 0;

    /* Basic terminal editing, enough for a reliable interactive POC. */
    if (c == 0x08U || c == 0x7fU) {
        if (usb2_cmd_len) {
            usb2_cmd_len--;
            usb2_console_puts("\b \b");
        }
        return;
    }

    /* Keep command storage bounded.  Echo only bytes that are accepted into
     * the line, so the terminal always reflects the parser's actual state. */
    if (usb2_cmd_len < (USB2_CMD_LINE_SIZE - 1U)) {
        usb2_cmd_line[usb2_cmd_len++] = c;
        usb2_echo_ring_put(c);
    }
}

static void usb2_console_from_fifo(uint32_t len)
{
    uint8_t buf[64];

    while (len) {
        uint32_t n = len > sizeof(buf) ? sizeof(buf) : len;
        usb2_fifo_read(buf, n);
        for (uint32_t i = 0; i < n; ++i)
            usb2_cmd_rx_byte(buf[i]);
        len -= n;
    }
}

static void usb2_flush_fifos(void)
{
    usb2_wr(USB_GRSTCTL, USB_GRSTCTL_TXFNUM_ALL |
                         USB_GRSTCTL_TXFFLSH |
                         USB_GRSTCTL_RXFFLSH);
    (void)usb2_wait_mask(USB_GRSTCTL,
                         USB_GRSTCTL_TXFFLSH | USB_GRSTCTL_RXFFLSH,
                         0, 1000000U);
}

static void usb2_ep0_arm_setup(void)
{
    usb2_wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_SUPCNT(3) |
                               USB_DOEPTSIZ0_PKTCNT |
                               24U);
    usb2_wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void usb2_ep0_arm_out(uint16_t len)
{
    usb2_wr(USB_DOEPTSIZ(0), USB_DOEPTSIZ0_PKTCNT | (uint32_t)len);
    usb2_wr(USB_DOEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void usb2_ep0_send(const uint8_t *buf, uint16_t len)
{
    /* EP0 maximum packet size is 64 bytes at high/full speed.
     * DIEPTSIZ0.PKTCNT must cover the complete control-IN data stage:
     * e.g. the 67-byte CDC configuration descriptor requires 2 packets. */
    uint32_t pktcnt = len ? (((uint32_t)len + 63U) / 64U) : 1U;

    usb2_wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(pktcnt) |
                               USB_DIEPTSIZ0_XFERSIZE(len));
    usb2_wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    if (len)
        usb2_fifo_write(0, buf, len);
}

static void usb2_ep0_zlp(void)
{
    usb2_wr(USB_DIEPTSIZ(0), USB_DIEPTSIZ0_PKTCNT(1));
    usb2_wr(USB_DIEPCTL(0), USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void usb2_ep0_stall(void)
{
    usb2_diag.stalls++;
    usb2_wr(USB_DIEPCTL(0), usb2_rd(USB_DIEPCTL(0)) | USB_DXEPCTL_STALL);
    usb2_wr(USB_DOEPCTL(0), usb2_rd(USB_DOEPCTL(0)) | USB_DXEPCTL_STALL);
}

static void usb2_set_address_now(uint8_t addr)
{
    uint32_t dcfg = usb2_rd(USB_DCFG);
    dcfg &= ~USB_DCFG_DEVADDR_MASK;
    dcfg |= USB_DCFG_DEVADDR(addr);
    usb2_wr(USB_DCFG, dcfg);
}

static void usb2_arm_data_out(void)
{
    if (!usb2_configured)
        return;

    usb2_wr(USB_DOEPINT(USB2_EP_DATA_OUT), 0xffffffffU);
    usb2_wr(USB_DOEPTSIZ(USB2_EP_DATA_OUT),
            USB_DXEPTSIZ_PKTCNT(1) | USB_DXEPTSIZ_XFERSIZE(usb2_bulk_mps));
    usb2_wr(USB_DOEPCTL(USB2_EP_DATA_OUT),
            usb2_rd(USB_DOEPCTL(USB2_EP_DATA_OUT)) |
            USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
}

static void usb2_configure_endpoints(void)
{
    uint32_t ctl;

    /* CDC notification endpoint, present for class compliance but unused. */
    ctl = USB_DXEPCTL_MPS(16) |
          USB_DXEPCTL_USBACTEP |
          USB_DXEPCTL_EPTYPE_INTR |
          USB_DXEPCTL_TXFNUM(USB2_EP_NOTIFY) |
          USB_DXEPCTL_SETD0PID;
    usb2_wr(USB_DIEPCTL(USB2_EP_NOTIFY), ctl);

    /* CDC data OUT. */
    ctl = USB_DXEPCTL_MPS(usb2_bulk_mps) |
          USB_DXEPCTL_USBACTEP |
          USB_DXEPCTL_EPTYPE_BULK |
          USB_DXEPCTL_SETD0PID;
    usb2_wr(USB_DOEPCTL(USB2_EP_DATA_OUT), ctl);

    /* CDC data IN. */
    ctl = USB_DXEPCTL_MPS(usb2_bulk_mps) |
          USB_DXEPCTL_USBACTEP |
          USB_DXEPCTL_EPTYPE_BULK |
          USB_DXEPCTL_TXFNUM(USB2_EP_DATA_IN) |
          USB_DXEPCTL_SETD0PID;
    usb2_wr(USB_DIEPCTL(USB2_EP_DATA_IN), ctl);

    usb2_bulk_busy = 0;
    usb2_arm_data_out();
}

static int usb2_start_bulk_in(const uint8_t *buf, uint32_t len)
{
    uint32_t words;

    if (!usb2_configured || usb2_bulk_busy || !len)
        return 0;
    if (len > usb2_bulk_mps)
        len = usb2_bulk_mps;

    words = (len + 3U) >> 2;
    if ((usb2_rd(USB_DTXFSTS(USB2_EP_DATA_IN)) & 0xffffU) < words)
        return 0;

    usb2_bulk_busy = 1;
    usb2_wr(USB_DIEPINT(USB2_EP_DATA_IN), 0xffffffffU);
    usb2_wr(USB_DIEPTSIZ(USB2_EP_DATA_IN),
            USB_DXEPTSIZ_PKTCNT(1) | USB_DXEPTSIZ_XFERSIZE(len));
    usb2_wr(USB_DIEPCTL(USB2_EP_DATA_IN),
            usb2_rd(USB_DIEPCTL(USB2_EP_DATA_IN)) |
            USB_DXEPCTL_EPENA | USB_DXEPCTL_CNAK);
    usb2_fifo_write(USB2_EP_DATA_IN, buf, len);
    return 1;
}

static void usb2_kick_log_tx(void)
{
    uint32_t n = 0;

    if (!usb2_configured || usb2_bulk_busy)
        return;

    /* POC8-PING: terminal echo/command replies have priority, then use any
     * room in the same BULK-IN packet for the normal Emu68 log stream. */
    while (usb2_echo_tail != usb2_echo_head && n < usb2_bulk_mps) {
        usb2_tx_stage[n++] = usb2_echo_ring[usb2_echo_tail];
        usb2_echo_tail = (usb2_echo_tail + 1U) & (USB2_ECHO_RING_SIZE - 1U);
    }

    while (usb2_log_tail != usb2_log_head && n < usb2_bulk_mps) {
        usb2_tx_stage[n++] = usb2_log_ring[usb2_log_tail];
        usb2_log_tail = (usb2_log_tail + 1U) & (USB2_LOG_RING_SIZE - 1U);
    }

    if (n && !usb2_start_bulk_in(usb2_tx_stage, n)) {
        /* Restore is intentionally avoided: the FIFO being temporarily full
         * should be rare, but losing a debug packet is preferable to blocking
         * the Emu68 kprintf path. */
    }
}

static void usb2_on_reset(void)
{
    usb2_configured = 0;
    usb2_pending_address = 0;
    usb2_apply_address = 0;
    usb2_bulk_busy = 0;
    usb2_ep0_out_kind = 0;
    usb2_ep0_state = USB2_EP0_IDLE;
    usb2_echo_head = 0;
    usb2_echo_tail = 0;
    usb2_cmd_len = 0;
    usb2_cmd_last_was_cr = 0;

    usb2_set_address_now(0);
    usb2_flush_fifos();

    for (unsigned ep = 0; ep <= USB2_EP_DATA_IN; ++ep) {
        usb2_wr(USB_DIEPINT(ep), 0xffffffffU);
        usb2_wr(USB_DOEPINT(ep), 0xffffffffU);
    }

    usb2_wr(USB_DAINTMSK,
            USB_DAINT_INEP(0) |
            USB_DAINT_INEP(USB2_EP_NOTIFY) |
            USB_DAINT_INEP(USB2_EP_DATA_IN) |
            USB_DAINT_OUTEP(0) |
            USB_DAINT_OUTEP(USB2_EP_DATA_OUT));

    usb2_ep0_arm_setup();
}

static void usb2_on_enum_done(void)
{
    uint32_t spd = usb2_rd(USB_DSTS) & USB_DSTS_ENUMSPD_MASK;
    usb2_bulk_mps = (spd == USB_DSTS_ENUMSPD_HS) ? 512U : 64U;
    usb2_ep0_arm_setup();
}

static void usb2_handle_setup(const struct usb2_setup_packet *r)
{
    uint16_t wValue  = r->wValue;
    uint16_t wLength = r->wLength;
    uint8_t tmp[80];
    const uint8_t *data = 0;
    uint16_t len = 0;

    usb2_diag.last_bmRequestType = r->bmRequestType;
    usb2_diag.last_bRequest = r->bRequest;
    usb2_diag.last_wValue = r->wValue;
    usb2_diag.last_wIndex = r->wIndex;
    usb2_diag.last_wLength = r->wLength;

    /* Standard IN requests. */
    if (r->bRequest == USB_REQ_GET_DESCRIPTOR && (r->bmRequestType & 0x80U)) {
        uint8_t type = (uint8_t)(wValue >> 8);
        uint8_t idx  = (uint8_t)wValue;

        if (type == USB_DT_DEVICE) {
            usb2_diag.get_device++;
            data = usb2_device_desc;
            len = sizeof(usb2_device_desc);
        } else if (type == USB_DT_CONFIG) {
            usb2_diag.get_config++;
            for (uint32_t i = 0; i < sizeof(usb2_config_desc); ++i)
                tmp[i] = usb2_config_desc[i];
            /* EP2 OUT MPS at bytes 57/58; EP3 IN at 64/65. */
            tmp[57] = (uint8_t)usb2_bulk_mps;
            tmp[58] = (uint8_t)(usb2_bulk_mps >> 8);
            tmp[64] = (uint8_t)usb2_bulk_mps;
            tmp[65] = (uint8_t)(usb2_bulk_mps >> 8);
            data = tmp;
            len = sizeof(usb2_config_desc);
        } else if (type == USB_DT_STRING) {
            usb2_diag.get_string++;
            if (idx == 0) { data = usb2_str0; len = sizeof(usb2_str0); }
            else if (idx == 1) { data = usb2_str1; len = sizeof(usb2_str1); }
            else if (idx == 2) { data = usb2_str2; len = sizeof(usb2_str2); }
            else if (idx == 3) { data = usb2_str3; len = sizeof(usb2_str3); }
        }

        if (!data) {
            usb2_ep0_stall();
            return;
        }
        if (len > wLength)
            len = wLength;
        usb2_ep0_state = USB2_EP0_IN_DATA;
        usb2_ep0_send(data, len);
        return;
    }

    if (r->bmRequestType == 0x00 && r->bRequest == USB_REQ_SET_ADDRESS) {
        /* Synopsys DWC2 expects DCFG.DAD to be programmed before the
         * status IN ZLP.  TinyUSB's DWC2 DCD does the same. */
        usb2_diag.set_address++;
        usb2_pending_address = (uint8_t)(wValue & 0x7fU);
        usb2_set_address_now(usb2_pending_address);
        usb2_apply_address = 0;
        usb2_ep0_state = USB2_EP0_IN_STATUS;
        usb2_ep0_zlp();
        return;
    }

    if (r->bmRequestType == 0x00 && r->bRequest == USB_REQ_SET_CONFIGURATION) {
        usb2_diag.set_configuration++;
        usb2_configured = (uint8_t)(wValue & 0xffU) ? 1U : 0U;
        if (usb2_configured)
            usb2_configure_endpoints();
        usb2_ep0_state = USB2_EP0_IN_STATUS;
        usb2_ep0_zlp();
        return;
    }

    if (r->bmRequestType == 0x80 && r->bRequest == USB_REQ_GET_CONFIGURATION) {
        tmp[0] = usb2_configured ? 1U : 0U;
        usb2_ep0_state = USB2_EP0_IN_DATA;
        usb2_ep0_send(tmp, wLength ? 1U : 0U);
        return;
    }

    if ((r->bmRequestType & 0x60U) == 0 && r->bRequest == USB_REQ_GET_STATUS) {
        tmp[0] = 0;
        tmp[1] = 0;
        usb2_ep0_state = USB2_EP0_IN_DATA;
        usb2_ep0_send(tmp, wLength < 2U ? wLength : 2U);
        return;
    }

    if ((r->bmRequestType == 0x81) && r->bRequest == USB_REQ_GET_INTERFACE) {
        tmp[0] = 0;
        usb2_ep0_state = USB2_EP0_IN_DATA;
        usb2_ep0_send(tmp, wLength ? 1U : 0U);
        return;
    }

    if ((r->bmRequestType == 0x01) && r->bRequest == USB_REQ_SET_INTERFACE) {
        usb2_ep0_state = USB2_EP0_IN_STATUS;
        usb2_ep0_zlp();
        return;
    }

    /* CDC ACM class requests addressed to interface 0. */
    if (r->bmRequestType == 0x21 && r->wIndex == 0) {
        if (r->bRequest == CDC_REQ_SET_LINE_CODING && wLength == 7U) {
            usb2_ep0_out_kind = 1;
            usb2_ep0_state = USB2_EP0_OUT_DATA;
            usb2_ep0_arm_out(7);
            return;
        }
        if (r->bRequest == CDC_REQ_SET_CONTROL_LINE_STATE ||
            r->bRequest == CDC_REQ_SEND_BREAK) {
            usb2_ep0_state = USB2_EP0_IN_STATUS;
            usb2_ep0_zlp();
            return;
        }
    }

    if (r->bmRequestType == 0xa1 && r->wIndex == 0 &&
        r->bRequest == CDC_REQ_GET_LINE_CODING) {
        len = wLength < 7U ? wLength : 7U;
        usb2_ep0_state = USB2_EP0_IN_DATA;
        usb2_ep0_send(usb2_line_coding, len);
        return;
    }

    /* Be permissive for endpoint CLEAR_FEATURE(HALT) during host recovery. */
    if ((r->bmRequestType & 0x7fU) == 0x02 &&
        r->bRequest == USB_REQ_CLEAR_FEATURE && wValue == 0) {
        usb2_ep0_state = USB2_EP0_IN_STATUS;
        usb2_ep0_zlp();
        return;
    }

    usb2_ep0_stall();
}

static void usb2_poll_rx(void)
{
    usb2_diag.rxflvl++;
    while (usb2_rd(USB_GINTSTS) & USB_GINTSTS_RXFLVL) {
        uint32_t st = usb2_rd(USB_GRXSTSP);
        usb2_diag.rx_entries++;
        usb2_diag.last_grxstsp = st;
        uint32_t pkt = USB_GRXSTS_PKTSTS(st);
        uint32_t len = USB_GRXSTS_BYTECNT(st);
        uint32_t ep = USB_GRXSTS_EPNUM(st);

        if (pkt == USB_PKTSTS_SETUPRX && ep == 0 && len == 8U) {
            uint8_t raw[8];
            usb2_diag.setup_rx++;
            struct usb2_setup_packet r;
            usb2_fifo_read(raw, 8);
            r.bmRequestType = raw[0];
            r.bRequest      = raw[1];
            r.wValue  = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
            r.wIndex  = (uint16_t)(raw[4] | ((uint16_t)raw[5] << 8));
            r.wLength = (uint16_t)(raw[6] | ((uint16_t)raw[7] << 8));
            usb2_handle_setup(&r);
            continue;
        }

        if (pkt == USB_PKTSTS_OUTRX) {
            usb2_diag.out_rx++;
            if (len == 0U) {
                /* Zero-length OUT status packet for an IN control transfer. */
                if (ep == 0 && usb2_ep0_state == USB2_EP0_OUT_STATUS) {
                    usb2_ep0_state = USB2_EP0_IDLE;
                }
                continue;
            }
            if (ep == 0 && usb2_ep0_out_kind == 1) {
                uint32_t n = len < 7U ? len : 7U;
                usb2_fifo_read(usb2_line_coding, n);
                if (len > n)
                    usb2_drain_fifo(len - n);
                usb2_ep0_out_kind = 0;
                usb2_ep0_state = USB2_EP0_IN_STATUS;
                usb2_ep0_zlp();
            } else if (ep == USB2_EP_DATA_OUT) {
                /* POC8-PING: feed CDC data to the CPU2-local line parser.
                 * Replies and terminal echo use the private console TX ring;
                 * no kprintf and no cross-core command execution are involved. */
                usb2_console_from_fifo(len);
            } else {
                usb2_drain_fifo(len);
            }
        }
    }
}

static void usb2_poll_epints(void)
{
    uint32_t daint = usb2_rd(USB_DAINT);

    if (daint & USB_DAINT_INEP(0)) {
        uint32_t i = usb2_rd(USB_DIEPINT(0));
        if (i)
            usb2_wr(USB_DIEPINT(0), i);
        if (i & USB_DXEPINT_XFERCOMPL) {
            usb2_diag.in0_done++;
            if (usb2_ep0_state == USB2_EP0_IN_STATUS) {
                usb2_ep0_state = USB2_EP0_IDLE;
                usb2_ep0_arm_setup();
            } else if (usb2_ep0_state == USB2_EP0_IN_DATA) {
                /* Host must now terminate the IN control transfer with OUT ZLP. */
                usb2_ep0_state = USB2_EP0_OUT_STATUS;
                usb2_ep0_arm_out(0);
            }
        }
    }

    if (daint & USB_DAINT_OUTEP(0)) {
        uint32_t i = usb2_rd(USB_DOEPINT(0));
        if (i)
            usb2_wr(USB_DOEPINT(0), i);
        if (i & USB_DXEPINT_XFERCOMPL) {
            usb2_diag.out0_done++;
            if (usb2_ep0_state == USB2_EP0_OUT_STATUS) {
                usb2_ep0_state = USB2_EP0_IDLE;
                usb2_ep0_arm_setup();
            }
        }
        /* Do NOT re-arm merely because SETUP is asserted: the RX FIFO
         * handler owns setup reception and the current control transfer state. */
    }

    if (daint & USB_DAINT_INEP(USB2_EP_NOTIFY)) {
        uint32_t i = usb2_rd(USB_DIEPINT(USB2_EP_NOTIFY));
        if (i)
            usb2_wr(USB_DIEPINT(USB2_EP_NOTIFY), i);
    }

    if (daint & USB_DAINT_INEP(USB2_EP_DATA_IN)) {
        uint32_t i = usb2_rd(USB_DIEPINT(USB2_EP_DATA_IN));
        if (i)
            usb2_wr(USB_DIEPINT(USB2_EP_DATA_IN), i);
        if (i & USB_DXEPINT_XFERCOMPL)
            usb2_bulk_busy = 0;
    }

    if (daint & USB_DAINT_OUTEP(USB2_EP_DATA_OUT)) {
        uint32_t i = usb2_rd(USB_DOEPINT(USB2_EP_DATA_OUT));
        if (i)
            usb2_wr(USB_DOEPINT(USB2_EP_DATA_OUT), i);
        if (i & USB_DXEPINT_XFERCOMPL)
            usb2_arm_data_out();
    }
}

/* One bounded polling pass.  Safe to call from the kprintf character path. */
void emu68_usb_poll(void)
{
    uint32_t g;

    if (!usb2_hw_up)
        return;

    g = usb2_rd(USB_GINTSTS);
    usb2_diag.polls++;
    usb2_diag.last_gintsts = g;
    usb2_diag.gint_or |= g;

    if (g & USB_GINTSTS_USBRST) {
        usb2_diag.usbrst++;
        usb2_wr(USB_GINTSTS, USB_GINTSTS_USBRST);
        usb2_on_reset();
    }

    if (g & USB_GINTSTS_ENUMDONE) {
        usb2_diag.enumdone++;
        usb2_wr(USB_GINTSTS, USB_GINTSTS_ENUMDONE);
        usb2_on_enum_done();
    }

    if (g & USB_GINTSTS_RXFLVL)
        usb2_poll_rx();

    if (g & (USB_GINTSTS_IEPINT | USB_GINTSTS_OEPINT))
        usb2_poll_epints();

    usb2_kick_log_tx();
}

/* Runtime entry used by PiStorm Classic CPU2.  It stays disabled while the
 * bootstrap core owns DWC2 during initial enumeration. */
void emu68_usb_housekeeper_poll(void)
{
    if (usb2_housekeeper_enabled) {
        emu68_usb_poll();

        if (usb2_reboot_pending) {
            uint64_t now;
            asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));

            if ((int64_t)(now - usb2_reboot_deadline) >= 0) {
                usb2_reboot_pending = 0;
                asm volatile("dmb sy" ::: "memory");
                ps_full_reboot();
            }
        }
    }
}

/*
 * Called SYSTEMWIDE by support_rpi.c from the PiStorm Classic kprintf mux.
 *
 * POC6 rule: this path NEVER touches DWC2 registers.  It only appends to the
 * software TX ring.  The PiStorm Classic housekeeper on CPU2 is the sole
 * runtime owner of emu68_usb_poll(), avoiding concurrent MMIO/FIFO access.
 */
static void usb2_log_ring_put(uint8_t c)
{
    uint32_t next;

    next = (usb2_log_head + 1U) & (USB2_LOG_RING_SIZE - 1U);
    if (next == usb2_log_tail)
        usb2_log_tail = (usb2_log_tail + 1U) & (USB2_LOG_RING_SIZE - 1U);

    usb2_log_ring[usb2_log_head] = c;
    asm volatile("dmb sy" ::: "memory");
    usb2_log_head = next;
}

void emu68_usb_putc(char c)
{
    /*
     * POC6.1: queue from the very first kprintf, even before DWC2 exists.
     * Queue from the earliest boot messages so the backlog can be drained:
     * once CDC becomes configured, the accumulated backlog is drained.
     *
     * PuTTY/Windows serial terminals expect CRLF for a normal new line.
     * Keep this translation local to USB so the underlying serial semantics
     * remain completely unchanged.
     */
    if (c == '\n')
        usb2_log_ring_put((uint8_t)'\r');
    usb2_log_ring_put((uint8_t)c);

    /* Wake CPU2 only once the USB runtime owner can actually service DWC2. */
    if (usb2_hw_up)
        asm volatile("sev" ::: "memory");
}

static int usb2_hw_init(void)
{
    uint32_t v;

    usb2_housekeeper_enabled = 0;

    (void)set_power_state(3, 3);      /* legacy USB domain */
    usb2_delay_ms(20);

    v = usb2_rd(USB_GSNPSID);
    kprintf("[USB-CDC] GSNPSID=%08x\n", v);
    if ((v & 0xffff0000U) != 0x4f540000U) {
        kprintf("[USB-CDC] no Synopsys OTG core at %08x\n", (uint32_t)USB2_BASE);
        return 0;
    }

    usb2_wr(USB_DCTL, usb2_rd(USB_DCTL) | USB_DCTL_SFTDISCON);

    v = usb2_rd(USB_GUSBCFG);
    v &= ~(USB_GUSBCFG_FORCEHOSTMODE |
           USB_GUSBCFG_HNPCAP |
           USB_GUSBCFG_SRPCAP |
           USB_GUSBCFG_TOUTCAL_MASK);
    v |= USB_GUSBCFG_FORCEDEVMODE | 7U;
    usb2_wr(USB_GUSBCFG, v);
    usb2_delay_ms(25);

    if (!usb2_wait_mask(USB_GRSTCTL, USB_GRSTCTL_AHBIDLE,
                        USB_GRSTCTL_AHBIDLE, 5000000U)) {
        kprintf("[USB-CDC] AHB idle timeout\n");
        return 0;
    }

    usb2_wr(USB_GRSTCTL, USB_GRSTCTL_CSFTRST);
    if (!usb2_wait_mask(USB_GRSTCTL, USB_GRSTCTL_CSFTRST, 0, 5000000U)) {
        kprintf("[USB-CDC] core reset timeout\n");
        return 0;
    }
    usb2_delay_ms(10);

    v = usb2_rd(USB_GUSBCFG);
    v &= ~USB_GUSBCFG_FORCEHOSTMODE;
    v |= USB_GUSBCFG_FORCEDEVMODE;
    usb2_wr(USB_GUSBCFG, v);
    usb2_delay_ms(25);

    if (usb2_rd(USB_GINTSTS) & USB_GINTSTS_CURMODE_HOST) {
        kprintf("[USB-CDC] controller remained in HOST mode\n");
        return 0;
    }

    /* PIO only, polling only. */
    v = usb2_rd(USB_GAHBCFG);
    v &= ~(USB_GAHBCFG_DMA_EN | USB_GAHBCFG_GLBL_INTR_EN);
    usb2_wr(USB_GAHBCFG, v);
    usb2_wr(USB_GINTMSK, 0);

    /* FIFO RAM layout, units are 32-bit words. */
    usb2_wr(USB_GRXFSIZ, 256U);                          /* RX: 0..255 */
    usb2_wr(USB_GNPTXFSIZ, (128U << 16) | 256U);        /* EP0: 256..383 */
    usb2_wr(USB_DPTXFSIZ(USB2_EP_NOTIFY),
            (64U << 16) | 384U);                        /* EP1: 384..447 */
    usb2_wr(USB_DPTXFSIZ(USB2_EP_DATA_IN),
            (128U << 16) | 448U);                       /* EP3: 448..575 */
    usb2_flush_fifos();

    v = usb2_rd(USB_DCFG);
    v &= ~(USB_DCFG_DEVADDR_MASK | USB_DCFG_DEVSPD_MASK);
    v |= USB_DCFG_DEVSPD_HS;
    usb2_wr(USB_DCFG, v);

    usb2_wr(USB_DIEPMSK, USB_DXEPINT_XFERCOMPL);
    usb2_wr(USB_DOEPMSK, USB_DXEPINT_XFERCOMPL | USB_DXEPINT_SETUP);

    /*
     * Do NOT clear usb2_log_head/tail here.  kprintf output produced before
     * DWC2 initialization is the POC6.1 early-boot backlog and must survive
     * until the host has configured the CDC ACM device.
     */
    {
        volatile uint8_t *dp = (volatile uint8_t *)&usb2_diag;
        for (uint32_t i = 0; i < sizeof(usb2_diag); ++i) dp[i] = 0;
    }
    usb2_on_reset();

    /* Enable the global mux only after all USB state is valid. */
    usb2_hw_up = 1;

    /* Connect to host. */
    usb2_wr(USB_DCTL, usb2_rd(USB_DCTL) & ~USB_DCTL_SFTDISCON);
    return 1;
}

static void pistorm_classic_usb_cdc_poc3_ep0diag(void)
{
    uint64_t start, now, freq, limit;
    static const char banner[] = "EMU68-USB-CDC-POC14-DEBUG-STATUS-RANGE-REBOOT-EARLYRING-CRLF READY\n";

    kprintf("[USB-CDC] starting persistent CDC ACM POC14-DEBUG-STATUS-RANGE-REBOOT-EARLYRING-CRLF\n");
    if (!usb2_hw_init())
        return;

    kprintf("[USB-CDC] connected as CDC ACM 0525:a4a7; EP0 diagnostic active\n");

    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
    limit = (freq * USB2_INITIAL_ENUM_MS) / 1000U;

    do {
        emu68_usb_poll();
        if (usb2_configured)
            break;
        asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
    } while ((now - start) < limit);

    /* Snapshot diagnostics before kprintf itself causes further USB polls. */
    {
        struct usb2_diag d = usb2_diag;
        uint32_t dsts = usb2_rd(USB_DSTS);
        uint32_t dcfg = usb2_rd(USB_DCFG);
        uint32_t dctl = usb2_rd(USB_DCTL);
        uint32_t daint = usb2_rd(USB_DAINT);

        kprintf("[USB-EP0DIAG] POC5 usbrst=%u enumdone=%u rxflvl=%u rxent=%u setup=%u outrx=%u\n",
                d.usbrst, d.enumdone, d.rxflvl, d.rx_entries, d.setup_rx, d.out_rx);
        kprintf("[USB-EP0DIAG] getdev=%u getcfg=%u getstr=%u setaddr=%u setcfg=%u stalls=%u\n",
                d.get_device, d.get_config, d.get_string, d.set_address,
                d.set_configuration, d.stalls);
        kprintf("[USB-EP0DIAG] in0done=%u out0done=%u ep0state=%u configured=%u mps=%u\n",
                d.in0_done, d.out0_done, (uint32_t)usb2_ep0_state,
                (uint32_t)usb2_configured, (uint32_t)usb2_bulk_mps);
        kprintf("[USB-EP0DIAG] lastsetup=%02x/%02x val=%04x idx=%04x len=%04x lastgrx=%08x\n",
                d.last_bmRequestType, d.last_bRequest, d.last_wValue,
                d.last_wIndex, d.last_wLength, d.last_grxstsp);
        kprintf("[USB-EP0DIAG] gint_or=%08x lastgint=%08x dsts=%08x dcfg=%08x dctl=%08x daint=%08x\n",
                d.gint_or, d.last_gintsts, dsts, dcfg, dctl, daint);
    }

    if (usb2_configured) {
        for (uint32_t i = 0; i < sizeof(banner) - 1U; ++i)
            emu68_usb_putc(banner[i]);
        kprintf("[USB-CDC] POC8-PING configured; EP2 console PING/PONG + early backlog + housekeeper USB active\n");
    } else {
        kprintf("[USB-CDC] POC8-PING host not configured; USB remains connected\n");
    }

    /* Hand DWC2 runtime servicing over to CPU2 only after bootstrap polling
     * is completely finished. */
    asm volatile("dmb sy" ::: "memory");
    usb2_housekeeper_enabled = 1;
    asm volatile("sev" ::: "memory");
}
#endif






void platform_post_init()
{
    void *base_vcmem;
    uint32_t size_vcmem;

    kprintf("[BOOT] Platform post init\n");

    if (get_max_clock_rate(3) != get_clock_rate(3)) {
        kprintf("[BOOT] Changing ARM clock from %d MHz to %d MHz\n", get_clock_rate(3)/1000000, get_max_clock_rate(3)/1000000);
        set_clock_rate(3, get_max_clock_rate(3));
    }
    kprintf("[BOOT] ARM Clock at %d MHz\n", get_clock_rate(3) / 1000000);  
    kprintf("[BOOT] CORE Clock at %d MHz\n", get_clock_rate(4) / 1000000);

    get_vc_memory(&base_vcmem, &size_vcmem);
    kprintf("[BOOT] VC4 memory: %p-%p\n", (intptr_t)base_vcmem, (intptr_t)base_vcmem + size_vcmem - 1);

    if (base_vcmem && size_vcmem)
    {
        mmu_map((uintptr_t)base_vcmem, (uintptr_t)base_vcmem, size_vcmem,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_WRITETHROUGH, 0);
    }

    display_logo();

#if defined(PISTORM_CLASSIC)
    pistorm_classic_usb_cdc_poc3_ep0diag();
#endif

#ifdef PISTORM_ANY_MODEL

#if defined(PISTORM_CLASSIC)
    int usercode = 0;

    if (firmware_file && firmware_size) {
        kprintf("[BOOT] Flashing CPLD firmware...\n");
        ps_cpld_load(firmware_file, firmware_size, 1, 0);
    }
    usercode =ps_cpld_load(firmware_file, firmware_size, 0, 0);

    (void)usercode; //suppress warning for now...for later inclusion in DT
#endif

    kprintf("[BOOT] sending RESET signal to Amiga\n");
    ps_pulse_reset();

    block_c0 = 0;

    ps_write_8_int(0xde1000, 0);
    if (ps_read_8_int(0xde1000) & 0x80)
    {
        if (ps_read_8_int(0xde1000) & 0x80)
        {
            if (!(ps_read_8_int(0xde1000) & 0x80))
            {
                if (ps_read_8_int(0xde1000) & 0x80)
                {
                    kprintf("[BOOT] Gayle appears to be present\n");
                    block_c0 = 1;
                }
            }
        }
    }

    if (block_c0 == 0)
    {
        kprintf("[BOOT] Gayle not detected\n");
    }

    of_node_t *e = dt_find_node("/emu68");
    if (e)
    {
        if (dt_find_property(e, "beamcon0-pal-clear")) {
            ps_write_16_int(0xdff1dc, 0x00);
        }
        else if (dt_find_property(e, "beamcon0-pal-set")) {
            ps_write_16_int(0xdff1dc, 0x20);
        }
    }

#endif

    //*(volatile uint32_t *)0xf3000034 = LE32((7680000) | 0x30000000);
}

#if defined(__BCM2708A0__)
   #define UNICAM_CTRL    0x000
   #define UNICAM_STA     0x004
   #define UNICAM_ANA     0x008
   #define UNICAM_PRI     0x00c
   #define UNICAM_CLK     0x010
   #define UNICAM_DAT0    0x014
   #define UNICAM_DAT1    0x018
   #define UNICAM_DAT2    0x01c
   #define UNICAM_DAT3    0x020
   #define UNICAM_CMP0    0x024
   #define UNICAM_CMP1    0x028
   #define UNICAM_CAP0    0x02c
   #define UNICAM_CAP1    0x030
   #define UNICAM_DBG0    0x0f0
   #define UNICAM_DBG1    0x0f4
   #define UNICAM_DBG2    0x0f8
   #define UNICAM_ICTL    0x100
   #define UNICAM_ISTA    0x104
   #define UNICAM_IDI     0x108
   #define UNICAM_IPIPE   0x10c
   #define UNICAM_IBSA    0x110
   #define UNICAM_IBEA    0x114
   #define UNICAM_IBLS    0x118
   #define UNICAM_IBWP    0x11c
   #define UNICAM_IHWIN   0x120
   #define UNICAM_IHSTA   0x124
   #define UNICAM_IVWIN   0x128
   #define UNICAM_IVSTA   0x12c
   #define UNICAM_DCS     0x200
   #define UNICAM_DBSA    0x204
   #define UNICAM_DBEA    0x208
   #define UNICAM_DBWP    0x20c
#else
   #define UNICAM_CTRL    0x000
   #define UNICAM_STA     0x004
   #define UNICAM_ANA     0x008
   #define UNICAM_PRI     0x00c
   #define UNICAM_CLK     0x010
   #define UNICAM_CLT     0x014
   #define UNICAM_DAT0    0x018
   #define UNICAM_DAT1    0x01c
   #define UNICAM_DAT2    0x020
   #define UNICAM_DAT3    0x024
   #define UNICAM_DLT     0x028
   #define UNICAM_CMP0    0x02c
   #define UNICAM_CMP1    0x030
   #define UNICAM_CAP0    0x034
   #define UNICAM_CAP1    0x038
   #define UNICAM_ICTL    0x100
   #define UNICAM_ISTA    0x104
   #define UNICAM_IDI0    0x108
   #define UNICAM_IPIPE   0x10c
   #define UNICAM_IBSA0   0x110
   #define UNICAM_IBEA0   0x114
   #define UNICAM_IBLS    0x118
   #define UNICAM_IBWP    0x11c
   #define UNICAM_IHWIN   0x120
   #define UNICAM_IHSTA   0x124
   #define UNICAM_IVWIN   0x128
   #define UNICAM_IVSTA   0x12c
   #define UNICAM_ICC     0x130
   #define UNICAM_ICS     0x134
   #define UNICAM_IDC     0x138
   #define UNICAM_IDPO    0x13c
   #define UNICAM_IDCA    0x140
   #define UNICAM_IDCD    0x144
   #define UNICAM_IDS     0x148
   #define UNICAM_DCS     0x200
   #define UNICAM_DBSA0   0x204
   #define UNICAM_DBEA0   0x208
   #define UNICAM_DBWP    0x20c
   #define UNICAM_DBCTL   0x300
   #define UNICAM_IBSA1   0x304
   #define UNICAM_IBEA1   0x308
   #define UNICAM_IDI1    0x30c
   #define UNICAM_DBSA1   0x310
   #define UNICAM_DBEA1   0x314
   #define UNICAM_MISC    0x400
#endif

/*
 * The following bitmasks are from the kernel released by Broadcom
 * for Android - https://android.googlesource.com/kernel/bcm/
 * The Rhea, Hawaii, and Java chips all contain the same VideoCore4
 * Unicam block as BCM2835, as defined in eg
 * arch/arm/mach-rhea/include/mach/rdb_A0/brcm_rdb_cam.h and similar.
 * Values reworked to use the kernel BIT and GENMASK macros.
 *
 * Some of the bit mnenomics have been amended to match the datasheet.
 */
/* UNICAM_CTRL Register */
#define UNICAM_CPE BIT(0)
#define UNICAM_MEM BIT(1)
#define UNICAM_CPR BIT(2)
#define UNICAM_CPM_MASK GENMASK(3, 3)
#define UNICAM_CPM_CSI2 0
#define UNICAM_CPM_CCP2 1
#define UNICAM_SOE BIT(4)
#define UNICAM_DCM_MASK GENMASK(5, 5)
#define UNICAM_DCM_STROBE 0
#define UNICAM_DCM_DATA 1
#define UNICAM_SLS BIT(6)
#define UNICAM_PFT_MASK GENMASK(11, 8)
#define UNICAM_OET_MASK GENMASK(20, 12)

/* UNICAM_STA Register */
#define UNICAM_SYN BIT(0)
#define UNICAM_CS BIT(1)
#define UNICAM_SBE BIT(2)
#define UNICAM_PBE BIT(3)
#define UNICAM_HOE BIT(4)
#define UNICAM_PLE BIT(5)
#define UNICAM_SSC BIT(6)
#define UNICAM_CRCE BIT(7)
#define UNICAM_OES BIT(8)
#define UNICAM_IFO BIT(9)
#define UNICAM_OFO BIT(10)
#define UNICAM_BFO BIT(11)
#define UNICAM_DL BIT(12)
#define UNICAM_PS BIT(13)
#define UNICAM_IS BIT(14)
#define UNICAM_PI0 BIT(15)
#define UNICAM_PI1 BIT(16)
#define UNICAM_FSI_S BIT(17)
#define UNICAM_FEI_S BIT(18)
#define UNICAM_LCI_S BIT(19)
#define UNICAM_BUF0_RDY BIT(20)
#define UNICAM_BUF0_NO BIT(21)
#define UNICAM_BUF1_RDY BIT(22)
#define UNICAM_BUF1_NO BIT(23)
#define UNICAM_DI BIT(24)

#define UNICAM_STA_MASK_ALL                                                    \
  (UNICAM_DL + UNICAM_SBE + UNICAM_PBE + UNICAM_HOE + UNICAM_PLE +             \
   UNICAM_SSC + UNICAM_CRCE + UNICAM_IFO + UNICAM_OFO + UNICAM_PS +            \
   UNICAM_PI0 + UNICAM_PI1)

/* UNICAM_ANA Register */
#define UNICAM_APD BIT(0)
#define UNICAM_BPD BIT(1)
#define UNICAM_AR BIT(2)
#define UNICAM_DDL BIT(3)
#define UNICAM_CTATADJ_MASK GENMASK(7, 4)
#define UNICAM_PTATADJ_MASK GENMASK(11, 8)

/* UNICAM_PRI Register */
#define UNICAM_PE BIT(0)
#define UNICAM_PT_MASK GENMASK(2, 1)
#define UNICAM_NP_MASK GENMASK(7, 4)
#define UNICAM_PP_MASK GENMASK(11, 8)
#define UNICAM_BS_MASK GENMASK(15, 12)
#define UNICAM_BL_MASK GENMASK(17, 16)

/* UNICAM_CLK Register */
#define UNICAM_CLE BIT(0)
#define UNICAM_CLPD BIT(1)
#define UNICAM_CLLPE BIT(2)
#define UNICAM_CLHSE BIT(3)
#define UNICAM_CLTRE BIT(4)
#define UNICAM_CLAC_MASK GENMASK(8, 5)
#define UNICAM_CLSTE BIT(29)

/* UNICAM_CLT Register */
#define UNICAM_CLT1_MASK GENMASK(7, 0)
#define UNICAM_CLT2_MASK GENMASK(15, 8)

/* UNICAM_DATn Registers */
#define UNICAM_DLE BIT(0)
#define UNICAM_DLPD BIT(1)
#define UNICAM_DLLPE BIT(2)
#define UNICAM_DLHSE BIT(3)
#define UNICAM_DLTRE BIT(4)
#define UNICAM_DLSM BIT(5)
#define UNICAM_DLFO BIT(28)
#define UNICAM_DLSTE BIT(29)

#define UNICAM_DAT_MASK_ALL (UNICAM_DLSTE + UNICAM_DLFO)

/* UNICAM_DLT Register */
#define UNICAM_DLT1_MASK GENMASK(7, 0)
#define UNICAM_DLT2_MASK GENMASK(15, 8)
#define UNICAM_DLT3_MASK GENMASK(23, 16)

/* UNICAM_ICTL Register */
#define UNICAM_FSIE BIT(0)
#define UNICAM_FEIE BIT(1)
#define UNICAM_IBOB BIT(2)
#define UNICAM_FCM BIT(3)
#define UNICAM_TFC BIT(4)
#define UNICAM_LIP_MASK GENMASK(6, 5)
#define UNICAM_LCIE_MASK GENMASK(28, 16)

/* UNICAM_IDI0/1 Register */
#define UNICAM_ID0_MASK GENMASK(7, 0)
#define UNICAM_ID1_MASK GENMASK(15, 8)
#define UNICAM_ID2_MASK GENMASK(23, 16)
#define UNICAM_ID3_MASK GENMASK(31, 24)

/* UNICAM_ISTA Register */
#define UNICAM_FSI BIT(0)
#define UNICAM_FEI BIT(1)
#define UNICAM_LCI BIT(2)

#define UNICAM_ISTA_MASK_ALL (UNICAM_FSI + UNICAM_FEI + UNICAM_LCI)

/* UNICAM_IPIPE Register */
#define UNICAM_PUM_MASK GENMASK(2, 0)
/* Unpacking modes */
#define UNICAM_PUM_NONE 0
#define UNICAM_PUM_UNPACK6 1
#define UNICAM_PUM_UNPACK7 2
#define UNICAM_PUM_UNPACK8 3
#define UNICAM_PUM_UNPACK10 4
#define UNICAM_PUM_UNPACK12 5
#define UNICAM_PUM_UNPACK14 6
#define UNICAM_PUM_UNPACK16 7
#define UNICAM_DDM_MASK GENMASK(6, 3)
#define UNICAM_PPM_MASK GENMASK(9, 7)
/* Packing modes */
#define UNICAM_PPM_NONE 0
#define UNICAM_PPM_PACK8 1
#define UNICAM_PPM_PACK10 2
#define UNICAM_PPM_PACK12 3
#define UNICAM_PPM_PACK14 4
#define UNICAM_PPM_PACK16 5
#define UNICAM_DEM_MASK GENMASK(11, 10)
#define UNICAM_DEBL_MASK GENMASK(14, 12)
#define UNICAM_ICM_MASK GENMASK(16, 15)
#define UNICAM_IDM_MASK GENMASK(17, 17)

/* UNICAM_ICC Register */
#define UNICAM_ICFL_MASK GENMASK(4, 0)
#define UNICAM_ICFH_MASK GENMASK(9, 5)
#define UNICAM_ICST_MASK GENMASK(12, 10)
#define UNICAM_ICLT_MASK GENMASK(15, 13)
#define UNICAM_ICLL_MASK GENMASK(31, 16)

/* UNICAM_DCS Register */
#define UNICAM_DIE BIT(0)
#define UNICAM_DIM BIT(1)
#define UNICAM_DBOB BIT(3)
#define UNICAM_FDE BIT(4)
#define UNICAM_LDP BIT(5)
#define UNICAM_EDL_MASK GENMASK(15, 8)

/* UNICAM_DBCTL Register */
#define UNICAM_DBEN BIT(0)
#define UNICAM_BUF0_IE BIT(1)
#define UNICAM_BUF1_IE BIT(2)

/* UNICAM_CMP[0,1] register */
#define UNICAM_PCE BIT(31)
#define UNICAM_GI BIT(9)
#define UNICAM_CPH BIT(8)
#define UNICAM_PCVC_MASK GENMASK(7, 6)
#define UNICAM_PCDT_MASK GENMASK(5, 0)

/* UNICAM_MISC register */
#define UNICAM_FL0 BIT(6)
#define UNICAM_FL1 BIT(9)


#define BIT(n) (UINT32_C(1) << (n))
#define u32 uint32_t
#define AARCH 32
#define GENMASK(h, l) ((~0 - (1 << (l)) + 1) & (~0 >> (AARCH - 1 - (h))))

#define ARM_IO_BASE (uintptr_t)0xf2000000
#define ARM_CSI0_BASE (ARM_IO_BASE + 0x800000)
#define ARM_CSI0_END (ARM_CSI0_BASE + 0x7FF)
#define ARM_CSI0_CLKGATE (ARM_IO_BASE + 0x802000) // 4 bytes
#define ARM_CSI1_BASE (ARM_IO_BASE + 0x801000)
#define ARM_CSI1_END (ARM_CSI1_BASE + 0x7FF)
#define ARM_CSI1_CLKGATE (ARM_IO_BASE + 0x802004) // 4 bytes
#define ARM_CM_BASE (ARM_IO_BASE + 0x101000)
#define ARM_CM_CAM0CTL (ARM_CM_BASE + 0x40)
#define ARM_CM_CAM0DIV (ARM_CM_BASE + 0x44)
#define ARM_CM_CAM1CTL (ARM_CM_BASE + 0x48)
#define ARM_CM_CAM1DIV (ARM_CM_BASE + 0x4C)
#define ARM_CM_PASSWD (0x5A << 24)

static void usleep(uint64_t delta)
{
    volatile struct
    {
        uint32_t LO;
        uint32_t HI;
    } *CLOCK = (volatile void *)0xf2003004;

    uint64_t hi = CLOCK->HI;
    uint64_t lo = CLOCK->LO;
    uint64_t t1, t2;

    if (unlikely(hi != CLOCK->HI))
    {
        hi = CLOCK->HI;
        lo = CLOCK->LO;
    }

    t1 = LE64(hi) | LE32(lo);
    t1 += delta;
    t2 = 0;

    do
    {
        hi = CLOCK->HI;
        lo = CLOCK->LO;
        if (unlikely(hi != CLOCK->HI))
        {
            hi = CLOCK->HI;
            lo = CLOCK->LO;
        }
        t2 = LE64(hi) | LE32(lo);
    } while (t2 < t1);
}

void setup_csiclk()
{
    *(volatile uint32_t *)(ARM_CM_CAM1CTL) = LE32(ARM_CM_PASSWD | (1 << 5));
    usleep(100);
    while ((*(volatile uint32_t *)(ARM_CM_CAM1CTL)) & LE32(1 << 7)) {}
    usleep(100);
    *(volatile uint32_t *)(ARM_CM_CAM1DIV) =
        LE32(ARM_CM_PASSWD | (4 << 12)); // divider , 12=100MHz on pi3 ??
    usleep(100);
    *(volatile uint32_t *)(ARM_CM_CAM1CTL) =
        LE32(ARM_CM_PASSWD | 6 | (1 << 4)); // pll? 6=plld, 5=pllc
    usleep(100);
    while (((*(volatile uint32_t *)(ARM_CM_CAM1CTL)) & LE32(1 << 7)) == 0) {}
    usleep(100);
}

void ClockWrite(uint32_t nValue)
{
    *(volatile uint32_t *)(ARM_CSI1_CLKGATE) = LE32(ARM_CM_PASSWD | nValue);
}


void SetField(uint32_t *pValue, uint32_t nValue, uint32_t nMask)
{
    uint32_t nTempMask = nMask;
    while (!(nTempMask & 1)) {
        nValue <<= 1;
        nTempMask >>= 1;
    }

    *pValue = (*pValue & ~nMask) | nValue;
}

uint32_t ReadReg(uint32_t nOffset)
{
    uint32_t temp;
    temp = LE32(*(volatile uint32_t *)(ARM_CSI1_BASE + nOffset));

    return temp;
}

void WriteReg(uint32_t nOffset, uint32_t nValue)
{
    *(volatile uint32_t *)(ARM_CSI1_BASE + nOffset) = LE32(nValue);
}

void WriteRegField(uint32_t nOffset, uint32_t nValue, uint32_t nMask)
{
    uint32_t nBuffer = ReadReg(nOffset);
    SetField(&nBuffer, nValue, nMask);
    WriteReg(nOffset, nBuffer);
}


void unicam_run(uintptr_t address , uint8_t lanes, uint8_t datatype, uint32_t width , uint32_t height , uint8_t bbp)
{
    //enable power domain
    enable_unicam_domain();

    //enable to clock to unicam
    setup_csiclk();

    // Enable lane clocks (2 lanes)
    ClockWrite(0b010101);

    // Basic init
    WriteReg(UNICAM_CTRL, UNICAM_MEM);

    // Enable analogue control, and leave in reset.
    uint32_t nValue = UNICAM_AR;
    SetField(&nValue, 7, UNICAM_CTATADJ_MASK);
    SetField(&nValue, 7, UNICAM_PTATADJ_MASK);
    WriteReg(UNICAM_ANA, nValue);

    usleep(1000);

    // Come out of reset
    WriteRegField(UNICAM_ANA, 0, UNICAM_AR);

    // Peripheral reset
    WriteRegField(UNICAM_CTRL, 1, UNICAM_CPR);
    WriteRegField(UNICAM_CTRL, 0, UNICAM_CPR);

    WriteRegField(UNICAM_CTRL, 0, UNICAM_CPE);

    // Enable Rx control (CSI2 DPHY)
    nValue = ReadReg(UNICAM_CTRL);
    SetField(&nValue, UNICAM_CPM_CSI2, UNICAM_CPM_MASK);
    SetField(&nValue, UNICAM_DCM_STROBE, UNICAM_DCM_MASK);

    // Packet framer timeout
    SetField(&nValue, 0xf, UNICAM_PFT_MASK);
    SetField(&nValue, 128, UNICAM_OET_MASK);
    WriteReg(UNICAM_CTRL, nValue);

    WriteReg(UNICAM_IHWIN, 0);
    WriteReg(UNICAM_IVWIN, 0);

    // AXI bus access QoS setup
    nValue = ReadReg(UNICAM_PRI);
    SetField(&nValue, 0, UNICAM_BL_MASK);
    SetField(&nValue, 0, UNICAM_BS_MASK);
    SetField(&nValue, 0xe, UNICAM_PP_MASK);
    SetField(&nValue, 8, UNICAM_NP_MASK);
    SetField(&nValue, 2, UNICAM_PT_MASK);
    SetField(&nValue, 1, UNICAM_PE);
    WriteReg(UNICAM_PRI, nValue);

    WriteRegField(UNICAM_ANA, 0, UNICAM_DDL);

    uint32_t nLineIntFreq = height >> 2;
    nValue = UNICAM_FSIE | UNICAM_FEIE | UNICAM_IBOB;
    SetField(&nValue, nLineIntFreq >= 128 ? nLineIntFreq : 128, UNICAM_LCIE_MASK);
    WriteReg(UNICAM_ICTL, nValue);
    WriteReg(UNICAM_STA, UNICAM_STA_MASK_ALL);
    WriteReg(UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);

    WriteRegField(UNICAM_CLT, 2, UNICAM_CLT1_MASK); // tclk_term_en
    WriteRegField(UNICAM_CLT, 6, UNICAM_CLT2_MASK); // tclk_settle
    WriteRegField(UNICAM_DLT, 2, UNICAM_DLT1_MASK); // td_term_en
    WriteRegField(UNICAM_DLT, 6, UNICAM_DLT2_MASK); // ths_settle
    WriteRegField(UNICAM_DLT, 0, UNICAM_DLT3_MASK); // trx_enable

    WriteRegField(UNICAM_CTRL, 0, UNICAM_SOE);

    // Packet compare setup - required to avoid missing frame ends
    nValue = 0;
    SetField(&nValue, 1, UNICAM_PCE);
    SetField(&nValue, 1, UNICAM_GI);
    SetField(&nValue, 1, UNICAM_CPH);
    SetField(&nValue, 0, UNICAM_PCVC_MASK);
    SetField(&nValue, 1, UNICAM_PCDT_MASK);
    WriteReg(UNICAM_CMP0, nValue);

    // Enable clock lane and set up terminations (CSI2 DPHY, non-continous clock)
    nValue = 0;
    SetField(&nValue, 1, UNICAM_CLE);
    SetField(&nValue, 1, UNICAM_CLLPE);
    WriteReg(UNICAM_CLK, nValue);

    // Enable required data lanes with appropriate terminations.
    // The same value needs to be written to UNICAM_DATn registers for
    // the active lanes, and 0 for inactive ones.
    // (CSI2 DPHY, non-continous clock, 2 data lanes)
    nValue = 0;
    SetField(&nValue, 1, UNICAM_DLE);
    SetField(&nValue, 1, UNICAM_DLLPE);
    WriteReg(UNICAM_DAT0, nValue);
    if (lanes == 1)
        WriteReg(UNICAM_DAT1, 0);
    if (lanes == 2)
        WriteReg(UNICAM_DAT1, nValue);

    WriteReg(UNICAM_IBLS, width*(bbp/8));

    // Write DMA buffer address

    WriteReg(UNICAM_IBSA0, ((u32)(address) & ~0xC0000000) | 0xC0000000);
    WriteReg(UNICAM_IBEA0,
            ((uint32_t)(address + (width * height * (bbp/8))) & ~0xC0000000) | 0xC0000000);

    // Set packing configuration
    uint32_t nUnPack = UNICAM_PUM_NONE;
    uint32_t nPack = UNICAM_PPM_NONE;

    nValue = 0;
    SetField(&nValue, nUnPack, UNICAM_PUM_MASK);
    SetField(&nValue, nPack, UNICAM_PPM_MASK);
    WriteReg(UNICAM_IPIPE, nValue);

    // CSI2 mode, hardcode VC 0 for now.
    WriteReg(UNICAM_IDI0, (0 << 6) | datatype);

    nValue = ReadReg(UNICAM_MISC);
    SetField(&nValue, 1, UNICAM_FL0);
    SetField(&nValue, 1, UNICAM_FL1);
    WriteReg(UNICAM_MISC, nValue);

    // Clear ED setup
    WriteReg(UNICAM_DCS, 0);

    // Enable peripheral
    WriteRegField(UNICAM_CTRL, 1, UNICAM_CPE);

    // Load image pointers
    WriteRegField(UNICAM_ICTL, 1, UNICAM_LIP_MASK);
}

#define UNICAM_MODE     0x22
#define UNICAM_WIDTH    720
#define UNICAM_HEIGHT   576
#define UNICAM_BPP      16

/* 
    Report stealth mode - in normal cases add a text on the screen, unless user requested 
    framethrower support during boot. In that case, set the framethrower output.
*/
void platform_report_stealth()
{
    int unicam_initialized = 0;
    of_property_t *prop = dt_find_property(dt_find_node("/emu68/unicam"), "status");

    if (prop && strcmp(prop->op_value, "okay") == 0)
    {
        struct Size sz = { 720, 576 };
        void *framebuffer = NULL;
        uint32_t pitch = 0;

        init_display(sz, &framebuffer, &pitch);

        setup_csiclk();
        unicam_run((uintptr_t)framebuffer, 1, UNICAM_MODE, 720, 576, UNICAM_BPP);
        
        unicam_initialized = 1;
    }

    if (!unicam_initialized)
    {
        struct Size sz = get_display_size();
        uint32_t last_x = text_x;
        uint32_t last_y = text_y;

        /* Get the logo height for stealth mode. Assume PiStorm */
        uint32_t logo_h = 10;
        logo_h += (logo_emu68[2] << 8) + logo_emu68[3];
        logo_h += logo_pistorm[2] << 8 | logo_pistorm[3];

        uint32_t start_y = (sz.height + logo_h) / 2;
        
        text_y = start_y / 16;
        text_x = (sz.width - 20 * 8) / 16;

        kprintf_pc(__putc, NULL, "!!! STEALTH MODE !!!");

        text_x = last_x;
        text_y = last_y;
    }
}
