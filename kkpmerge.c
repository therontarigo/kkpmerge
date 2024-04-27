
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Note, this source is intended as a main program only and not as a library:
// It liberally allocates memory without later freeing.
// It assumes little-endian system.

typedef struct _kkpm_source {
    const char *name;
    double      packed_size;
    uint32_t    unpacked_size;
} kkpm_source;

typedef struct _kkpm_symbol {
    const char *name;          // KKP
    double      packed_size;   // KKP
    uint32_t    unpacked_size; // KKP
    uint32_t    source_file;   // KKP
    uint32_t    position;      // KKP
    uint8_t     is_code;       // KKP
    uint32_t    size;          // kkpmerge
} kkpm_symbol;

typedef struct _kkpm_byte {
    double      packed_size;
    uint16_t    source_file;
    uint16_t    source_line;
    uint16_t    symbol;
    uint8_t     data;
} kkpm_byte;

static char *read_asciiz (FILE *file);
static float read_float (FILE *file);
static double read_double (FILE *file);
static uint8_t read_u8 (FILE *file);
static uint16_t read_u16 (FILE *file);
static uint32_t read_u32 (FILE *file);
static void write_asciiz (const char *v, FILE *file);
static void write_float (float v, FILE *file);
static void write_double (double v, FILE *file);
static void write_u8 (uint8_t v, FILE *file);
static void write_u16 (uint16_t v, FILE *file);
static void write_u32 (uint32_t v, FILE *file);

static int iw (uint16_t w) { return w==0xFFFF?-1:w; }

#if 0
static int matchsymname (const char *ln, const char *cn) {
    if(ln[0]=='_')++ln;
    int len=strlen(ln);
    int lcn=strlen(cn);
    if (len>lcn)len=lcn;
    return !memcmp(ln,cn,len);
}
#endif

