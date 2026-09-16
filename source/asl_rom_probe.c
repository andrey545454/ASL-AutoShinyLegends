#include "asl_rom_probe.h"
#include "asl_platform.h"
#include <string.h>

#define PROBE_SCAN_BEGIN 0x00100000u
#define PROBE_SCAN_END   0x40000000u
#define PROBE_FRAME_BUDGET (256u * 1024u)
#define PROBE_PTR_BUDGET   (256u * 1024u)
#define MAX_ROM_HITS 3u
#define MAX_PTR_REFS 3u

/* Crystal Rev 1 / VC keeps the normal ROM layout. */
#define CRYSTAL_UPDATE_DVS_FILE_OFFSET 0x0003E9A8u

static const u8 k_title[] = { 'P','M','_','C','R','Y','S','T','A','L' };
static const u8 k_nintendo_logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
    0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
    0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
};

/* 0f:69a8 LoadEnemyMon.UpdateDVs on Crystal Rev 1. */
static const u8 k_update_dvs_signature[] = {
    0x21,0x0C,0xD2, /* ld hl, wEnemyMonDVs */
    0x78,           /* ld a, b */
    0x22,           /* ld [hli], a */
    0x71            /* ld [hl], c */
};

typedef struct RomProbeRuntime
{
    bool started;
    bool scan_done;
    u32 scan_cursor;
    u32 readable_bytes_scanned;

    u32 rom_hit_count;
    u32 rom_base[MAX_ROM_HITS];
    bool rom_writable[MAX_ROM_HITS];

    bool patch_signature_checked;
    bool patch_signature_match;
    u32 patch_host_address;
    u16 bank0_cave_guest_address;
    u32 bank0_trailing_zero_bytes;

    bool ptr_scan_started;
    bool ptr_scan_done;
    u32 ptr_scan_cursor;
    u32 ptr_ref_count;
    u32 ptr_ref_address[MAX_PTR_REFS];
    u8 ptr_ref_kind[MAX_PTR_REFS];
} RomProbeRuntime;

static RomProbeRuntime g_probe;

static u32 add_sat(u32 a,u32 b)
{
    const u32 c=a+b;
    return c<a ? 0xFFFFFFFFu : c;
}

static bool query_mapping(u32 address,MemInfo *info)
{
    PageInfo page;
    if (!info) return false;
    return R_SUCCEEDED(svcQueryMemory(info,&page,address));
}

static u32 mapping_end(const MemInfo *info)
{
    if (!info || info->size==0u) return 0u;
    return add_sat(info->base_addr,info->size);
}

static bool is_readable_map(const MemInfo *info)
{
    return info && (info->perm & MEMPERM_READ)!=0;
}

static bool is_writable_address(u32 address)
{
    MemInfo info;
    if (!query_mapping(address,&info)) return false;
    return (info.perm & MEMPERM_WRITE)!=0;
}

static bool bytes_equal(u32 address,const u8 *needle,u32 size)
{
    if (!needle || size==0u || !asl_address_is_readable(address,size)) return false;
    return memcmp((const void *)address,needle,size)==0;
}

static bool valid_crystal_header(u32 base)
{
    if (!asl_address_is_readable(base,0x150u)) return false;
    if (*((volatile const u8 *)(base+0x100u))!=0x00u) return false;
    if (*((volatile const u8 *)(base+0x101u))!=0xC3u) return false;
    if (!bytes_equal(base+0x104u,k_nintendo_logo,sizeof(k_nintendo_logo))) return false;
    if (!bytes_equal(base+0x134u,k_title,sizeof(k_title))) return false;
    /* Crystal is CGB-only and MBC3+TIMER+RAM+BATTERY. */
    if (*((volatile const u8 *)(base+0x143u))!=0xC0u) return false;
    if (*((volatile const u8 *)(base+0x147u))!=0x10u) return false;
    return true;
}

static bool known_rom_base(u32 base)
{
    u32 i;
    for (i=0;i<g_probe.rom_hit_count && i<MAX_ROM_HITS;++i)
        if (g_probe.rom_base[i]==base) return true;
    return false;
}

