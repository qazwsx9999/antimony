// pe.c - direct Windows x64 PE32+ (.exe) writer.
// Emits a complete executable with .text, .rdata (imports + string literals),
// and .data, importing only GetStdHandle, WriteFile, and ExitProcess from
// kernel32.dll. No external assembler or linker is used.
#include "sb.h"

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void write_section_header(Buf *out, const char name[8],
                                 uint32_t vsize, uint32_t rva,
                                 uint32_t raw_size, uint32_t raw_ptr,
                                 uint32_t ch) {
    buf_bytes(out, name, 8);
    buf_u32(out, vsize);
    buf_u32(out, rva);
    buf_u32(out, raw_size);
    buf_u32(out, raw_ptr);
    buf_u32(out, 0); // PointerToRelocations
    buf_u32(out, 0); // PointerToLinenumbers
    buf_u16(out, 0); // NumberOfRelocations
    buf_u16(out, 0); // NumberOfLinenumbers
    buf_u32(out, ch);
}

void write_pe(const char *path, Image *img) {
    const uint32_t FILE_ALIGN = 0x200;
    const uint32_t SECT_ALIGN = 0x1000;
    const uint64_t IMAGE_BASE = 0x140000000ULL;

    // -----------------------------------------------------------------------
    // 1. Build .rdata: Import Directory + ILT + IAT + hint/names + strings
    // -----------------------------------------------------------------------
    int num_dlls    = img->dll_imports.len;
    int total_slots = img->total_iat_slots;

    const uint32_t IDATA_OFF = 0x00;
    const uint32_t IDATA_SZ  = (uint32_t)((num_dlls + 1) * 20);
    const uint32_t ILT_OFF   = align_up(IDATA_OFF + IDATA_SZ, 8);
    const uint32_t IAT_SZ    = (uint32_t)(total_slots * 8);
    const uint32_t IAT_OFF   = ILT_OFF + IAT_SZ;
    const uint32_t NAMES_OFF = IAT_OFF + IAT_SZ;

    uint32_t text_rva  = SECT_ALIGN; // 0x1000
    uint32_t text_vsz  = (uint32_t)img->text.len ? (uint32_t)img->text.len : 1;
    uint32_t text_raw  = align_up(text_vsz, FILE_ALIGN);

    uint32_t rdata_rva = text_rva + align_up(text_vsz, SECT_ALIGN);

    Buf rdata = {0};
    buf_zero(&rdata, NAMES_OFF); // reserve space for descriptors, ILT, IAT

    for (int d = 0; d < num_dlls; d++) {
        DllImport *di = &img->dll_imports.items[d];
        uint32_t dll_name_off = (uint32_t)rdata.len;
        buf_bytes(&rdata, di->dll_name, strlen(di->dll_name) + 1);
        buf_align(&rdata, 2);

        for (int f = 0; f < di->funcs.len; f++) {
            const char *fn_name = di->funcs.items[f];
            uint32_t hn_off = (uint32_t)rdata.len;
            buf_u16(&rdata, 0); // Hint
            buf_bytes(&rdata, fn_name, strlen(fn_name) + 1);
            buf_align(&rdata, 2);

            int slot = di->first_iat_slot + f;
            uint64_t rva = (uint64_t)(rdata_rva + hn_off);
            memcpy(rdata.data + ILT_OFF + slot * 8, &rva, 8);
            memcpy(rdata.data + IAT_OFF + slot * 8, &rva, 8);
        }

        uint32_t desc_off = IDATA_OFF + (uint32_t)(d * 20);
        uint32_t first_slot_bytes = (uint32_t)(di->first_iat_slot * 8);
        buf_put_u32(&rdata, desc_off + 0,  rdata_rva + ILT_OFF + first_slot_bytes); // OriginalFirstThunk
        buf_put_u32(&rdata, desc_off + 4,  0);                                      // TimeDateStamp
        buf_put_u32(&rdata, desc_off + 8,  0);                                      // ForwarderChain
        buf_put_u32(&rdata, desc_off + 12, rdata_rva + dll_name_off);               // Name
        buf_put_u32(&rdata, desc_off + 16, rdata_rva + IAT_OFF + first_slot_bytes); // FirstThunk
    }

    buf_align(&rdata, 8);
    uint32_t strings_off = (uint32_t)rdata.len;
    buf_bytes(&rdata, img->strings.data, img->strings.len);
    if (rdata.len == strings_off) buf_u8(&rdata, 0);

    uint32_t rdata_vsz = (uint32_t)rdata.len;
    uint32_t rdata_raw = align_up(rdata_vsz, FILE_ALIGN);

    uint32_t data_rva  = rdata_rva + align_up(rdata_vsz, SECT_ALIGN);
    uint32_t data_vsz  = img->data_size ? img->data_size : 8;
    uint32_t data_raw  = align_up((uint32_t)img->data.len, FILE_ALIGN);

    uint32_t image_size = data_rva + align_up(data_vsz, SECT_ALIGN);

    // -----------------------------------------------------------------------
    // 2. Patch RIP-relative displacements in .text
    // -----------------------------------------------------------------------
    for (int i = 0; i < img->nfixups; i++) {
        RipFixup fx = img->fixups[i];
        uint32_t target_rva = 0;
        switch (fx.section) {
        case SEC_RDATA: target_rva = rdata_rva + strings_off + fx.off; break;
        case SEC_DATA:  target_rva = data_rva  + fx.off;               break;
        case SEC_IAT:   target_rva = rdata_rva + IAT_OFF     + fx.off; break;
        default:        die("internal error: bad fixup section");
        }
        uint32_t next_ip_rva = text_rva + fx.pos + 4 + (uint32_t)fx.addend;
        int32_t disp = (int32_t)(target_rva - next_ip_rva);
        buf_put_u32(&img->text, fx.pos, (uint32_t)disp);
    }

    // -----------------------------------------------------------------------
    // 3. Emit DOS header + PE32+ headers + section headers + section bodies
    // -----------------------------------------------------------------------
    uint32_t headers_raw = FILE_ALIGN; // 512 bytes is enough for all headers
    uint32_t text_ptr    = headers_raw;
    uint32_t rdata_ptr   = text_ptr  + text_raw;
    uint32_t data_ptr    = rdata_ptr + rdata_raw;

    Buf out = {0};

    // DOS header (64 bytes), with e_lfanew = 0x40.
    buf_u16(&out, 0x5A4D); // "MZ"
    buf_zero(&out, 58);
    buf_u32(&out, 0x40);   // e_lfanew

    // PE signature + COFF file header (24 bytes).
    buf_bytes(&out, "PE\0\0", 4);
    buf_u16(&out, 0x8664); // IMAGE_FILE_MACHINE_AMD64
    buf_u16(&out, 3);      // NumberOfSections (.text, .rdata, .data)
    buf_u32(&out, 0);      // TimeDateStamp
    buf_u32(&out, 0);      // PointerToSymbolTable
    buf_u32(&out, 0);      // NumberOfSymbols
    buf_u16(&out, 240);    // SizeOfOptionalHeader (PE32+)
    buf_u16(&out, 0x0022); // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    // Optional header PE32+ (240 bytes).
    buf_u16(&out, 0x020B);              // Magic (PE32+)
    buf_u8(&out, 1);                    // MajorLinkerVersion
    buf_u8(&out, 0);                    // MinorLinkerVersion
    buf_u32(&out, text_raw);            // SizeOfCode
    buf_u32(&out, rdata_raw + data_raw);// SizeOfInitializedData
    buf_u32(&out, 0);                   // SizeOfUninitializedData
    buf_u32(&out, text_rva + img->entry);// AddressOfEntryPoint
    buf_u32(&out, text_rva);            // BaseOfCode
    buf_u64(&out, IMAGE_BASE);          // ImageBase
    buf_u32(&out, SECT_ALIGN);          // SectionAlignment
    buf_u32(&out, FILE_ALIGN);          // FileAlignment
    buf_u16(&out, 6);                   // MajorOperatingSystemVersion
    buf_u16(&out, 0);                   // MinorOperatingSystemVersion
    buf_u16(&out, 0);                   // MajorImageVersion
    buf_u16(&out, 0);                   // MinorImageVersion
    buf_u16(&out, 6);                   // MajorSubsystemVersion
    buf_u16(&out, 0);                   // MinorSubsystemVersion
    buf_u32(&out, 0);                   // Win32VersionValue
    buf_u32(&out, image_size);          // SizeOfImage
    buf_u32(&out, headers_raw);         // SizeOfHeaders
    buf_u32(&out, 0);                   // CheckSum
    buf_u16(&out, 3);                   // Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI (console)
    buf_u16(&out, 0x0100);              // DllCharacteristics: NX_COMPAT
    buf_u64(&out, 0x100000);            // SizeOfStackReserve (1 MB)
    buf_u64(&out, 0x10000);             // SizeOfStackCommit  (64 KB)
    buf_u64(&out, 0x100000);            // SizeOfHeapReserve
    buf_u64(&out, 0x1000);              // SizeOfHeapCommit
    buf_u32(&out, 0);                   // LoaderFlags
    buf_u32(&out, 16);                  // NumberOfRvaAndSizes

    // 16 Data Directories (each: RVA u32 + Size u32).
    for (int i = 0; i < 16; i++) {
        if (i == 1) { // Import Table
            buf_u32(&out, rdata_rva + IDATA_OFF);
            buf_u32(&out, IDATA_SZ);
        } else if (i == 12) { // IAT
            buf_u32(&out, rdata_rva + IAT_OFF);
            buf_u32(&out, IAT_SZ);
        } else {
            buf_u32(&out, 0);
            buf_u32(&out, 0);
        }
    }

    // Section headers (3 x 40 bytes).
    write_section_header(&out, ".text\0\0\0",  text_vsz,  text_rva,  text_raw,  text_ptr,  0x60000020u);
    write_section_header(&out, ".rdata\0\0",   rdata_vsz, rdata_rva, rdata_raw, rdata_ptr, 0x40000040u);
    write_section_header(&out, ".data\0\0\0",  data_vsz,  data_rva,  data_raw,  data_ptr,  0xC0000040u);

    buf_align(&out, FILE_ALIGN);

    // .text
    buf_bytes(&out, img->text.data, img->text.len);
    buf_align(&out, FILE_ALIGN);

    // .rdata
    buf_bytes(&out, rdata.data, rdata.len);
    buf_align(&out, FILE_ALIGN);

    // .data
    buf_bytes(&out, img->data.data, img->data.len);
    buf_align(&out, FILE_ALIGN);

    FILE *fp = fopen(path, "wb");
    if (!fp) die("cannot open output file '%s' for writing", path);
    if (fwrite(out.data, 1, out.len, fp) != out.len) {
        fclose(fp);
        die("failed to write output file '%s'", path);
    }
    fclose(fp);
    free(rdata.data);
    free(out.data);
}