int main (int argc, char **argv) {

    if(argc<2) {
      printf("Usage: %s <packer kkp> <debug kkp>...\n"
        "Options:\n"
        "  -o <file>     Output to <file>\n",
        argv[0]);
      return 1;
    }

    // merged sources info
    #define msrcsmax 0x10000
    static kkpm_source msrcs[msrcsmax];
    int nmsrcs=0;

    kkpm_source nullsrc;
    nullsrc.name="<no source>";
    nullsrc.packed_size=0;
    nullsrc.unpacked_size=0;
    msrcs[0]=nullsrc;
    ++nmsrcs;

    // main binary bytes
    uint32_t nmbytes=0;
    kkpm_byte *mbytes =0;

    // main binary symbols
    uint32_t nmsyms=0;
    kkpm_symbol *msyms =0;

    for(int ifile=0;ifile<argc-1;++ifile) {
      const char *kkpname =argv[ifile+1];
      fprintf(stderr,"%s\n",kkpname);
      FILE *f=fopen(kkpname,"rb");
      if(!f) {
        fprintf(stderr, "file error: %s\n", kkpname);
        return 1;
      }

      // header
      char magic[4];
      fread(magic,4,1,f);
      uint32_t nbytes=read_u32(f);
      uint32_t nsrcs=read_u32(f);
      if(feof(f)) goto errtrunc;
      if(memcmp(magic,"KK64",4)) goto errfmt;
      if(nsrcs>0xFFFF) goto errinval;

      uint16_t srcmap[0x10000];

      // source code descriptors
      for(int i=0;i<nsrcs;++i) {
        kkpm_source src;
        src.name=read_asciiz(f);
        src.packed_size=read_float(f);
        src.unpacked_size=read_u32(f);
        if(feof(f)) goto errtrunc;
        int imsrc;
        for(imsrc=0;imsrc<nmsrcs;++imsrc) {
          if(!strcmp(msrcs[imsrc].name,src.name)) break;
        }
        kkpm_source *msrc=&msrcs[imsrc];
        if(imsrc==nmsrcs) {
          if(imsrc==msrcsmax) {
            fprintf(stderr, "too many sources\n");
            return 1;
          }
          ++nmsrcs;
          msrc->name=src.name;
          // to be re-generated from merged bytes info
          msrc->packed_size=0;
          msrc->unpacked_size=0;
        }
        srcmap[i]=imsrc;
      }

      uint32_t nsyms=read_u32(f);
      if(feof(f)) goto errtrunc;
      if(nsyms>0xFFFF) goto errinval;

      kkpm_symbol *syms =malloc(sizeof(*msyms)*nsyms);
      kkpm_byte *bytes =malloc(sizeof(*bytes)*nbytes);

      // symbol data
      for(int i=0;i<nsyms;++i) {
        kkpm_symbol sym;
        sym.name=read_asciiz(f);
        sym.packed_size=read_double(f);
        sym.unpacked_size=read_u32(f);
        sym.is_code=read_u8(f);
        sym.source_file=read_u32(f);
        sym.position=read_u32(f);
        if(feof(f)) goto errtrunc;
        if((int)sym.source_file>0xFFFF) goto errinval;
        if(sym.position>=nbytes) {
          fprintf(stderr,"warning: symbol %s:\n  position %08X out of range\n",
          sym.name,sym.position);
        }
        sym.size=0;
        syms[i]=sym;
      }

      // binary data
      for(int i=0;i<nbytes;++i) {
        kkpm_byte byte;
        byte.data=read_u8(f);
        byte.symbol=read_u16(f);
        byte.packed_size=read_double(f);
        byte.source_line=read_u16(f);
        byte.source_file=read_u16(f);
        if(feof(f)) goto errtrunc;
        bytes[i]=byte;
        int isym=iw(byte.symbol);
        if(isym>=0) {
          // get symbol size
          kkpm_symbol *sym =&syms[isym];
          if(sym->position>=nbytes) continue;
          if(sym->position>i) goto errinval;
          sym->size=i+1-sym->position;
        }
      }

      // check each symbol against main file and merge info as appropriate
      if(ifile) for(int i=0;i<nsyms;++i) {
        kkpm_symbol sym=syms[i];
        for(int imsym=0;imsym<nmsyms;++imsym) {
          kkpm_symbol msym=msyms[imsym];
        #if 1 // strict symbol matching
          if(strcmp(msym.name,sym.name)) goto nextsym;
        #else
          if(!matchsymname(msym.name,sym.name)) goto nextsym;
          if(strcmp(msym.name,sym.name)) {
            fprintf(stderr,"warning: matching %s with %s\n",
              msym.name,sym.name);
          }
        #endif
          int size=msym.size<sym.size?msym.size:sym.size;
          for(int ib=0;ib<size;++ib) {
            kkpm_byte mbyte=mbytes[msym.position+ib];
            kkpm_byte byte=bytes[sym.position+ib];
            if(mbyte.symbol!=imsym||byte.symbol!=i) {
              // should never happen, but possible within KKP format.
              fprintf(stderr,"warning: ignoring discontiguous symbol %s\n",
                sym.name);
              goto nextsym;
            }
            // comparing contents is no good because of e.g. relocations,
            // and any other linker-performed transformations
            /* if(mbyte.data!=byte.data) {
              fprintf(stderr,"warning: ignoring same-named symbol %s:\n"
                "  bytes differ: +%08X %02X %02X\n",
                sym.name,ib,mbyte.data,byte.data);
              goto nextsym;
            } */
          }
          if(size!=msym.size) {
            nbytes=sym.size;
            // size warning 1
          }
          if(size!=sym.size) {
            // size warning 2
          }
          for(int ib=0;ib<size;++ib) {
            kkpm_byte byte=bytes[sym.position+ib];
            kkpm_byte *mbyte =&mbytes[msym.position+ib];
            int isrc=iw(byte.source_file);
            if(isrc<0) continue;
            mbyte->source_file=srcmap[isrc];
            mbyte->source_line=byte.source_line;
          }
          break;
          nextsym: {}
        }
      }

      if(!ifile) {
        // main file
        nmsyms=nsyms;
        msyms=syms;
        nmbytes=nbytes;
        mbytes=bytes;
      } else {
        free(syms);
        free(bytes);
      }

      fgetc(f);
      if(!feof(f))
        fprintf(stderr, "warning: junk at end of file %s\n", kkpname);

      fclose(f);
      continue;

    errinval:
      fprintf(stderr, "invalid kkp data: %s\n", kkpname);
      return 1;
    errfmt:
      fprintf(stderr, "malformed kkp format: %s\n", kkpname);
      return 1;
    errtrunc:
      fprintf(stderr, "truncated kkp format: %s\n", kkpname);
      return 1;
    }

    // regenerate source size info
    for(int i=0;i<nmbytes;i++) {
      kkpm_byte byte=mbytes[i];
      int imsrc=iw(byte.source_file);
      if(imsrc<0) imsrc=0;
      msrcs[imsrc].unpacked_size+=1;
      msrcs[imsrc].packed_size+=byte.packed_size;
    }

    FILE *f=fopen("merged.kkp","wb");
    fwrite("KK64",4,1,f);
    write_u32(nmbytes,f);
    write_u32(nmsrcs,f);
    for(int i=0;i<nmsrcs;i++) {
      kkpm_source src=msrcs[i];
      write_asciiz(src.name,f);
      write_float(src.packed_size,f);
      write_u32(src.unpacked_size,f);
    }
    write_u32(nmsyms,f);
    for(int i=0;i<nmsyms;i++) {
      kkpm_symbol sym=msyms[i];
      write_asciiz(sym.name,f);
      write_double(sym.packed_size,f);
      write_u32(sym.unpacked_size,f);
      write_u8(sym.is_code,f);
      write_u32(sym.source_file,f);
      write_u32(sym.position,f);
    }
    for(int i=0;i<nmbytes;i++) {
      kkpm_byte byte=mbytes[i];
      write_u8(byte.data,f);
      write_u16(byte.symbol,f);
      write_double(byte.packed_size,f);
      write_u16(byte.source_line,f);
      write_u16(byte.source_file,f);
    }
    fclose(f);
    return 0;
}

#define READERWRITER(N,T) \
static T read_##N (FILE *file) { \
    T v=0; \
    fread(&v,sizeof(T),1,file); \
    return v; \
} \
static void write_##N (T v, FILE *file) { \
    fwrite(&v,sizeof(T),1,file); \
}

static char *read_asciiz (FILE *file) {
    const int szmax=0x400;
    char *buf=malloc(szmax);
    for(int n=0;n<szmax;++n) {
      int c=fgetc(file);
      if(c<0) break;
      buf[n]=c;
      if(!c) {
        buf=realloc(buf,n+2);
        return buf;
      }
    }
    free(buf);
    return 0;
}

static void write_asciiz (const char *v, FILE *file) {
    fwrite(v,1,strlen(v)+1,file);
}

READERWRITER(float,float)
READERWRITER(double,double)
READERWRITER(u8,uint8_t)
READERWRITER(u16,uint16_t)
READERWRITER(u32,uint32_t)