static void inspect_primary_rom(void)
{
    u32 base,zero_count=0u,pos;
    if (g_probe.rom_hit_count==0u || g_probe.patch_signature_checked) return;
    base=g_probe.rom_base[0];
    g_probe.patch_host_address=base+CRYSTAL_UPDATE_DVS_FILE_OFFSET;
    g_probe.patch_signature_checked=true;
    g_probe.patch_signature_match=bytes_equal(g_probe.patch_host_address,k_update_dvs_signature,sizeof(k_update_dvs_signature));

    if (!asl_address_is_readable(base,0x4000u)) return;
    pos=0x4000u;
    while (pos>0u)
    {
        const u8 value=*((volatile const u8 *)(base+pos-1u));
        if (value!=0u) break;
        ++zero_count;
        --pos;
    }
    g_probe.bank0_trailing_zero_bytes=zero_count;
    if (zero_count>0u && zero_count<=0x4000u)
        g_probe.bank0_cave_guest_address=(u16)(0x4000u-zero_count);
}

static void record_rom_hit(u32 base)
{
    u32 index;
    if (known_rom_base(base)) return;
    index=g_probe.rom_hit_count;
    ++g_probe.rom_hit_count;
    if (index<MAX_ROM_HITS)
    {
        g_probe.rom_base[index]=base;
        g_probe.rom_writable[index]=is_writable_address(base);
    }
    inspect_primary_rom();

    /*
     * The normal build only needs one validated writable Crystal ROM mapping.
     * Stop immediately once the exact Rev-1 UpdateDVs signature is confirmed;
     * the older probe builds kept scanning only to collect diagnostics.
     */
    if (g_probe.patch_signature_match && g_probe.rom_writable[0])
        g_probe.scan_done=true;
}

static void scan_for_title_in_range(u32 begin,u32 end)
{
    u32 p;
    if (end<=begin || end-begin<sizeof(k_title)) return;
    for (p=begin;p+sizeof(k_title)<=end;++p)
    {
        if (*((volatile const u8 *)p)!='P') continue;
        if (memcmp((const void *)p,k_title,sizeof(k_title))==0 && p>=0x134u)
        {
            const u32 base=p-0x134u;
            if (valid_crystal_header(base)) record_rom_hit(base);
        }
    }
}

static void advance_title_scan(void)
{
    u32 budget=PROBE_FRAME_BUDGET;
    while (!g_probe.scan_done && budget>0u)
    {
        MemInfo info;
        u32 end,start,avail,chunk,scan_end;
        if (g_probe.scan_cursor>=PROBE_SCAN_END)
        {
            g_probe.scan_done=true;
            break;
        }
        if (!query_mapping(g_probe.scan_cursor,&info) || info.size==0u)
        {
            g_probe.scan_cursor=add_sat(g_probe.scan_cursor,0x1000u);
            continue;
        }
        end=mapping_end(&info);
        if (end<=g_probe.scan_cursor)
        {
            g_probe.scan_cursor=add_sat(g_probe.scan_cursor,0x1000u);
            continue;
        }
        if (!is_readable_map(&info))
        {
            g_probe.scan_cursor=end;
            continue;
        }
        start=g_probe.scan_cursor<info.base_addr?info.base_addr:g_probe.scan_cursor;
        if (start>=end)
        {
            g_probe.scan_cursor=end;
            continue;
        }
        avail=end-start;
        chunk=avail<budget?avail:budget;
        scan_end=start+chunk;
        if (scan_end<end)
        {
            const u32 extra=(u32)sizeof(k_title)-1u;
            const u32 room=end-scan_end;
            scan_end+=room<extra?room:extra;
        }
        scan_for_title_in_range(start,scan_end);
        g_probe.readable_bytes_scanned+=chunk;
        g_probe.scan_cursor=start+chunk;
        budget-=chunk;
    }
}

static bool known_ref(u32 address)
{
    u32 i;
    for (i=0;i<g_probe.ptr_ref_count && i<MAX_PTR_REFS;++i)
        if (g_probe.ptr_ref_address[i]==address) return true;
    return false;
}

static void record_ref(u32 address,u8 kind)
{
    const u32 index=g_probe.ptr_ref_count;
    if (known_ref(address)) return;
    ++g_probe.ptr_ref_count;
    if (index<MAX_PTR_REFS)
    {
        g_probe.ptr_ref_address[index]=address;
        g_probe.ptr_ref_kind[index]=kind;
    }
}

static void scan_pointer_range(u32 begin,u32 end)
{
    const u32 rom=g_probe.rom_base[0];
    const u32 title=rom+0x134u;
    const u32 bank1=rom+0x4000u;
    u32 p=(begin+3u)&~3u;
    while (p+4u<=end)
    {
        const u32 value=*((volatile const u32 *)p);
        if (value==rom) record_ref(p,1u);
        else if (value==title) record_ref(p,2u);
        else if (value==bank1) record_ref(p,3u);
        p+=4u;
    }
}

static void advance_pointer_scan(void)
{
    u32 budget=PROBE_PTR_BUDGET;
    if (!g_probe.ptr_scan_started || g_probe.ptr_scan_done || g_probe.rom_hit_count==0u) return;
    while (!g_probe.ptr_scan_done && budget>0u)
    {
        MemInfo info;
        u32 end,start,avail,chunk;
        if (g_probe.ptr_scan_cursor>=PROBE_SCAN_END)
        {
            g_probe.ptr_scan_done=true;
            break;
        }
        if (!query_mapping(g_probe.ptr_scan_cursor,&info) || info.size==0u)
        {
            g_probe.ptr_scan_cursor=add_sat(g_probe.ptr_scan_cursor,0x1000u);
            continue;
        }
        end=mapping_end(&info);
        if (end<=g_probe.ptr_scan_cursor)
        {
            g_probe.ptr_scan_cursor=add_sat(g_probe.ptr_scan_cursor,0x1000u);
            continue;
        }
        if (!is_readable_map(&info) || (info.perm & MEMPERM_WRITE)==0)
        {
            g_probe.ptr_scan_cursor=end;
            continue;
        }
        start=g_probe.ptr_scan_cursor<info.base_addr?info.base_addr:g_probe.ptr_scan_cursor;
        if (start>=end)
        {
            g_probe.ptr_scan_cursor=end;
            continue;
        }
        avail=end-start;
        chunk=avail<budget?avail:budget;
        scan_pointer_range(start,start+chunk);
        g_probe.ptr_scan_cursor=start+chunk;
        budget-=chunk;
    }
}

void asl_rom_probe_initialize(void)
{
    memset(&g_probe,0,sizeof(g_probe));
    g_probe.started=true;
    g_probe.scan_cursor=PROBE_SCAN_BEGIN;
}

void asl_rom_probe_step(void)
{
    if (!g_probe.started) asl_rom_probe_initialize();
    advance_title_scan();
    advance_pointer_scan();
}

void asl_rom_probe_get_snapshot(AslRomProbeSnapshot *out)
{
    u32 i;
    if (!out) return;
    memset(out,0,sizeof(*out));
    out->started=g_probe.started;
    out->scan_done=g_probe.scan_done;
    out->scan_cursor=g_probe.scan_cursor;
    out->readable_bytes_scanned=g_probe.readable_bytes_scanned;
    out->rom_hit_count=g_probe.rom_hit_count;
    for (i=0;i<3u;++i)
    {
        out->rom_base[i]=g_probe.rom_base[i];
        out->rom_writable[i]=g_probe.rom_writable[i];
        out->ptr_ref_address[i]=g_probe.ptr_ref_address[i];
        out->ptr_ref_kind[i]=g_probe.ptr_ref_kind[i];
    }
    out->patch_signature_checked=g_probe.patch_signature_checked;
    out->patch_signature_match=g_probe.patch_signature_match;
    out->patch_host_address=g_probe.patch_host_address;
    out->bank0_cave_guest_address=g_probe.bank0_cave_guest_address;
    out->bank0_trailing_zero_bytes=g_probe.bank0_trailing_zero_bytes;
    out->ptr_scan_started=g_probe.ptr_scan_started;
    out->ptr_scan_done=g_probe.ptr_scan_done;
    out->ptr_scan_cursor=g_probe.ptr_scan_cursor;
    out->ptr_ref_count=g_probe.ptr_ref_count;
}
