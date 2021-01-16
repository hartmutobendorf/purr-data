/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* this file contains, first, a collection of soundfile access routines, a
sort of soundfile library.  Second, the "soundfiler" object is defined which
uses the routines to read or write soundfiles, synchronously, from garrays.
These operations are not to be done in "real time" as they may have to wait
for disk accesses (even the write routine.)  Finally, the realtime objects
readsf~ and writesf~ are defined which confine disk operations to a separate
thread so that they can be used in real time.  The readsf~ and writesf~
objects use Posix-like threads. */

#include "config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifndef MSW
#include <fcntl.h>
#endif
#include <pthread.h>

#ifdef HAVE_IO_H
#include <io.h>
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#include "m_pd.h"

#define MAXSFCHANS 64

#ifdef _LARGEFILE64_SOURCE
# define open open64
# define lseek lseek64
# define off_t __off64_t
#endif
#ifdef MSW
# define off_t long
#endif

/***************** soundfile header structures ************************/

typedef unsigned short uint16;
typedef unsigned int uint32;

typedef union
{
    float f;
    uint32_t ui;
}t_aliasfloatuint;

#define FORMAT_WAVE 0
#define FORMAT_AIFF 1
#define FORMAT_NEXT 2

/* the NeXTStep sound header structure; can be big or little endian  */

typedef struct _nextstep
{
    char ns_fileid[4];      /* magic number '.snd' if file is big-endian */
    uint32 ns_onset;        /* byte offset of first sample */
    uint32 ns_length;       /* length of sound in bytes */
    uint32 ns_format;        /* format; see below */
    uint32 ns_sr;           /* sample rate */
    uint32 ns_nchans;       /* number of channels */
    char ns_info[4];        /* comment */
} t_nextstep;

#define NS_FORMAT_LINEAR_16     3
#define NS_FORMAT_LINEAR_24     4
#define NS_FORMAT_FLOAT         6
#define SCALE (1./(1024. * 1024. * 1024. * 2.))

/* the WAVE header.  All Wave files are little endian.  We assume
    the "fmt" chunk comes first which is usually the case but perhaps not
    always; same for AIFF and the "COMM" chunk.   */

typedef unsigned word;
typedef unsigned long dword;

typedef struct _wave
{
    char  w_fileid[4];              /* chunk id 'RIFF'            */
    uint32 w_chunksize;             /* chunk size                 */
    char  w_waveid[4];              /* wave chunk id 'WAVE'       */
    char  w_fmtid[4];               /* format chunk id 'fmt '     */
    uint32 w_fmtchunksize;          /* format chunk size          */
    uint16  w_fmttag;               /* format tag (WAV_INT etc)   */
    uint16  w_nchannels;            /* number of channels         */
    uint32 w_samplespersec;         /* sample rate in hz          */
    uint32 w_navgbytespersec;       /* average bytes per second   */
    uint16  w_nblockalign;          /* number of bytes per frame  */
    uint16  w_nbitspersample;       /* number of bits in a sample */
    char  w_datachunkid[4];         /* data chunk id 'data'       */
    uint32 w_datachunksize;         /* length of data chunk       */
} t_wave;

typedef struct _fmt         /* format chunk */
{
    uint16 f_fmttag;                /* format tag, 1 for PCM      */
    uint16 f_nchannels;             /* number of channels         */
    uint32 f_samplespersec;         /* sample rate in hz          */
    uint32 f_navgbytespersec;       /* average bytes per second   */
    uint16 f_nblockalign;           /* number of bytes per frame  */
    uint16 f_nbitspersample;        /* number of bits in a sample */
} t_fmt;

typedef struct _wavechunk           /* ... and the last two items */
{
    char  wc_id[4];                 /* data chunk id, e.g., 'data' or 'fmt ' */
    uint32 wc_size;                 /* length of data chunk       */
} t_wavechunk;

#define WAV_INT 1
#define WAV_FLOAT 3

/* the AIFF header.  I'm assuming AIFC is compatible but don't really know
    that. */

typedef struct _datachunk
{
    char  dc_id[4];                 /* data chunk id 'SSND'       */
    uint32 dc_size;                 /* length of data chunk       */
    uint32 dc_offset;               /* additional offset in bytes */
    uint32 dc_block;                /* block size                 */
} t_datachunk;

typedef struct _comm
{
    uint16 c_nchannels;             /* number of channels         */
    uint16 c_nframeshi;             /* # of sample frames (hi)    */
    uint16 c_nframeslo;             /* # of sample frames (lo)    */
    uint16 c_bitspersamp;           /* bits per sample            */
    unsigned char c_samprate[10];   /* sample rate, 80-bit float! */
} t_comm;

    /* this version is more convenient for writing them out: */
typedef struct _aiff
{
    char  a_fileid[4];              /* chunk id 'FORM'            */
    uint32 a_chunksize;             /* chunk size                 */
    char  a_aiffid[4];              /* aiff chunk id 'AIFF'       */
    char  a_fmtid[4];               /* format chunk id 'COMM'     */
    uint32 a_fmtchunksize;          /* format chunk size, 18      */
    uint16 a_nchannels;             /* number of channels         */
    uint16 a_nframeshi;             /* # of sample frames (hi)    */
    uint16 a_nframeslo;             /* # of sample frames (lo)    */
    uint16 a_bitspersamp;           /* bits per sample            */
    unsigned char a_samprate[10];   /* sample rate, 80-bit float! */
} t_aiff;

#define AIFFHDRSIZE 38      /* probably not what sizeof() gives */


#define AIFFPLUS (AIFFHDRSIZE + 16)  /* header size including SSND chunk hdr */

#define WHDR1 sizeof(t_nextstep)
#define WHDR2 (sizeof(t_wave) > WHDR1 ? sizeof (t_wave) : WHDR1)
#define WRITEHDRSIZE (AIFFPLUS > WHDR2 ? AIFFPLUS : WHDR2)

#define READHDRSIZE (16 > WHDR2 + 2 ? 16 : WHDR2 + 2)

#define OBUFSIZE MAXPDSTRING  /* assume MAXPDSTRING is bigger than headers */

#ifdef MSW
#include <fcntl.h>
#define BINCREATE _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY
#else
#define BINCREATE O_WRONLY | O_CREAT | O_TRUNC
#endif

/* this routine returns 1 if the high order byte comes at the lower
address on our architecture (big-endianness.).  It's 1 for Motorola,
0 for Intel: */

extern int garray_ambigendian(void);

/* byte swappers */

static uint32 swap4(uint32 n, int doit)
{
    if (doit)
        return (((n & 0xff) << 24) | ((n & 0xff00) << 8) |
            ((n & 0xff0000) >> 8) | ((n & 0xff000000) >> 24));
    else return (n);
}

static uint16 swap2(uint32 n, int doit)
{
    if (doit)
        return (((n & 0xff) << 8) | ((n & 0xff00) >> 8));
    else return (n);
}

static void swapstring(char *foo, int doit)
{
    if (doit)
    {
        char a = foo[0], b = foo[1], c = foo[2], d = foo[3];
        foo[0] = d; foo[1] = c; foo[2] = b; foo[3] = a;
    }
}

    /* read sample rate from an 80-bit AIFF-compatible number */
static double readaiffsamprate(char *shit, int swap)
{
    unsigned long mantissa, last = 0;
    unsigned char exp;

    swapstring(shit+2, swap);

    mantissa = (unsigned long) *((unsigned long *)(shit+2));
    exp = 30 - *(shit+1);
    while (exp--)
    {
        last = mantissa;
        mantissa >>= 1;
    }
    if (last & 0x00000001) mantissa++;
        return mantissa;
}

    /* write a sample rate as an 80-bit AIFF-compatible number */
static void makeaiffsamprate(double sr, unsigned char *shit)
{
    int exponent;
    double mantissa = frexp(sr, &exponent);
    unsigned long fixmantissa = ldexp(mantissa, 32);
    shit[0] = (exponent+16382)>>8;
    shit[1] = exponent+16382;
    shit[2] = fixmantissa >> 24;
    shit[3] = fixmantissa >> 16;
    shit[4] = fixmantissa >> 8;
    shit[5] = fixmantissa;
    shit[6] = shit[7] = shit[8] = shit[9] = 0;
}

/******************** soundfile access routines **********************/

typedef struct _soundfile_info
{
    int samplerate;
    int channels;
    int bytespersample;
    int headersize;
    int bigendian;
    long bytelimit;
} t_soundfile_info;

static void outlet_soundfile_info(t_outlet *out, t_soundfile_info *info)
{
    t_atom info_list[5];
    SETFLOAT((t_atom *)info_list, (t_float)info->samplerate);
    SETFLOAT((t_atom *)info_list+1,
        (t_float)(info->headersize < 0 ? 0 : info->headersize));
    SETFLOAT((t_atom *)info_list+2, (t_float)info->channels);
    SETFLOAT((t_atom *)info_list+3, (t_float)info->bytespersample);
    SETSYMBOL((t_atom *)info_list+4, gensym((info->bigendian ? "b" : "l")));
    outlet_list(out, &s_list, 5, (t_atom *)info_list);
}

/* This routine opens a file, looks for either a nextstep or "wave" header,
* seeks to end of it, and fills in bytes per sample and number of channels.
* Only 2- and 3-byte fixed-point samples and 4-byte floating point samples
* are supported.  If "headersize" is nonzero, the
* caller should supply the number of channels, endinanness, and bytes per
* sample; the header is ignored.  Otherwise, the routine tries to read the
* header and fill in the properties.
*/

int open_soundfile_via_fd(int fd, t_soundfile_info *p_info, long skipframes)
{
    int nchannels, bigendian, bytespersamp, samprate, swap;
    int headersize = 0;
    long sysrtn, bytelimit = 0x7fffffff;
    errno = 0;
    if (p_info->headersize >= 0) /* header detection overridden */
    {
        headersize = p_info->headersize;
        bigendian = p_info->bigendian;
        nchannels = p_info->channels;
        bytespersamp = p_info->bytespersample;
        bytelimit = p_info->samplerate;
        samprate = p_info->samplerate;
    }
    else
    {
        char buf[OBUFSIZE];
        int bytesread = read(fd, buf, READHDRSIZE);
        int format;
        if (bytesread < 4)
            goto badheader;
        if (!strncmp(buf, ".snd", 4))
            format = FORMAT_NEXT, bigendian = 1;
        else if (!strncmp(buf, "dns.", 4))
            format = FORMAT_NEXT, bigendian = 0;
        else if (!strncmp(buf, "RIFF", 4))
        {
            if (bytesread < 12 || strncmp(buf + 8, "WAVE", 4))
                goto badheader;
            format = FORMAT_WAVE, bigendian = 0;
        }
        else if (!strncmp(buf, "FORM", 4))
        {
            if (bytesread < 12 || strncmp(buf + 8, "AIFF", 4))
                goto badheader;
            format = FORMAT_AIFF, bigendian = 1;
        }
        else
            goto badheader;
        swap = (bigendian != garray_ambigendian());
        if (format == FORMAT_NEXT)   /* nextstep header */
        {
            if (bytesread < (int)sizeof(t_nextstep))
                goto badheader;
            nchannels = swap4(((t_nextstep *)buf)->ns_nchans, swap);
            format = swap4(((t_nextstep *)buf)->ns_format, swap);
            headersize = swap4(((t_nextstep *)buf)->ns_onset, swap);
            if (format == NS_FORMAT_LINEAR_16)
                bytespersamp = 2;
            else if (format == NS_FORMAT_LINEAR_24)
                bytespersamp = 3;
            else if (format == NS_FORMAT_FLOAT)
                bytespersamp = 4;
            else goto badheader;
            bytelimit = 0x7fffffff;
            samprate = swap4(((t_nextstep *)buf)->ns_sr, swap);
        }
        else if (format == FORMAT_WAVE)     /* wave header */
        {
               /*  This is awful.  You have to skip over chunks,
               except that if one happens to be a "fmt" chunk, you want to
               find out the format from that one.  The case where the
               "fmt" chunk comes after the audio isn't handled. */
            headersize = 12;
            if (bytesread < 20)
                goto badheader;
                /* First we guess a number of channels, etc., in case there's
                no "fmt" chunk to follow. */
            nchannels = 1;
            bytespersamp = 2;
            samprate = 44100;
                /* copy the first chunk header to beginnning of buffer. */
            memcpy(buf, buf + headersize, sizeof(t_wavechunk));
            /* post("chunk %c %c %c %c",
                    ((t_wavechunk *)buf)->wc_id[0],
                    ((t_wavechunk *)buf)->wc_id[1],
                    ((t_wavechunk *)buf)->wc_id[2],
                    ((t_wavechunk *)buf)->wc_id[3]); */
                /* read chunks in loop until we get to the data chunk */
            while (strncmp(((t_wavechunk *)buf)->wc_id, "data", 4))
            {
                long chunksize = swap4(((t_wavechunk *)buf)->wc_size,
                    swap), seekto = headersize + chunksize + 8, seekout;
                if (seekto & 1)     /* pad up to even number of bytes */
                    seekto++;
                if (!strncmp(((t_wavechunk *)buf)->wc_id, "fmt ", 4))
                {
                    long commblockonset = headersize + 8;
                    seekout = lseek(fd, commblockonset, SEEK_SET);
                    if (seekout != commblockonset)
                        goto badheader;
                    if (read(fd, buf, sizeof(t_fmt)) < (int) sizeof(t_fmt))
                            goto badheader;
                    nchannels = swap2(((t_fmt *)buf)->f_nchannels, swap);
                    format = swap2(((t_fmt *)buf)->f_nbitspersample, swap);
                    if (format == 16)
                        bytespersamp = 2;
                    else if (format == 24)
                        bytespersamp = 3;
                    else if (format == 32)
                        bytespersamp = 4;
                    else goto badheader;
                    samprate = swap4(
                        ((t_fmt *)buf)->f_samplespersec, swap);
                }
                seekout = (long)lseek(fd, seekto, SEEK_SET);
                if (seekout != seekto)
                    goto badheader;
                if (read(fd, buf, sizeof(t_wavechunk)) <
                    (int) sizeof(t_wavechunk))
                        goto badheader;
                /* post("new chunk %c %c %c %c at %d",
                    ((t_wavechunk *)buf)->wc_id[0],
                    ((t_wavechunk *)buf)->wc_id[1],
                    ((t_wavechunk *)buf)->wc_id[2],
                    ((t_wavechunk *)buf)->wc_id[3], seekto); */
                headersize = (int)seekto;
            }
            bytelimit = swap4(((t_wavechunk *)buf)->wc_size, swap);
            headersize += 8;
        }
        else
        {
                /* AIFF.  same as WAVE; actually predates it.  Disgusting. */
            headersize = 12;
            if (bytesread < 20)
                goto badheader;
                /* First we guess a number of channels, etc., in case there's
                no COMM block to follow. */
            nchannels = 1;
            bytespersamp = 2;
            samprate = 44100;
                /* copy the first chunk header to beginnning of buffer. */
            memcpy(buf, buf + headersize, sizeof(t_datachunk));
                /* read chunks in loop until we get to the data chunk */
            while (strncmp(((t_datachunk *)buf)->dc_id, "SSND", 4))
            {
                long chunksize = swap4(((t_datachunk *)buf)->dc_size,
                    swap), seekto = headersize + chunksize + 8, seekout;
                if (seekto & 1)     /* pad up to even number of bytes */
                    seekto++;
                /* post("chunk %c %c %c %c seek %d",
                    ((t_datachunk *)buf)->dc_id[0],
                    ((t_datachunk *)buf)->dc_id[1],
                    ((t_datachunk *)buf)->dc_id[2],
                    ((t_datachunk *)buf)->dc_id[3], seekto); */
                if (!strncmp(((t_datachunk *)buf)->dc_id, "COMM", 4))
                {
                    long commblockonset = headersize + 8;
                    seekout = lseek(fd, commblockonset, SEEK_SET);
                    if (seekout != commblockonset)
                        goto badheader;
                    if (read(fd, buf, sizeof(t_comm)) <
                        (int) sizeof(t_comm))
                            goto badheader;
                    nchannels = swap2(((t_comm *)buf)->c_nchannels, swap);
                    format = swap2(((t_comm *)buf)->c_bitspersamp, swap);
                    if (format == 16)
                        bytespersamp = 2;
                    else if (format == 24)
                        bytespersamp = 3;
                    else goto badheader;
                    samprate = readaiffsamprate(
                        (char *)((t_comm *)buf)->c_samprate, swap);
                }
                seekout = (long)lseek(fd, seekto, SEEK_SET);
                if (seekout != seekto)
                    goto badheader;
                if (read(fd, buf, sizeof(t_datachunk)) <
                    (int) sizeof(t_datachunk))
                        goto badheader;
                headersize = (int)seekto;
            }
            bytelimit = swap4(((t_datachunk *)buf)->dc_size, swap) - 8;
            headersize += sizeof(t_datachunk);
        }
    }
        /* seek past header and any sample frames to skip */
    sysrtn = (long)lseek(fd,
        ((off_t)nchannels) * bytespersamp * skipframes + headersize, 0);
    if (sysrtn != nchannels * bytespersamp * skipframes + headersize)
        return (-1);
     bytelimit -= nchannels * bytespersamp * skipframes;
     if (bytelimit < 0)
        bytelimit = 0;
        /* copy sample format back to caller */
    p_info->headersize = headersize;
    p_info->bigendian = bigendian;
    p_info->channels = nchannels;
    p_info->bytespersample = bytespersamp;
    p_info->bytelimit = bytelimit;
    p_info->samplerate = samprate;
    return (fd);
badheader:
        /* the header wasn't recognized.  We're threadable here so let's not
        print out the error... */
    errno = EIO;
    return (-1);
}

    /* open a soundfile, using open_via_path().  This is used by readsf~ in
    a not-perfectly-threadsafe way.  LATER replace with a thread-hardened
    version of open_soundfile_via_canvas() */
int open_soundfile(const char *dirname, const char *filename,
    t_soundfile_info *p_info, long skipframes)
{
    char buf[FILENAME_MAX], *bufptr;
    int fd, sf_fd;
    fd = open_via_path(dirname, filename, "", buf, &bufptr, FILENAME_MAX, 1);
    if (fd < 0)
        return (-1);
    sf_fd = open_soundfile_via_fd(fd, p_info, skipframes);
    if (sf_fd < 0)
        sys_close(fd);
    return (sf_fd);
}

    /* open a soundfile, using open_via_canvas().  This is used by readsf~ in
    a not-perfectly-threadsafe way.  LATER replace with a thread-hardened
    version of open_soundfile_via_canvas() */
int open_soundfile_via_canvas(t_canvas *canvas, const char *filename,
    t_soundfile_info *p_info, long skipframes)
{
    char buf[FILENAME_MAX], *bufptr;
    int fd, sf_fd;
    fd = canvas_open(canvas, filename, "", buf, &bufptr, FILENAME_MAX, 1);
    if (fd < 0)
        return (-1);
    sf_fd = open_soundfile_via_fd(fd, p_info, skipframes);
    if (sf_fd < 0)
        sys_close(fd);
    return (sf_fd);
}

static void soundfile_xferin_sample(int sfchannels, int nvecs, t_sample **vecs,
    long itemsread, unsigned char *buf, int nitems, int bytespersamp,
    int bigendian)
{
    int i, j;
    unsigned char *sp, *sp2;
    t_sample *fp;
    t_aliasfloatuint alias;
    int nchannels = (sfchannels < nvecs ? sfchannels : nvecs);
    int bytesperframe = bytespersamp * sfchannels;
    for (i = 0, sp = buf; i < nchannels; i++, sp += bytespersamp)
    {
        if (bytespersamp == 2)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp++)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp ++)
                        *fp = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
            }
        }
        else if (bytespersamp == 3)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp++)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16)
                            | (sp2[2] << 8));
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp ++)
                        *fp = SCALE * ((sp2[2] << 24) | (sp2[1] << 16)
                            | (sp2[0] << 8));
            }
        }
        else if (bytespersamp == 4)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                     j < nitems; j++, sp2 += bytesperframe, fp++)
                    {
                        alias.ui = ((sp2[0] << 24) | (sp2[1] << 16)
                            | (sp2[2] << 8) | sp2[3]);
                        *fp = (t_float)alias.f;
                    }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + itemsread;
                     j < nitems; j++, sp2 += bytesperframe, fp++)
                    {
                        alias.ui = ((sp2[3] << 24) | (sp2[2] << 16)
                            | (sp2[1] << 8) | sp2[0]);
                        *fp = (t_float)alias.f;
                    }
            }
        }
    }
        /* zero out other outputs */
    for (i = sfchannels; i < nvecs; i++)
        for (j = nitems, fp = vecs[i]; j--; )
            *fp++ = 0;

}

static void soundfile_xferin_float(int sfchannels, int nvecs, t_float **vecs,
    long itemsread, unsigned char *buf, int nitems, int bytespersamp,
    int bigendian, int spread)
{
    int i, j;
    unsigned char *sp, *sp2;
    t_float *fp;
    t_aliasfloatuint alias;
    int nchannels = (sfchannels < nvecs ? sfchannels : nvecs);
    int bytesperframe = bytespersamp * sfchannels;
    for (i = 0, sp = buf; i < nchannels; i++, sp += bytespersamp)
    {
        if (bytespersamp == 2)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                        *fp = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
            }
        }
        else if (bytespersamp == 3)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16)
                            | (sp2[2] << 8));
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                        *fp = SCALE * ((sp2[2] << 24) | (sp2[1] << 16)
                            | (sp2[0] << 8));
            }
        }
        else if (bytespersamp == 4)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                     j < nitems; j++, sp2 += bytesperframe, fp += spread)
                    {
                        alias.ui = ((sp2[0] << 24) | (sp2[1] << 16)
                            | (sp2[2] << 8) | sp2[3]);
                        *fp = (t_float)alias.f;
                    }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + spread * itemsread;
                     j < nitems; j++, sp2 += bytesperframe, fp += spread)
                    {
                        alias.ui = ((sp2[3] << 24) | (sp2[2] << 16)
                            | (sp2[1] << 8) | sp2[0]);
                        *fp = (t_float)alias.f;
                    }
            }
        }
    }
        /* zero out other outputs */
    for (i = sfchannels; i < nvecs; i++)
        for (j = nitems, fp = vecs[i]; j--; )
            *fp++ = 0;

}

    /* soundfiler_write ...
 
    usage: write [flags] filename table ...
    flags:
        -nframes <frames>
        -skip <frames>
        -bytes <bytes per sample>
        -normalize
        -nextstep
        -wave
        -big
        -little
    */


static void argerror(void *obj, t_symbol *s, int argc, t_atom *argv,
    const char *fmt, ...)
{
    char error_str[MAXPDSTRING];
    char *user_msg; /* message from user that triggered the error */
    int len;
    char *classname = class_getname(pd_class((t_pd *)obj));
    va_list ap;
    t_binbuf *b = binbuf_new();
    va_start(ap, fmt);
    vsnprintf(error_str, MAXPDSTRING-1, fmt, ap);
    va_end(ap);

    binbuf_addv(b, "s", s);
    binbuf_add(b, argc, argv);
    binbuf_gettext(b, &user_msg, &len); /* not null-terminated! */
    
    pd_error(obj, "%s: '%.*s': %s",
        classname,
        len < MAXPDSTRING ? len : MAXPDSTRING,
        user_msg,
        error_str);
}

static int flag_missing_floatarg(void *obj, t_symbol *s, int argc,
    t_atom *argv, char *flag, int flagc, t_atom *flagv)
{
        /* First check if our flag has an arg at all. If not, error out.
           (flagc includes the flag itself.) */
    if (flagc < 2)
    {
        argerror(obj, s, argc, argv,
            "'%s' flag expects a float argument", flag);
        return 1;
    }
    if (flagv[1].a_type != A_FLOAT)
    {
        argerror(obj, s, argc, argv, "'%s' flag expects a float but got '%s'",
           flag,
           flagv[1].a_type == A_SYMBOL ?
           flagv[1].a_w.w_symbol->s_name :
           "unexpected arg type");
        return 1;
    }
    return 0;
}

static int flag_has_unexpected_floatarg(void *obj, t_symbol *s,
    int argc, t_atom *argv, char *flag, int flagc, t_atom *flagv)
{
    if (flagc < 2) return 0;
    if (flagv[1].a_type == A_FLOAT)
    {
        argerror(obj, s, argc, argv,
            "'%s' flag does not accept a float argument", flag);
        return 1;
    }
    return 0;
}

    /* catch filenames that are flags, with and without pre-fixed '-' */
static int file_is_a_flag_name(t_symbol *sym, int *nodash)
{
    char *s = sym->s_name;
    if (s[0] == '-')
    {
        s++;
        *nodash = 1;
    }
    else
        *nodash = 0;

    if (!strcmp(s, "skip")
        || !strcmp(s, "nframes")
        || !strcmp(s, "bytes")
        || !strcmp(s, "normalize")
        || !strcmp(s, "wave")
        || !strcmp(s, "nextstep")
        || !strcmp(s, "aiff")
        || !strcmp(s, "big")
        || !strcmp(s, "little")
        || !strcmp(s, "r")
        || !strcmp(s, "rate"))
    {
        return 1;
    }
    else
        return 0;
}

    /* the routine which actually does the work should LATER also be called
    from garray_write16. */

    /* Parse arguments for writing.  The "obj" argument is only for flagging
    errors.  For streaming to a file the "normalize", "skip" and "nframes"
    arguments shouldn't be set but the calling routine flags this. */

    /* Also note that streaming objects like writesf~ don't take args after
       the filename, while soundfiler does to specify the source tables */
static int soundfiler_writeargparse(void *obj, t_symbol *s,
    int *p_argc, t_atom **p_argv,
    t_symbol **p_filesym,
    int *p_filetype, int *p_bytespersamp, int *p_swap, int *p_bigendian,
    int *p_normalize, long *p_onset, long *p_nframes, t_float *p_rate)
{
    /* copies for convenience, and for the ruthless mutation below. :) */
    int argc = *p_argc;
    int ac = argc;
    t_atom *argv = *p_argv;
    t_atom *av = argv;
    int bytespersamp = 2, bigendian = 0,
        endianness = -1, swap, filetype = -1, normalize = 0;
    long onset = 0, nframes = 0x7fffffff;
    t_symbol *filesym;
    t_float rate = -1;
    
    while (ac > 0 && atom_getsymbol(av)->s_name[0] == '-')
    {
        char *flag = av->a_w.w_symbol->s_name;
        if (!strcmp(flag, "-skip"))
        {
            if (flag_missing_floatarg(obj, s, argc, argv, flag, ac, av))
                goto usage;
            if ((onset = av[1].a_w.w_float) < 0)
            {
                argerror(obj, s, argc, argv,
                    "'-skip' flag does not allow a negative number");
                goto usage;
            }
            ac -= 2; av += 2;
        }
        else if (!strcmp(flag, "-nframes"))
        {
            if (flag_missing_floatarg(obj, s, argc, argv, flag, ac, av))
                goto usage;
            if ((nframes = av[1].a_w.w_float) < 0)
            {
                argerror(obj, s, argc, argv,
                    "'-nframes' flag does not allow a negative number");
                goto usage;
            }
            ac -= 2; av += 2;
        }
        else if (!strcmp(flag, "-bytes"))
        {
            if (flag_missing_floatarg(obj, s, argc, argv, flag, ac, av))
                goto usage;
            if ((bytespersamp = av[1].a_w.w_float) < 2 ||
                   bytespersamp > 4)
            {
                argerror(obj, s, argc, argv,
                    "'-bytes' flag requires a number between 2 and 4");
                goto usage;
            }
            ac -= 2; av += 2;
        }
        else if (!strcmp(flag, "-normalize"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            normalize = 1;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-wave"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            filetype = FORMAT_WAVE;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-nextstep"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            filetype = FORMAT_NEXT;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-aiff"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            filetype = FORMAT_AIFF;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-big"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            endianness = 1;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-little"))
        {
            if (flag_has_unexpected_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            endianness = 0;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-r") || !strcmp(flag, "-rate"))
        {
            if (flag_missing_floatarg(obj, s, argc, argv,
                flag, ac, av))
            {
                goto usage;
            }
            if ((rate = av[1].a_w.w_float) <= 0)
            {
                argerror(obj, s, argc, argv,
                    "'%s' flag must have a float arg greater than zero", flag);
                goto usage;
            }
            ac -= 2; av += 2;
        }
        else
        {
            argerror(obj, s, argc, argv, "unknown flag '%s'", flag);
            goto usage;
        }
    }
    if (ac < 1)
    {
            /* a bit tricky-- writesf~ "open" method doesn't need table args */
        argerror(obj, s, argc, argv, "%s",
            s == gensym("open") ? "need a filename" :
            "need a filename and table argument(s)");
        goto usage;
    }
        /* Now that we know we have at least one arg, let's make sure it's
           a symbol. */
    if (av->a_type != A_SYMBOL)
    {
        argerror(obj, s, argc, argv, "filename must be a symbol");
        goto usage;
    }
    filesym = av->a_w.w_symbol;
        /* check if filesym is a flag name, and warn if so. */
    int nodash;
    if (file_is_a_flag_name(filesym, &nodash))
    {
        post("warning: filename '%s' looks like a flag%s",
            filesym->s_name, nodash ? " name" : "");
    }
        /* check if format not specified and fill in */
    if (filetype < 0) 
    {
        if (strlen(filesym->s_name) >= 5 &&
             (!strcmp(filesym->s_name + strlen(filesym->s_name) - 4, ".aif") ||
              !strcmp(filesym->s_name + strlen(filesym->s_name) - 4, ".AIF")))
        {
            filetype = FORMAT_AIFF;
        }
        if (strlen(filesym->s_name) >= 6 &&
             (!strcmp(filesym->s_name + strlen(filesym->s_name) - 5, ".aiff") ||
              !strcmp(filesym->s_name + strlen(filesym->s_name) - 5, ".AIFF")))
        {
            filetype = FORMAT_AIFF;
        }
        if (strlen(filesym->s_name) >= 5 &&
             (!strcmp(filesym->s_name + strlen(filesym->s_name) - 4, ".snd") ||
              !strcmp(filesym->s_name + strlen(filesym->s_name) - 4, ".SND")))
        {
            filetype = FORMAT_NEXT;
        }
        if (strlen(filesym->s_name) >= 4 &&
             (!strcmp(filesym->s_name + strlen(filesym->s_name) - 3, ".au") ||
              !strcmp(filesym->s_name + strlen(filesym->s_name) - 3, ".AU")))
        {
            filetype = FORMAT_NEXT;
        }
        if (filetype < 0)
            filetype = FORMAT_WAVE;
    }
        /* don't handle AIFF floating point samples */
    if (bytespersamp == 4)
    {
        if (filetype == FORMAT_AIFF)
        {
            argerror(obj, s, argc, argv,
                "AIFF floating-point file format unavailable");
            goto usage;
        }
    }
        /* for WAVE force little endian; for nextstep use machine native */
    if (filetype == FORMAT_WAVE)
    {
        bigendian = 0;
        if (endianness == 1)
            pd_error(obj, "WAVE file forced to little endian");
    }
    else if (filetype == FORMAT_AIFF)
    {
        bigendian = 1;
        if (endianness == 0)
            pd_error(obj, "AIFF file forced to big endian");
    }
    else if (endianness == -1)
    {
        bigendian = garray_ambigendian();
    }
    else bigendian = endianness;
    swap = (bigendian != garray_ambigendian());
    
    ac--; av++;
    
    *p_argc = ac;
    *p_argv = av;
    *p_filesym = filesym;
    *p_filetype = filetype;
    *p_bytespersamp = bytespersamp;
    *p_swap = swap;
    *p_normalize = normalize;
    *p_onset = onset;
    *p_nframes = nframes;
    *p_bigendian = bigendian;
    *p_rate = rate;
    return (0);
usage:
    return (-1);
}

    /* p_headersize is a getter, set to NULL if not needed */
static int create_soundfile(t_canvas *canvas, const char *filename,
    int filetype, long nframes, int bytespersamp,
    int bigendian, int nchannels, int swap, t_float samplerate,
    int *p_headersize)
{
    char filenamebuf[FILENAME_MAX], buf2[FILENAME_MAX];
    char headerbuf[WRITEHDRSIZE];
    t_wave *wavehdr = (t_wave *)headerbuf;
    t_nextstep *nexthdr = (t_nextstep *)headerbuf;
    t_aiff *aiffhdr = (t_aiff *)headerbuf;
    int fd, headersize = 0;
    
    strncpy(filenamebuf, filename, FILENAME_MAX-10);
    filenamebuf[FILENAME_MAX-10] = 0;

    if (filetype == FORMAT_NEXT)
    {
        if (strcmp(filenamebuf + strlen(filenamebuf)-4, ".snd"))
            strcat(filenamebuf, ".snd");
        if (bigendian)
            memcpy(nexthdr->ns_fileid, ".snd", 4);
        else memcpy(nexthdr->ns_fileid, "dns.", 4);
        nexthdr->ns_onset = swap4(sizeof(*nexthdr), swap);
        nexthdr->ns_length = 0;
        nexthdr->ns_format = swap4((bytespersamp == 3 ? NS_FORMAT_LINEAR_24 :
           (bytespersamp == 4 ? NS_FORMAT_FLOAT : NS_FORMAT_LINEAR_16)), swap);
        nexthdr->ns_sr = swap4(samplerate, swap);
        nexthdr->ns_nchans = swap4(nchannels, swap);
        strcpy(nexthdr->ns_info, "Pd ");
        swapstring(nexthdr->ns_info, swap);
        headersize = sizeof(t_nextstep);
    }
    else if (filetype == FORMAT_AIFF)
    {
        long datasize = nframes * nchannels * bytespersamp;
        long longtmp;
        if (strcmp(filenamebuf + strlen(filenamebuf)-4, ".aif") &&
            strcmp(filenamebuf + strlen(filenamebuf)-5, ".aiff"))
                strcat(filenamebuf, ".aif");
        memcpy(aiffhdr->a_fileid, "FORM", 4);
        aiffhdr->a_chunksize = swap4((uint32_t)(datasize +
            sizeof(*aiffhdr) + 4), swap);
        memcpy(aiffhdr->a_aiffid, "AIFF", 4);
        memcpy(aiffhdr->a_fmtid, "COMM", 4);
        aiffhdr->a_fmtchunksize = swap4(18, swap);
        aiffhdr->a_nchannels = swap2(nchannels, swap);
        longtmp = swap4((uint32_t)nframes, swap);
        memcpy(&aiffhdr->a_nframeshi, &longtmp, 4);
        aiffhdr->a_bitspersamp = swap2(8 * bytespersamp, swap);
        makeaiffsamprate(samplerate, aiffhdr->a_samprate);
        memcpy(((char *)(&aiffhdr->a_samprate))+10, "SSND", 4);
        longtmp = swap4((uint32_t)(datasize + 8), swap);
        memcpy(((char *)(&aiffhdr->a_samprate))+14, &longtmp, 4);
        memset(((char *)(&aiffhdr->a_samprate))+18, 0, 8);
        headersize = AIFFPLUS;
    }
    else    /* WAVE format */
    {
        long datasize = nframes * nchannels * bytespersamp;
        if (strcmp(filenamebuf + strlen(filenamebuf)-4, ".wav"))
            strcat(filenamebuf, ".wav");
        memcpy(wavehdr->w_fileid, "RIFF", 4);
        wavehdr->w_chunksize = swap4((uint32_t)(datasize +
            sizeof(*wavehdr) - 8), swap);
        memcpy(wavehdr->w_waveid, "WAVE", 4);
        memcpy(wavehdr->w_fmtid, "fmt ", 4);
        wavehdr->w_fmtchunksize = swap4(16, swap);
        wavehdr->w_fmttag =
            swap2((bytespersamp == 4 ? WAV_FLOAT : WAV_INT), swap);
        wavehdr->w_nchannels = swap2(nchannels, swap);
        wavehdr->w_samplespersec = swap4(samplerate, swap);
        wavehdr->w_navgbytespersec =
            swap4((int)(samplerate * nchannels * bytespersamp), swap);
        wavehdr->w_nblockalign = swap2(nchannels * bytespersamp, swap);
        wavehdr->w_nbitspersample = swap2(8 * bytespersamp, swap);
        memcpy(wavehdr->w_datachunkid, "data", 4);
        wavehdr->w_datachunksize = swap4((uint32_t)datasize, swap);
        headersize = sizeof(t_wave);
    }
    canvas_makefilename(canvas, filenamebuf, buf2, FILENAME_MAX);
    sys_bashfilename(buf2, buf2);
    if ((fd = sys_open(buf2, BINCREATE, 0666)) < 0)
        return (-1);

    if (write(fd, headerbuf, headersize) < headersize)
    {
        sys_close (fd);
        return (-1);
    }
    if (p_headersize)
        *p_headersize = headersize;
    return (fd);
}

static void soundfile_finishwrite(void *obj, char *filename, int fd,
    int filetype, long nframes, long itemswritten, int bytesperframe, int swap)
{
    if (itemswritten < nframes) 
    {
        if (nframes < 0x7fffffff)
            pd_error(obj, "soundfiler_write: %ld out of %ld bytes written",
                itemswritten, nframes);
            /* try to fix size fields in header */
        if (filetype == FORMAT_WAVE)
        {
            long datasize = itemswritten * bytesperframe, mofo;
            
            if (lseek(fd,
                ((char *)(&((t_wave *)0)->w_chunksize)) - (char *)0,
                    SEEK_SET) == 0)
                        goto baddonewrite;
            mofo = swap4((uint32_t)(datasize + sizeof(t_wave) - 8), swap);
            if (write(fd, (char *)(&mofo), 4) < 4)
                goto baddonewrite;
            if (lseek(fd,
                ((char *)(&((t_wave *)0)->w_datachunksize)) - (char *)0,
                    SEEK_SET) == 0)
                        goto baddonewrite;
            mofo = swap4((uint32_t)datasize, swap);
            if (write(fd, (char *)(&mofo), 4) < 4)
                goto baddonewrite;
        }
        if (filetype == FORMAT_AIFF)
        {
            long mofo;
            if (lseek(fd,
                ((char *)(&((t_aiff *)0)->a_nframeshi)) - (char *)0,
                    SEEK_SET) == 0)
                        goto baddonewrite;
            mofo = swap4((uint32_t)itemswritten, swap);
            if (write(fd, (char *)(&mofo), 4) < 4)
                goto baddonewrite;
            if (lseek(fd,
                ((char *)(&((t_aiff *)0)->a_chunksize)) - (char *)0,
                    SEEK_SET) == 0)
                        goto baddonewrite;
            mofo = swap4((uint32_t)itemswritten*bytesperframe+AIFFHDRSIZE, swap);
            if (write(fd, (char *)(&mofo), 4) < 4)
                goto baddonewrite;
            if (lseek(fd, (AIFFHDRSIZE+4), SEEK_SET) == 0)
                goto baddonewrite;
            mofo = swap4((uint32_t)itemswritten*bytesperframe, swap);
            if (write(fd, (char *)(&mofo), 4) < 4)
                goto baddonewrite;
        }
        if (filetype == FORMAT_NEXT)
        {
            /* do it the lazy way: just set the size field to 'unknown size'*/
            uint32_t nextsize = 0xffffffff;
            if (lseek(fd, 8, SEEK_SET) == 0)
            {
                goto baddonewrite;
            }
            if (write(fd, &nextsize, 4) < 4)
            {
                goto baddonewrite;
            }
        }
    }
    return;
baddonewrite:
    post("%s: %s", filename, strerror(errno));
}

static void soundfile_xferout_sample(int nchannels, t_sample **vecs,
    unsigned char *buf, int nitems, long onset, int bytespersamp,
    int bigendian, t_sample normalfactor, int spread)
{
    int i, j;
    unsigned char *sp, *sp2;
    t_sample *fp;
    t_aliasfloatuint alias;
    int bytesperframe = bytespersamp * nchannels;
    long xx;
    for (i = 0, sp = buf; i < nchannels; i++, sp += bytespersamp)
    {
        if (bytespersamp == 2)
        {
            t_sample ff = normalfactor * 32768.;
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[0] = (xx >> 8);
                    sp2[1] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (bytespersamp == 3)
        {
            t_sample ff = normalfactor * 8388608.;
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[0] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[2] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (bytespersamp == 4)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    alias.f = (float)(*fp * normalfactor);
                    xx = alias.ui;
                    sp2[0] = (xx >> 24); sp2[1] = (xx >> 16);
                    sp2[2] = (xx >> 8); sp2[3] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    alias.f = (float)(*fp * normalfactor);
                    xx = alias.ui;
                    sp2[3] = (xx >> 24); sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8); sp2[0] = xx;
                }
            }
        }
    }
}
static void soundfile_xferout_float(int nchannels, t_float **vecs,
    unsigned char *buf, int nitems, long onset, int bytespersamp,
    int bigendian, t_sample normalfactor, int spread)
{
    int i, j;
    unsigned char *sp, *sp2;
    t_float *fp;
    t_aliasfloatuint alias;
    int bytesperframe = bytespersamp * nchannels;
    long xx;
    for (i = 0, sp = buf; i < nchannels; i++, sp += bytespersamp)
    {
        if (bytespersamp == 2)
        {
            t_sample ff = normalfactor * 32768.;
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[0] = (xx >> 8);
                    sp2[1] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (bytespersamp == 3)
        {
            t_sample ff = normalfactor * 8388608.;
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[0] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[2] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (bytespersamp == 4)
        {
            if (bigendian)
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    alias.f = (float)(*fp * normalfactor);
                    xx = alias.ui;
                    sp2[0] = (xx >> 24); sp2[1] = (xx >> 16);
                    sp2[2] = (xx >> 8); sp2[3] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp=vecs[i] + onset;
                    j < nitems; j++, sp2 += bytesperframe, fp += spread)
                {
                    alias.f = (float)(*fp * normalfactor);
                    xx = alias.ui;
                    sp2[3] = (xx >> 24); sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8); sp2[0] = xx;
                }
            }
        }
    }
}

/* ------- soundfiler - reads and writes soundfiles to/from "garrays" ---- */
#define DEFMAXSIZE 0x7fffffff /* default maximum 16 MB per channel */
#define SAMPBUFSIZE 1024


static t_class *soundfiler_class;

typedef struct _soundfiler
{
    t_object x_obj;
    t_outlet *x_out2;
    t_canvas *x_canvas;
} t_soundfiler;

static t_soundfiler *soundfiler_new(void)
{
    t_soundfiler *x = (t_soundfiler *)pd_new(soundfiler_class);
    x->x_canvas = canvas_getcurrent();
    outlet_new(&x->x_obj, &s_float);
    x->x_out2 = outlet_new(&x->x_obj, &s_float);
    return (x);
}

static void soundfiler_readascii(t_soundfiler *x, char *filename,
    int narray, t_garray **garrays, t_word **vecs, int resize, int finalsize)
{
    t_binbuf *b = binbuf_new();
    int n, i, j, nframes, vecsize;
    t_atom *atoms, *ap;
    if (binbuf_read_via_canvas(b, filename, x->x_canvas, 0))
        return;
    n = binbuf_getnatom(b);
    atoms = binbuf_getvec(b);
    nframes = n/narray;
    if (nframes < 1)
    {
        binbuf_free(b);
        pd_error(x, "soundfiler_read: %s: empty or very short file", filename);
        return;
    }

    if (resize)
    {
        for (i = 0; i < narray; i++)
        {
            garray_resize_long(garrays[i], nframes);
            if (!garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
                bug("garray_getfloatwords[%d]", i);
        }
    }
    else if (finalsize < nframes)
        nframes = finalsize;
    for (j = 0, ap = atoms; j < nframes; j++)
        for (i = 0; i < narray; i++)
            vecs[i][j].w_float = atom_getfloat(ap++);
        /* zero out remaining elements of vectors */
    for (i = 0; i < narray; i++)
    {
        int vecsize;
        if (garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
        for (j = nframes; j < vecsize; j++)
            vecs[i][j].w_float = 0;
    }
    for (i = 0; i < narray; i++)
        garray_redraw(garrays[i]);
}

    /* soundfiler_read ...
    
    usage: read [flags] filename table ...
    flags:
        -skip <frames> ... frames to skip in file
        -onset <frames> ... onset in table to read into (NOT DONE YET)
        -raw <headersize channels bytes endian>
        -resize
        -maxsize <max-size>
    */

#define RAWSYNTAX "'-raw' flag syntax: " \
                  "<headersize> <channels> <bytespersample> " \
                  "<endianness: 'b' for big, 'l' for little, 'n' for auto>"

static void soundfiler_read(t_soundfiler *x, t_symbol *s,
    int argc, t_atom *argv)
{
    /* copies of argc, argv so we can iterate without mutating
       the originals (needed for error reporting below) */
    int ac = argc;
    t_atom *av = argv;
    t_soundfile_info info;
    int resize = 0, i, j;
    long skipframes = 0, finalsize = 0,
        maxsize = DEFMAXSIZE, itemsread = 0;
    int fd = -1;
    char endianness, *filename;
    t_garray *garrays[MAXSFCHANS];
    t_word *vecs[MAXSFCHANS];
    char sampbuf[SAMPBUFSIZE];
    int bufframes, nitems;
    FILE *fp;
    int ascii = 0;
    info.samplerate = 0;
    info.channels = 0;
    info.bytespersample = 0;
    info.headersize = -1;
    info.bigendian = 0;
    info.bytelimit = 0x7fffffff;
    while (ac > 0 && atom_getsymbol(av)->s_name[0] == '-')
    {
        char *flag = av->a_w.w_symbol->s_name;
        if (!strcmp(flag, "-skip"))
        {
            if (flag_missing_floatarg(x, s, argc, argv, flag, ac, av))
                goto done;
            if ((skipframes = av[1].a_w.w_float) < 0)
            {
                argerror(x, s, argc, argv,
                    "'-skip' flag does not allow a negative number");
                goto done;
            }
            ac -= 2; av += 2;
        }
        else if (!strcmp(flag, "-ascii"))
        {
            if (info.headersize >= 0)
                post("soundfiler_read: '-raw' overidden by '-ascii'");
            ascii = 1;
            ac--; av++;
        }
        else if (!strcmp(flag, "-raw"))
        {
            if (ascii)
                post("soundfiler_read: '-raw' overridden by '-ascii'");
            if (ac < 5)
            {
                argerror(x, s, argc, argv,
                    "'-raw' flag needs four arguments\n" RAWSYNTAX);
                goto done;
            }
            if (av[1].a_type != A_FLOAT)
            {
                argerror(x, s, argc, argv,
                  "'-raw' flag needs a float for the headersize\n" RAWSYNTAX);
                goto done;
            }
            info.headersize = av[1].a_w.w_float;
            if (info.headersize < 0)
            {
                argerror(x, s, argc, argv,
                    "'-raw' headersize cannot be less than zero\n"RAWSYNTAX);
                goto done;
            }
            if (av[2].a_type != A_FLOAT)
            {
                argerror(x, s, argc, argv,
                  "'-raw' flag needs a float to specify channels\n"RAWSYNTAX);
                goto done;
            }
            info.channels = av[2].a_w.w_float;
            if (info.channels < 1)
            {
                argerror(x, s, argc, argv,
                    "'-raw' flag needs at least one channel\n" RAWSYNTAX);
                goto done;
            }
            if (info.channels > MAXSFCHANS)
            {
                argerror(x, s, argc, argv,
                  "'-raw' channels value %d exceeds "
                  "maximum of %d channels\n" RAWSYNTAX,
                    info.channels, MAXSFCHANS);
                goto done;
            }
            if (av[3].a_type != A_FLOAT)
            {
                argerror(x, s, argc, argv,
                    "'-raw' flag needs a float to specify "
                    "bytes per sample\n" RAWSYNTAX);
                goto done;
            }
            info.bytespersample = av[3].a_w.w_float;
            if (info.bytespersample < 2)
            {
                argerror(x, s, argc, argv,
                    "'-raw' bytes per sample must be at least 2\n" RAWSYNTAX);
                goto done;
            }
            if (info.bytespersample > 4)
            {
                argerror(x, s, argc, argv,
                   "'-raw' bytes per sample must be less than 4\n" RAWSYNTAX);
                goto done;
            }
            if (av[4].a_type != A_SYMBOL ||
                ((endianness = av[4].a_w.w_symbol->s_name[0]) != 'b'
                  && endianness != 'l' && endianness != 'n'))
            {
                argerror(x, s, argc, argv,
                   "'-raw' endianness must be 'l' or 'b' or 'n'\n" RAWSYNTAX);
                goto done;
            }
            if (endianness == 'b')
                info.bigendian = 1;
            else if (endianness == 'l')
                info.bigendian = 0;
            else
                info.bigendian = garray_ambigendian();
            info.samplerate = sys_getsr();
            ac -= 5; av += 5;
        }
        else if (!strcmp(flag, "-resize"))
        {
            if (flag_has_unexpected_floatarg(x, s, argc, argv,
                flag, ac, av))
            {
                goto done;
            }
            resize = 1;
            ac -= 1; av += 1;
        }
        else if (!strcmp(flag, "-maxsize"))
        {
            if (flag_missing_floatarg(x, s, argc, argv, flag, ac, av))
                goto done;
            t_float fmax = av[1].a_w.w_float;
            if (fmax > (t_float)LONG_MAX)
            {
                argerror(x, s, argc, argv,
                    "'-maxsize' overflow detected. "
                    "Setting '-maxsize' to maximum legal value (%g) and "
                    "continuing...", (t_float)LONG_MAX);
                maxsize = LONG_MAX;
            }
            else if (fmax < 0.)
            {
                argerror(x, s, argc, argv,
                    "'-maxsize' flag cannot be less than zero");
                goto done;
            }
            else
            {
                maxsize = (long)fmax;
            }
            resize = 1;     /* maxsize implies resize. */
            ac -= 2; av += 2;
        }
        else
        {
            argerror(x, s, argc, argv, "unknown flag '%s'", flag);
            goto done;
        }
    }
    if (ac < 1)
    {
        argerror(x, s, argc, argv, "need filename and table argument(s)");
        goto done;
    }
    if (ac > MAXSFCHANS + 1)
    {
        argerror(x, s, argc, argv,
            "cannot write more than %d channels", MAXSFCHANS);
        goto done;
    }
    if (av->a_type != A_SYMBOL)
    {
        argerror(x, s, argc, argv, "filename must be a symbol");
        goto done;
    }
    filename = av[0].a_w.w_symbol->s_name;
    ac--; av++;
    
    for (i = 0; i < ac; i++)
    {
        int vecsize;
        if (av[i].a_type != A_SYMBOL)
            goto done;
        if (!(garrays[i] =
            (t_garray *)pd_findbyclass(av[i].a_w.w_symbol, garray_class)))
        {
            argerror(x, s, argc, argv, "%s: no such table",
                av[i].a_w.w_symbol->s_name);
            goto done;
        }
        else if (!garray_getfloatwords(garrays[i], &vecsize, 
                &vecs[i]))
            error("%s: bad template for tabwrite",
                av[i].a_w.w_symbol->s_name);
        if (finalsize && finalsize != vecsize && !resize)
        {
            post("soundfiler_read: arrays have different lengths; resizing...");
            resize = 1;
        }
        finalsize = vecsize;
    }
    if (ascii)
    {
        soundfiler_readascii(x, filename, ac, garrays, vecs, resize,
            finalsize);
        return;
    }
    fd = open_soundfile_via_canvas(x->x_canvas, filename, &info, skipframes);
    
    if (fd < 0)
    {
        argerror(x, s, argc, argv, "%s: %s", filename, (errno == EIO ?
            "unknown or bad header format" : strerror(errno)));
        /* don't bail yet so we can potentially give a warning below */
    }

    /* check if the filename looks like a flag; if so, post a warning */
    int nodash;
    if (file_is_a_flag_name(gensym(filename), &nodash))
    {
        post("warning: filename '%s' looks like a flag%s",
            filename, nodash ? " name" : "");
    }

    /* Now that we've posted our warning, bail if we couldn't open the file */
    if (fd < 0)
        goto done;

    if (resize)
    {
            /* figure out what to resize to */
        long poswas, eofis, framesinfile;

        poswas = lseek(fd, 0, SEEK_CUR);
        eofis = lseek(fd, 0, SEEK_END);
        if (poswas < 0 || eofis < 0 || eofis < poswas)
        {
            argerror(x, s, argc, argv, "lseek failed: %ld..%ld", poswas,
                eofis);
            goto done;
        }
        lseek(fd, poswas, SEEK_SET);
        framesinfile = (eofis - poswas) / (info.channels * info.bytespersample);
        if (framesinfile > maxsize)
        {
            argerror(x, s, argc, argv, "truncated to %ld elements", maxsize);
            framesinfile = maxsize;
        }
        if (framesinfile > info.bytelimit /
            (info.channels * info.bytespersample))
        {
            framesinfile = info.bytelimit /
                (info.channels * info.bytespersample);
        }
        finalsize = framesinfile;
        for (i = 0; i < ac; i++)
        {
            int vecsize;

            garray_resize_long(garrays[i], finalsize);

            /* for sanity's sake let's clear the save-in-patch flag here */
            garray_setsaveit(garrays[i], 0);
            if (!garray_getfloatwords(garrays[i], &vecsize, &vecs[i])
                || (vecsize != framesinfile))
            {
                /* if the resize failed, garray_resize reported the error */
                argerror(x, s, argc, argv, "resize failed");
                goto done;
            }
        }
    }
    if (!finalsize) finalsize = 0x7fffffff;
    if (finalsize > info.bytelimit / (info.channels * info.bytespersample))
        finalsize = info.bytelimit / (info.channels * info.bytespersample);
    fp = fdopen(fd, "rb");
    bufframes = SAMPBUFSIZE / (info.channels * info.bytespersample);

    for (itemsread = 0; itemsread < finalsize; )
    {
        long thisread = finalsize - itemsread;
        thisread = (thisread > bufframes ? bufframes : thisread);
        nitems = fread(sampbuf, info.channels * info.bytespersample, thisread,
            fp);
        if (nitems <= 0) break;
        soundfile_xferin_float(info.channels, ac, (t_float **)vecs, itemsread,
            (unsigned char *)sampbuf, nitems, info.bytespersample,
            info.bigendian, sizeof(t_word)/sizeof(t_sample));
        itemsread += nitems;
    }
        /* zero out remaining elements of vectors */
        
    for (i = 0; i < ac; i++)
    {
        int vecsize;
        if (garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
            for (j = itemsread; j < vecsize; j++)
                vecs[i][j].w_float = 0;
    }
        /* zero out vectors in excess of number of channels */
    for (i = info.channels; i < ac; i++)
    {
        int vecsize;
        t_word *foo;
        if (garray_getfloatwords(garrays[i], &vecsize, &foo))
            for (j = 0; j < vecsize; j++)
                foo[j].w_float = 0;
    }
        /* do all graphics updates */
    for (i = 0; i < ac; i++)
        garray_redraw(garrays[i]);
    fclose(fp);
    fd = -1;
done:
    if (fd >= 0)
        sys_close(fd);
    outlet_soundfile_info(x->x_out2, &info);
    outlet_float(x->x_obj.ob_outlet, (t_float)itemsread); 
}

    /* this is broken out from soundfiler_write below so garray_write can
    call it too... not done yet though. */

long soundfiler_dowrite(void *obj, t_canvas *canvas,
    int argc, t_atom *argv, t_soundfile_info *info)
{
    /* workaround for ruthless argc/argv mutation in writeargparse... */
    int original_argc = argc;
    t_atom *original_argv = argv;
    int swap, filetype, normalize, i;
    long onset, nframes, itemswritten = 0, j;
    t_garray *garrays[MAXSFCHANS];
    t_word *vecs[MAXSFCHANS];
    char sampbuf[SAMPBUFSIZE];
    int bufframes;
    int fd = -1;
    t_sample normfactor, biggest = 0;
    t_float samplerate;
    t_symbol *filesym;

    if (soundfiler_writeargparse(obj, gensym("write"), &argc, &argv,
        &filesym, &filetype,
        &info->bytespersample, &swap, &info->bigendian, &normalize, &onset,
        &nframes, &samplerate))
                goto usage;
    info->channels = argc;
        /* Need at least one table name for a channel to write... */
    if (info->channels < 1)
    {
        argerror(obj, gensym("write"), original_argc, original_argv,
            "argument for table name missing");
        goto usage;
    }
        /* Can't have more than max number of channels to write */
    if (info->channels > MAXSFCHANS)
    {
        argerror(obj, gensym("write"), original_argc, original_argv,
            "cannot have more than %d channels", MAXSFCHANS);
        goto usage;
    }
    if (samplerate < 0)
        info->samplerate = sys_getsr();
    else
        info->samplerate = (int)samplerate;
    for (i = 0; i < info->channels; i++)
    {
        int vecsize;
        if (argv[i].a_type != A_SYMBOL)
        {
            argerror(obj, gensym("write"), original_argc, original_argv,
                "table name must be a symbol");
            goto usage;
        }
        if (!(garrays[i] =
            (t_garray *)pd_findbyclass(argv[i].a_w.w_symbol, garray_class)))
        {
            argerror(obj, gensym("write"), original_argc, original_argv,
                "%s: no such table", argv[i].a_w.w_symbol->s_name);
            goto fail;
        }
            /* need to check this one-- */
        else if (!garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
            error("%s: bad template for tabwrite",
                argv[i].a_w.w_symbol->s_name);
        if (nframes > vecsize - onset)
            nframes = vecsize - onset;
    }
    if (nframes <= 0)
    {
        argerror(obj, gensym("write"), original_argc, original_argv,
            "no samples at onset %ld", onset);
        goto fail;
    }
        /* find biggest sample for normalizing */
    for (i = 0; i < info->channels; i++)
    {
        for (j = onset; j < nframes + onset; j++)
        {
            if (vecs[i][j].w_float > biggest)
                biggest = vecs[i][j].w_float;
            else if (-vecs[i][j].w_float > biggest)
                biggest = -vecs[i][j].w_float;
        }
    }
    if ((fd = create_soundfile(canvas, filesym->s_name, filetype,
        nframes, info->bytespersample, info->bigendian, info->channels,
            swap, info->samplerate, &info->headersize)) < 0)
    {
        post("%s: %s\n", filesym->s_name, strerror(errno));
        goto fail;
    }
    if (!normalize)
    {
        if ((info->bytespersample != 4) && (biggest > 1))
        {
            post("%s: normalizing max amplitude %f to 1", filesym->s_name,
                biggest);
            normalize = 1;
        }
        else post("%s: biggest amplitude = %f", filesym->s_name, biggest);
    }
    if (normalize)
        normfactor = (biggest > 0 ? 32767./(32768. * biggest) : 1);
    else normfactor = 1;

    bufframes = SAMPBUFSIZE / (info->channels * info->bytespersample);

    for (itemswritten = 0; itemswritten < nframes; )
    {
        int thiswrite = nframes - itemswritten, nbytes;
        thiswrite = (thiswrite > bufframes ? bufframes : thiswrite);
        soundfile_xferout_float(argc, (t_float **)vecs, (unsigned char *)sampbuf,
            thiswrite, onset, info->bytespersample, info->bigendian, normfactor,
                 sizeof(t_word)/sizeof(t_sample));
        nbytes = write(fd, sampbuf, info->channels * info->bytespersample *
            thiswrite);
        if (nbytes < info->channels * info->bytespersample * thiswrite)
        {
            post("%s: %s", filesym->s_name, strerror(errno));
            if (nbytes > 0)
                itemswritten += nbytes / (info->channels * info->bytespersample);
            break;
        }
        itemswritten += thiswrite;
        onset += thiswrite * (sizeof(t_word)/sizeof(float));
    }
    if (fd >= 0)
    {
        soundfile_finishwrite(obj, filesym->s_name, fd,
            filetype, nframes, itemswritten,
            info->channels * info->bytespersample, swap);
        sys_close(fd);
    }
    return ((float)itemswritten); 
usage:
fail:
    if (fd >= 0)
        sys_close(fd);
    return (0); 
}

static void soundfiler_write(t_soundfiler *x, t_symbol *s,
    int argc, t_atom *argv)
{
    t_soundfile_info info;
    long bozo;
    info.samplerate = 0,
    info.channels = 0,
    info.bytespersample = 0,
    info.headersize = -1,
    info.bigendian = 0,
    info.bytelimit = 0x7fffffff;

    bozo = soundfiler_dowrite(x, x->x_canvas, argc, argv, &info);
    outlet_soundfile_info(x->x_out2, &info);
    outlet_float(x->x_obj.ob_outlet, (t_float)bozo); 
}

static void soundfiler_setup(void)
{
    soundfiler_class = class_new(gensym("soundfiler"), (t_newmethod)soundfiler_new, 
        0, sizeof(t_soundfiler), 0, 0);
    class_addmethod(soundfiler_class, (t_method)soundfiler_read, gensym("read"), 
        A_GIMME, 0);
    class_addmethod(soundfiler_class, (t_method)soundfiler_write,
        gensym("write"), A_GIMME, 0);
}


/************************* readsf object ******************************/

/* READSF uses the Posix threads package; for the moment we're Linux
only although this should be portable to the other platforms.

Each instance of readsf~ owns a "child" thread for doing the unix (MSW?) file
reading.  The parent thread signals the child each time:
    (1) a file wants opening or closing;
    (2) we've eaten another 1/16 of the shared buffer (so that the
        child thread should check if it's time to read some more.)
The child signals the parent whenever a read has completed.  Signalling
is done by setting "conditions" and putting data in mutex-controlled common
areas.
*/

#define MAXBYTESPERSAMPLE 4
#define MAXVECSIZE 128

#define READSIZE 65536
#define WRITESIZE 65536
#define DEFBUFPERCHAN 262144
#define MINBUFSIZE (4 * READSIZE)
#define MAXBUFSIZE 16777216     /* arbitrary; just don't want to hang malloc */

#define REQUEST_NOTHING 0
#define REQUEST_OPEN 1
#define REQUEST_CLOSE 2
#define REQUEST_QUIT 3
#define REQUEST_BUSY 4

#define STATE_IDLE 0
#define STATE_STARTUP 1
#define STATE_STREAM 2

static t_class *readsf_class;

typedef struct _readsf
{
    t_object x_obj;
    t_canvas *x_canvas;
    t_clock *x_clock;
    char *x_buf;                            /* soundfile buffer */
    int x_bufsize;                          /* buffer size in bytes */
    int x_noutlets;                         /* number of audio outlets */
    t_sample *(x_outvec[MAXSFCHANS]);       /* audio vectors */
    int x_vecsize;                          /* vector size for transfers */
    t_outlet *x_bangout;                    /* bang-on-done outlet */
    int x_state;                            /* opened, running, or idle */
    t_float x_insamplerate;   /* sample rate of input signal if known */
        /* parameters to communicate with subthread */
    int x_requestcode;      /* pending request from parent to I/O thread */
    char *x_filename;       /* file to open (string is permanently allocated) */
    int x_fileerror;        /* slot for "errno" return */
    int x_skipheaderbytes;  /* size of header we'll skip */
    int x_bytespersample;   /* bytes per sample (2 or 3) */
    int x_bigendian;        /* true if file is big-endian */
    int x_sfchannels;       /* number of channels in soundfile */
    t_float x_samplerate;     /* sample rate of soundfile */
    long x_onsetframes;     /* number of sample frames to skip */
    long x_bytelimit;       /* max number of data bytes to read */
    int x_fd;               /* filedesc */
    int x_fifosize;         /* buffer size appropriately rounded down */            
    int x_fifohead;         /* index of next byte to get from file */
    int x_fifotail;         /* index of next byte the ugen will read */
    int x_eof;              /* true if fifohead has stopped changing */
    int x_sigcountdown;     /* counter for signalling child for more data */
    int x_sigperiod;        /* number of ticks per signal */
    int x_filetype;         /* writesf~ only; type of file to create */
    int x_itemswritten;     /* writesf~ only; items writen */
    int x_swap;             /* writesf~ only; true if byte swapping */
    t_float x_f;              /* writesf~ only; scalar for signal inlet */
    pthread_mutex_t x_mutex;
    pthread_cond_t x_requestcondition;
    pthread_cond_t x_answercondition;
    pthread_t x_childthread;
} t_readsf;


/************** the child thread which performs file I/O ***********/

#if 0
static void pute(char *s)   /* debug routine */
{
    write(2, s, strlen(s));
}
#define DEBUG_SOUNDFILE
#endif

#if 1
#define sfread_cond_wait pthread_cond_wait
#define sfread_cond_signal pthread_cond_signal
#else
#include <sys/time.h>    /* debugging version... */
#include <sys/types.h>
static void readsf_fakewait(pthread_mutex_t *b)
{
    struct timeval timout;
    timout.tv_sec = 0;
    timout.tv_usec = 1000000;
    pthread_mutex_unlock(b);
    select(0, 0, 0, 0, &timout);
    pthread_mutex_lock(b);
}

#define sfread_cond_wait(a,b) readsf_fakewait(b)
#define sfread_cond_signal(a) 
#endif

static void *readsf_child_main(void *zz)
{
    t_readsf *x = zz;
#ifdef DEBUG_SOUNDFILE
    pute("1\n");
#endif
    pthread_mutex_lock(&x->x_mutex);
    while (1)
    {
        int fd, fifohead;
        char *buf;
#ifdef DEBUG_SOUNDFILE
        pute("0\n");
#endif
        if (x->x_requestcode == REQUEST_NOTHING)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait 2\n");
#endif
            sfread_cond_signal(&x->x_answercondition);
            sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
            pute("3\n");
#endif
        }
        else if (x->x_requestcode == REQUEST_OPEN)
        {
            int sysrtn, wantbytes;
            
                /* copy file stuff out of the data structure so we can
                relinquish the mutex while we're in open_soundfile(). */
            t_soundfile_info info;
            long onsetframes = x->x_onsetframes;
            info.samplerate = x->x_samplerate;
            info.channels = x->x_sfchannels;
            info.headersize = x->x_skipheaderbytes;
            info.bytespersample = x->x_bytespersample;
            info.bigendian = x->x_bigendian;
            info.bytelimit = 0x7fffffff;
            char *filename = x->x_filename;
            char *dirname = canvas_getdir(x->x_canvas)->s_name;
                /* alter the request code so that an ensuing "open" will get
                noticed. */
#ifdef DEBUG_SOUNDFILE
            pute("4\n");
#endif
            x->x_requestcode = REQUEST_BUSY;
            x->x_fileerror = 0;

                /* if there's already a file open, close it */
            if (x->x_fd >= 0)
            {
                fd = x->x_fd;
                pthread_mutex_unlock(&x->x_mutex);
                sys_close(fd);
                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
                if (x->x_requestcode != REQUEST_BUSY)
                    goto lost;
            }
                /* open the soundfile with the mutex unlocked */
            pthread_mutex_unlock(&x->x_mutex);
            fd = open_soundfile(dirname, filename, &info, onsetframes);      
            pthread_mutex_lock(&x->x_mutex);

#ifdef DEBUG_SOUNDFILE
            pute("5\n");
#endif
                /* copy back into the instance structure. */
            x->x_bytespersample = info.bytespersample;
            x->x_sfchannels = info.channels;
            x->x_bigendian = info.bigendian;
            x->x_fd = fd;
            x->x_bytelimit = info.bytelimit;
            if (fd < 0)
            {
                x->x_fileerror = errno;
                x->x_eof = 1;
#ifdef DEBUG_SOUNDFILE
                pute("open failed\n");
                pute(filename);
                pute(dirname);
#endif
                goto lost;
            }
                /* check if another request has been made; if so, field it */
            if (x->x_requestcode != REQUEST_BUSY)
                goto lost;
#ifdef DEBUG_SOUNDFILE
            pute("6\n");
#endif
            x->x_fifohead = 0;
                    /* set fifosize from bufsize.  fifosize must be a
                    multiple of the number of bytes eaten for each DSP
                    tick.  We pessimistically assume MAXVECSIZE samples
                    per tick since that could change.  There could be a
                    problem here if the vector size increases while a
                    soundfile is being played...  */
            x->x_fifosize = x->x_bufsize - (x->x_bufsize %
                (x->x_bytespersample * x->x_sfchannels * MAXVECSIZE));
                    /* arrange for the "request" condition to be signalled 16
                    times per buffer */
#ifdef DEBUG_SOUNDFILE
            char boo[80];
            sprintf(boo, "fifosize %d\n", 
                x->x_fifosize);
            pute(boo);
#endif
            x->x_sigcountdown = x->x_sigperiod =
                (x->x_fifosize /
                    (16 * x->x_bytespersample * x->x_sfchannels *
                        x->x_vecsize));
                /* in a loop, wait for the fifo to get hungry and feed it */

            while (x->x_requestcode == REQUEST_BUSY)
            {
                int fifosize = x->x_fifosize;
#ifdef DEBUG_SOUNDFILE
                pute("77\n");
#endif
                if (x->x_eof)
                    break;
                if (x->x_fifohead >= x->x_fifotail)
                {
                        /* if the head is >= the tail, we can immediately read
                        to the end of the fifo.  Unless, that is, we would
                        read all the way to the end of the buffer and the 
                        "tail" is zero; this would fill the buffer completely
                        which isn't allowed because you can't tell a completely
                        full buffer from an empty one. */
                    if (x->x_fifotail || (fifosize - x->x_fifohead > READSIZE))
                    {
                        wantbytes = fifosize - x->x_fifohead;
                        if (wantbytes > READSIZE)
                            wantbytes = READSIZE;
                        if (wantbytes > x->x_bytelimit)
                            wantbytes = x->x_bytelimit;
#ifdef DEBUG_SOUNDFILE
                        sprintf(boo, "head %d, tail %d, size %d\n", 
                            x->x_fifohead, x->x_fifotail, wantbytes);
                        pute(boo);
#endif
                    }
                    else
                    {
#ifdef DEBUG_SOUNDFILE
                        pute("wait 7a ...\n");
#endif
                        sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE
                        pute("signalled\n");
#endif
                        sfread_cond_wait(&x->x_requestcondition,
                            &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                        pute("7a done\n");
#endif
                        continue;
                    }
                }
                else
                {
                        /* otherwise check if there are at least READSIZE
                        bytes to read.  If not, wait and loop back. */
                    wantbytes =  x->x_fifotail - x->x_fifohead - 1;
                    if (wantbytes < READSIZE)
                    {
#ifdef DEBUG_SOUNDFILE
                        pute("wait 7...\n");
#endif
                        sfread_cond_signal(&x->x_answercondition);
                        sfread_cond_wait(&x->x_requestcondition,
                            &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                        pute("7 done\n");
#endif
                        continue;
                    }
                    else wantbytes = READSIZE;
                    if (wantbytes > x->x_bytelimit)
                        wantbytes = x->x_bytelimit;
                }
#ifdef DEBUG_SOUNDFILE
                pute("8\n");
#endif
                fd = x->x_fd;
                buf = x->x_buf;
                fifohead = x->x_fifohead;
                pthread_mutex_unlock(&x->x_mutex);
                sysrtn = read(fd, buf + fifohead, wantbytes);
                pthread_mutex_lock(&x->x_mutex);
                if (x->x_requestcode != REQUEST_BUSY)
                    break;
                if (sysrtn < 0)
                {
#ifdef DEBUG_SOUNDFILE
                    pute("fileerror\n");
#endif
                    x->x_fileerror = errno;
                    break;
                }
                else if (sysrtn == 0)
                {
                    x->x_eof = 1;
                    break;
                }
                else
                {
                    x->x_fifohead += sysrtn;
                    x->x_bytelimit -= sysrtn;
                    if (x->x_fifohead == fifosize)
                        x->x_fifohead = 0;
                    if (x->x_bytelimit <= 0)
                    {
                        x->x_eof = 1;
                        break;
                    }
                }
#ifdef DEBUG_SOUNDFILE
                sprintf(boo, "after: head %d, tail %d\n", 
                    x->x_fifohead, x->x_fifotail);
                pute(boo);
#endif
                    /* signal parent in case it's waiting for data */
                sfread_cond_signal(&x->x_answercondition);
            }
        lost:

            if (x->x_requestcode == REQUEST_BUSY)
                x->x_requestcode = REQUEST_NOTHING;
                /* fell out of read loop: close file if necessary,
                set EOF and signal once more */
            if (x->x_fd >= 0)
            {
                fd = x->x_fd;
                pthread_mutex_unlock(&x->x_mutex);
                sys_close(fd);
                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
            }
            sfread_cond_signal(&x->x_answercondition);

        }
        else if (x->x_requestcode == REQUEST_CLOSE)
        {
            if (x->x_fd >= 0)
            {
                fd = x->x_fd;
                pthread_mutex_unlock(&x->x_mutex);
                sys_close(fd);
                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
            }
            if (x->x_requestcode == REQUEST_CLOSE)
                x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
        }
        else if (x->x_requestcode == REQUEST_QUIT)
        {
            if (x->x_fd >= 0)
            {
                fd = x->x_fd;
                pthread_mutex_unlock(&x->x_mutex);
                sys_close(fd);
                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
            }
            x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
            break;
        }
        else
        {
#ifdef DEBUG_SOUNDFILE
            pute("13\n");
#endif
        }
    }
#ifdef DEBUG_SOUNDFILE
    pute("thread exit\n");
#endif
    pthread_mutex_unlock(&x->x_mutex);
    return (0);
}

/******** the object proper runs in the calling (parent) thread ****/

static void readsf_tick(t_readsf *x);

static void *readsf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
    t_readsf *x;
    int nchannels = fnchannels, bufsize = fbufsize, i;
    char *buf;
    
    if (nchannels < 1)
        nchannels = 1;
    else if (nchannels > MAXSFCHANS)
        nchannels = MAXSFCHANS;
    if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
    else if (bufsize < MINBUFSIZE)
        bufsize = MINBUFSIZE;
    else if (bufsize > MAXBUFSIZE)
        bufsize = MAXBUFSIZE;
    buf = getbytes(bufsize);
    if (!buf) return (0);
    
    x = (t_readsf *)pd_new(readsf_class);
    
    for (i = 0; i < nchannels; i++)
        outlet_new(&x->x_obj, gensym("signal"));
    x->x_noutlets = nchannels;
    x->x_bangout = outlet_new(&x->x_obj, &s_bang);
    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_requestcondition, 0);
    pthread_cond_init(&x->x_answercondition, 0);
    x->x_vecsize = MAXVECSIZE;
    x->x_state = STATE_IDLE;
    x->x_clock = clock_new(x, (t_method)readsf_tick);
    x->x_canvas = canvas_getcurrent();
    x->x_bytespersample = 2;
    x->x_sfchannels = 1;
    x->x_fd = -1;
    x->x_buf = buf;
    x->x_bufsize = bufsize;
    x->x_fifosize = x->x_fifohead = x->x_fifotail = x->x_requestcode = 0;
    pthread_create(&x->x_childthread, 0, readsf_child_main, x);
    return (x);
}

static void readsf_tick(t_readsf *x)
{
    outlet_bang(x->x_bangout);
}

static t_int *readsf_perform(t_int *w)
{
    t_readsf *x = (t_readsf *)(w[1]);
    int vecsize = x->x_vecsize, noutlets = x->x_noutlets, i, j,
        bytespersample = x->x_bytespersample,
        bigendian = x->x_bigendian;
    t_sample *fp;
    if (x->x_state == STATE_STREAM)
    {
        int wantbytes, sfchannels = x->x_sfchannels;
        pthread_mutex_lock(&x->x_mutex);
        wantbytes = sfchannels * vecsize * bytespersample;
        while (
            !x->x_eof && x->x_fifohead >= x->x_fifotail &&
                x->x_fifohead < x->x_fifotail + wantbytes-1)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait...\n");
#endif
            sfread_cond_signal(&x->x_requestcondition);
            sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
                /* resync local cariables -- bug fix thanks to Shahrokh */
            vecsize = x->x_vecsize;
            bytespersample = x->x_bytespersample;
            sfchannels = x->x_sfchannels;
            wantbytes = sfchannels * vecsize * bytespersample;
            bigendian = x->x_bigendian;
#ifdef DEBUG_SOUNDFILE
            pute("done\n");
#endif
        }
        if (x->x_eof && x->x_fifohead >= x->x_fifotail &&
            x->x_fifohead < x->x_fifotail + wantbytes-1)
        {
            int xfersize;
            if (x->x_fileerror)
            {
                pd_error(x, "dsp: %s: %s", x->x_filename,
                    (x->x_fileerror == EIO ?
                        "unknown or bad header format" :
                            strerror(x->x_fileerror)));
            }
            clock_delay(x->x_clock, 0);
            x->x_state = STATE_IDLE;

                /* if there's a partial buffer left, copy it out. */
            xfersize = (x->x_fifohead - x->x_fifotail + 1) /
                (sfchannels * bytespersample);
            if (xfersize)
            {
                soundfile_xferin_sample(sfchannels, noutlets, x->x_outvec, 0,
                    (unsigned char *)(x->x_buf + x->x_fifotail), xfersize,
                        bytespersample, bigendian);
                vecsize -= xfersize;
            }
                /* then zero out the (rest of the) output */
            for (i = 0; i < noutlets; i++)
                for (j = vecsize, fp = x->x_outvec[i] + xfersize; j--; )
                    *fp++ = 0;

            sfread_cond_signal(&x->x_requestcondition);
            pthread_mutex_unlock(&x->x_mutex);
            return (w+2); 
        }

        soundfile_xferin_sample(sfchannels, noutlets, x->x_outvec, 0,
            (unsigned char *)(x->x_buf + x->x_fifotail), vecsize,
                bytespersample, bigendian);
        
        x->x_fifotail += wantbytes;
        if (x->x_fifotail >= x->x_fifosize)
            x->x_fifotail = 0;
        if ((--x->x_sigcountdown) <= 0)
        {
            sfread_cond_signal(&x->x_requestcondition);
            x->x_sigcountdown = x->x_sigperiod;
        }
        pthread_mutex_unlock(&x->x_mutex);
    }
    else
    {
        for (i = 0; i < noutlets; i++)
            for (j = vecsize, fp = x->x_outvec[i]; j--; )
                *fp++ = 0;
    }
    return (w+2);
}

static void readsf_start(t_readsf *x)
{
    /* start making output.  If we're in the "startup" state change
    to the "running" state. */
    if (x->x_state == STATE_STARTUP)
        x->x_state = STATE_STREAM;
    else pd_error(x, "readsf: start requested with no prior 'open'");
}

static void readsf_stop(t_readsf *x)
{
        /* LATER rethink whether you need the mutex just to set a variable? */
    pthread_mutex_lock(&x->x_mutex);
    x->x_state = STATE_IDLE;
    x->x_requestcode = REQUEST_CLOSE;
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

static void readsf_float(t_readsf *x, t_floatarg f)
{
    if (f != 0)
        readsf_start(x);
    else readsf_stop(x);
}

    /* open method.  Called as:
    open filename [skipframes headersize channels bytespersamp endianness]
        (if headersize is zero, header is taken to be automatically
        detected; thus, use the special "-1" to mean a truly headerless file.)
    */

static void readsf_open(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
    t_symbol *filesym = atom_getsymbolarg(0, argc, argv);
    t_float onsetframes = atom_getfloatarg(1, argc, argv);
    t_float headerbytes = atom_getfloatarg(2, argc, argv);
    t_float channels = atom_getfloatarg(3, argc, argv);
    t_float bytespersamp = atom_getfloatarg(4, argc, argv);
    t_symbol *endian = atom_getsymbolarg(5, argc, argv);
    if (!*filesym->s_name)
        return;
    pthread_mutex_lock(&x->x_mutex);
    x->x_requestcode = REQUEST_OPEN;
    x->x_filename = filesym->s_name;
    x->x_fifotail = 0;
    x->x_fifohead = 0;
    if (*endian->s_name == 'b')
         x->x_bigendian = 1;
    else if (*endian->s_name == 'l')
         x->x_bigendian = 0;
    else if (*endian->s_name)
        pd_error(x, "endianness neither 'b' nor 'l'");
    else x->x_bigendian = garray_ambigendian();
    x->x_onsetframes = (onsetframes > 0 ? onsetframes : 0);
    x->x_skipheaderbytes = (headerbytes > 0 ? headerbytes : 
        (headerbytes == 0 ? -1 : 0));
    x->x_sfchannels = (channels >= 1 ? channels : 1);
    x->x_bytespersample = (bytespersamp > 2 ? bytespersamp : 2);
    x->x_eof = 0;
    x->x_fileerror = 0;
    x->x_state = STATE_STARTUP;
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

static void readsf_dsp(t_readsf *x, t_signal **sp)
{
    int i, noutlets = x->x_noutlets;
    pthread_mutex_lock(&x->x_mutex);
    x->x_vecsize = sp[0]->s_n;
    
    x->x_sigperiod = (x->x_fifosize /
        (x->x_bytespersample * x->x_sfchannels * x->x_vecsize));
    for (i = 0; i < noutlets; i++)
        x->x_outvec[i] = sp[i]->s_vec;
    pthread_mutex_unlock(&x->x_mutex);
    dsp_add(readsf_perform, 1, x);
}

static void readsf_print(t_readsf *x)
{
    post("state %d", x->x_state);
    post("fifo head %d", x->x_fifohead);
    post("fifo tail %d", x->x_fifotail);
    post("fifo size %d", x->x_fifosize);
    post("fd %d", x->x_fd);
    post("eof %d", x->x_eof);
}

static void readsf_free(t_readsf *x)
{
        /* request QUIT and wait for acknowledge */
    void *threadrtn;
    pthread_mutex_lock(&x->x_mutex);
    x->x_requestcode = REQUEST_QUIT;
    sfread_cond_signal(&x->x_requestcondition);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    pthread_mutex_unlock(&x->x_mutex);
    if (pthread_join(x->x_childthread, &threadrtn))
        error("readsf_free: join failed");
    
    pthread_cond_destroy(&x->x_requestcondition);
    pthread_cond_destroy(&x->x_answercondition);
    pthread_mutex_destroy(&x->x_mutex);
    freebytes(x->x_buf, x->x_bufsize);
    clock_free(x->x_clock);
}

static void readsf_setup(void)
{
    readsf_class = class_new(gensym("readsf~"), (t_newmethod)readsf_new, 
        (t_method)readsf_free, sizeof(t_readsf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addfloat(readsf_class, (t_method)readsf_float);
    class_addmethod(readsf_class, (t_method)readsf_start, gensym("start"), 0);
    class_addmethod(readsf_class, (t_method)readsf_stop, gensym("stop"), 0);
    class_addmethod(readsf_class, (t_method)readsf_dsp, gensym("dsp"),
        A_CANT, 0);
    class_addmethod(readsf_class, (t_method)readsf_open, gensym("open"), 
        A_GIMME, 0);
    class_addmethod(readsf_class, (t_method)readsf_print, gensym("print"), 0);
}

/******************************* writesf *******************/

static t_class *writesf_class;

#define t_writesf t_readsf      /* just re-use the structure */

/************** the child thread which performs file I/O ***********/

static void *writesf_child_main(void *zz)
{
    t_writesf *x = zz;
#ifdef DEBUG_SOUNDFILE
    pute("1\n");
#endif
    pthread_mutex_lock(&x->x_mutex);
    while (1)
    {
#ifdef DEBUG_SOUNDFILE
        pute("0\n");
#endif
        if (x->x_requestcode == REQUEST_NOTHING)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait 2\n");
#endif
            sfread_cond_signal(&x->x_answercondition);
            sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
            pute("3\n");
#endif
        }
        else if (x->x_requestcode == REQUEST_OPEN)
        {
            int fd, sysrtn, writebytes;
            
                /* copy file stuff out of the data structure so we can
                relinquish the mutex while we're in open_soundfile(). */
            int bytespersample = x->x_bytespersample;
            int sfchannels = x->x_sfchannels;
            int bigendian = x->x_bigendian;
            int filetype = x->x_filetype;
            char *filename = x->x_filename;
            t_canvas *canvas = x->x_canvas;
            t_float samplerate = x->x_samplerate;

                /* alter the request code so that an ensuing "open" will get
                noticed. */
#ifdef DEBUG_SOUNDFILE
            pute("4\n");
#endif
            x->x_requestcode = REQUEST_BUSY;
            x->x_fileerror = 0;

                /* if there's already a file open, close it.  This
                should never happen since writesf_open() calls stop if
                needed and then waits until we're idle. */
            if (x->x_fd >= 0)
            {
                int bytesperframe = x->x_bytespersample * x->x_sfchannels;
                char *filename = x->x_filename;
                int fd = x->x_fd;
                int filetype = x->x_filetype;
                int itemswritten = x->x_itemswritten;
                int swap = x->x_swap;
                pthread_mutex_unlock(&x->x_mutex);
                
                soundfile_finishwrite(x, filename, fd,
                    filetype, 0x7fffffff, itemswritten,
                    bytesperframe, swap);
                sys_close(fd);

                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
#ifdef DEBUG_SOUNDFILE
                {
                    char s[1000];
                    sprintf(s, "bug??? ditched %d\n", itemswritten);
                    pute(s);
                }
#endif  
                if (x->x_requestcode != REQUEST_BUSY)
                    continue;
            }
                /* open the soundfile with the mutex unlocked */
            pthread_mutex_unlock(&x->x_mutex);
            fd = create_soundfile(canvas, filename, filetype, 0,
                    bytespersample, bigendian, sfchannels, 
                        garray_ambigendian() != bigendian, samplerate, 0);
            pthread_mutex_lock(&x->x_mutex);
#ifdef DEBUG_SOUNDFILE
            pute("5\n");
#endif

            if (fd < 0)
            {
                x->x_fd = -1;
                x->x_eof = 1;
                x->x_fileerror = errno;
#ifdef DEBUG_SOUNDFILE
                pute("open failed\n");
                pute(filename);
#endif
                x->x_requestcode = REQUEST_NOTHING;
                continue;
            }
            /* check if another request has been made; if so, field it */
            if (x->x_requestcode != REQUEST_BUSY)
                continue;
#ifdef DEBUG_SOUNDFILE
            pute("6\n");
#endif
            x->x_fd = fd;
            x->x_fifotail = 0;
            x->x_itemswritten = 0;
            x->x_swap = garray_ambigendian() != bigendian;      
                /* in a loop, wait for the fifo to have data and write it
                    to disk */
            while (x->x_requestcode == REQUEST_BUSY ||
                (x->x_requestcode == REQUEST_CLOSE &&
                    x->x_fifohead != x->x_fifotail))
            {
                int fifosize = x->x_fifosize, fifotail;
                char *buf = x->x_buf;
#ifdef DEBUG_SOUNDFILE
                pute("77\n");
#endif

                    /* if the head is < the tail, we can immediately write
                    from tail to end of fifo to disk; otherwise we hold off
                    writing until there are at least WRITESIZE bytes in the
                    buffer */
                if (x->x_fifohead < x->x_fifotail ||
                    x->x_fifohead >= x->x_fifotail + WRITESIZE
                    || (x->x_requestcode == REQUEST_CLOSE &&
                        x->x_fifohead != x->x_fifotail))
                {
                    writebytes = (x->x_fifohead < x->x_fifotail ?
                        fifosize : x->x_fifohead) - x->x_fifotail;
                    if (writebytes > READSIZE)
                        writebytes = READSIZE;
                }
                else
                {
#ifdef DEBUG_SOUNDFILE
                    pute("wait 7a ...\n");
#endif
                    sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE
                    pute("signalled\n");
#endif
                    sfread_cond_wait(&x->x_requestcondition,
                        &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                    pute("7a done\n");
#endif
                    continue;
                }
#ifdef DEBUG_SOUNDFILE
                pute("8\n");
#endif
                fifotail = x->x_fifotail;
                fd = x->x_fd;
                pthread_mutex_unlock(&x->x_mutex);
                sysrtn = write(fd, buf + fifotail, writebytes);
                pthread_mutex_lock(&x->x_mutex);
                if (x->x_requestcode != REQUEST_BUSY &&
                    x->x_requestcode != REQUEST_CLOSE)
                        break;
                if (sysrtn < writebytes)
                {
#ifdef DEBUG_SOUNDFILE
                    pute("fileerror\n");
#endif
                    x->x_fileerror = errno;
                    break;
                }
                else
                {
                    x->x_fifotail += sysrtn;
                    if (x->x_fifotail == fifosize)
                        x->x_fifotail = 0;
                }
                x->x_itemswritten +=
                    sysrtn / (x->x_bytespersample * x->x_sfchannels);
#ifdef DEBUG_SOUNDFILE
                sprintf(boo, "after: head %d, tail %d written %d\n", 
                    x->x_fifohead, x->x_fifotail, x->x_itemswritten);
                pute(boo);
#endif
                    /* signal parent in case it's waiting for data */
                sfread_cond_signal(&x->x_answercondition);
            }
        }
        else if (x->x_requestcode == REQUEST_CLOSE ||
            x->x_requestcode == REQUEST_QUIT)
        {
            int quit = (x->x_requestcode == REQUEST_QUIT);
            if (x->x_fd >= 0)
            {
                int bytesperframe = x->x_bytespersample * x->x_sfchannels;
                char *filename = x->x_filename;
                int fd = x->x_fd;
                int filetype = x->x_filetype;
                int itemswritten = x->x_itemswritten;
                int swap = x->x_swap;
                pthread_mutex_unlock(&x->x_mutex);

                soundfile_finishwrite(x, filename, fd,
                    filetype, 0x7fffffff, itemswritten,
                    bytesperframe, swap);
                sys_close(fd);

                pthread_mutex_lock(&x->x_mutex);
                x->x_fd = -1;
            }
            x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
            if (quit)
                break;
        }
        else
        {
#ifdef DEBUG_SOUNDFILE
            pute("13\n");
#endif
        }
    }
#ifdef DEBUG_SOUNDFILE
    pute("thread exit\n");
#endif
    pthread_mutex_unlock(&x->x_mutex);
    return (0);
}

/******** the object proper runs in the calling (parent) thread ****/

/* Not sure why this is declared here so commenting it out... */
//static void writesf_tick(t_writesf *x);

static void *writesf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
    t_writesf *x;
    int nchannels = fnchannels, bufsize = fbufsize, i;
    char *buf;
    
    if (nchannels < 1)
        nchannels = 1;
    else if (nchannels > MAXSFCHANS)
        nchannels = MAXSFCHANS;
    if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
    else if (bufsize < MINBUFSIZE)
        bufsize = MINBUFSIZE;
    else if (bufsize > MAXBUFSIZE)
        bufsize = MAXBUFSIZE;
    buf = getbytes(bufsize);
    if (!buf) return (0);
    
    x = (t_writesf *)pd_new(writesf_class);
    
    for (i = 1; i < nchannels; i++)
        inlet_new(&x->x_obj,  &x->x_obj.ob_pd, &s_signal, &s_signal);

    x->x_f = 0;
    x->x_sfchannels = nchannels;
    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_requestcondition, 0);
    pthread_cond_init(&x->x_answercondition, 0);
    x->x_vecsize = MAXVECSIZE;
    x->x_insamplerate = x->x_samplerate = 0;
    x->x_state = STATE_IDLE;
    x->x_clock = 0;     /* no callback needed here */
    x->x_canvas = canvas_getcurrent();
    x->x_bytespersample = 2;
    x->x_fd = -1;
    x->x_buf = buf;
    x->x_bufsize = bufsize;
    x->x_fifosize = x->x_fifohead = x->x_fifotail = x->x_requestcode = 0;
    pthread_create(&x->x_childthread, 0, writesf_child_main, x);
    return (x);
}

static t_int *writesf_perform(t_int *w)
{
    t_writesf *x = (t_writesf *)(w[1]);
    int vecsize = x->x_vecsize, sfchannels = x->x_sfchannels,
        bytespersample = x->x_bytespersample,
        bigendian = x->x_bigendian;
    if (x->x_state == STATE_STREAM)
    {
        int wantbytes, roominfifo;
        pthread_mutex_lock(&x->x_mutex);
        wantbytes = sfchannels * vecsize * bytespersample;
        roominfifo = x->x_fifotail - x->x_fifohead;
        if (roominfifo <= 0)
            roominfifo += x->x_fifosize;
        while (roominfifo < wantbytes + 1)
        {
            fprintf(stderr, "writesf waiting for disk write..\n");
            fprintf(stderr, "(head %d, tail %d, room %d, want %d)\n",
                x->x_fifohead, x->x_fifotail, roominfifo, wantbytes);
            sfread_cond_signal(&x->x_requestcondition);
            sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
            fprintf(stderr, "... done waiting.\n");
            roominfifo = x->x_fifotail - x->x_fifohead;
            if (roominfifo <= 0)
                roominfifo += x->x_fifosize;
        }

        soundfile_xferout_sample(sfchannels, x->x_outvec,
            (unsigned char *)(x->x_buf + x->x_fifohead), vecsize, 0,
                bytespersample, bigendian, 1., 1);
        
        x->x_fifohead += wantbytes;
        if (x->x_fifohead >= x->x_fifosize)
            x->x_fifohead = 0;
        if ((--x->x_sigcountdown) <= 0)
        {
#ifdef DEBUG_SOUNDFILE
            pute("signal 1\n");
#endif
            sfread_cond_signal(&x->x_requestcondition);
            x->x_sigcountdown = x->x_sigperiod;
        }
        pthread_mutex_unlock(&x->x_mutex);
    }
    return (w+2);
}

static void writesf_start(t_writesf *x)
{
    /* start making output.  If we're in the "startup" state change
    to the "running" state. */
    if (x->x_state == STATE_STARTUP)
        x->x_state = STATE_STREAM;
    else
        pd_error(x, "writesf: start requested with no prior 'open'");
}

static void writesf_stop(t_writesf *x)
{
        /* LATER rethink whether you need the mutex just to set a Svariable? */
    pthread_mutex_lock(&x->x_mutex);
    x->x_state = STATE_IDLE;
    x->x_requestcode = REQUEST_CLOSE;
#ifdef DEBUG_SOUNDFILE
    pute("signal 2\n");
#endif
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}


    /* open method.  Called as: open [args] filename with args as in
        soundfiler_writeargparse().
    */

static void writesf_open(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
    t_symbol *filesym;
    int filetype, bytespersamp, swap, bigendian, normalize;
    long onset, nframes;
    t_float samplerate;
    if (x->x_state != STATE_IDLE)
    {
        writesf_stop(x);
    }
    if (soundfiler_writeargparse(x, gensym("open"), &argc,
        &argv, &filesym, &filetype, &bytespersamp, &swap, &bigendian,
        &normalize, &onset, &nframes, &samplerate))
    {
            /* errors handled above in soundfiler_writeargparse */
        return;
    }
    if (normalize || onset || (nframes != 0x7fffffff))
        pd_error(x, "normalize/skip/nframes argument to writesf~: ignored");
    if (argc)
        pd_error(x, "extra argument(s) to writesf~: ignored");
    pthread_mutex_lock(&x->x_mutex);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    x->x_bytespersample = bytespersamp;
    x->x_swap = swap;
    x->x_bigendian = bigendian;
    x->x_filename = filesym->s_name;
    x->x_filetype = filetype;
    x->x_itemswritten = 0;
    x->x_requestcode = REQUEST_OPEN;
    x->x_fifotail = 0;
    x->x_fifohead = 0;
    x->x_eof = 0;
    x->x_fileerror = 0;
    x->x_state = STATE_STARTUP;
    x->x_bytespersample = (bytespersamp > 2 ? bytespersamp : 2);
    if (samplerate > 0)
        x->x_samplerate = samplerate;
    else if (x->x_insamplerate > 0)
        x->x_samplerate = x->x_insamplerate;
    else x->x_samplerate = sys_getsr();
        /* set fifosize from bufsize.  fifosize must be a
        multiple of the number of bytes eaten for each DSP
        tick.  */
    x->x_fifosize = x->x_bufsize - (x->x_bufsize %
        (x->x_bytespersample * x->x_sfchannels * MAXVECSIZE));
            /* arrange for the "request" condition to be signalled 16
            times per buffer */
    x->x_sigcountdown = x->x_sigperiod = (x->x_fifosize /
            (16 * x->x_bytespersample * x->x_sfchannels * x->x_vecsize));
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

static void writesf_dsp(t_writesf *x, t_signal **sp)
{
    int i, ninlets = x->x_sfchannels;
    pthread_mutex_lock(&x->x_mutex);
    x->x_vecsize = sp[0]->s_n;
    x->x_sigperiod = (x->x_fifosize /
            (16 * x->x_bytespersample * x->x_sfchannels * x->x_vecsize));
    for (i = 0; i < ninlets; i++)
        x->x_outvec[i] = sp[i]->s_vec;
    x->x_insamplerate = sp[0]->s_sr;
    pthread_mutex_unlock(&x->x_mutex);
    dsp_add(writesf_perform, 1, x);
}

static void writesf_print(t_writesf *x)
{
    post("state %d", x->x_state);
    post("fifo head %d", x->x_fifohead);
    post("fifo tail %d", x->x_fifotail);
    post("fifo size %d", x->x_fifosize);
    post("fd %d", x->x_fd);
    post("eof %d", x->x_eof);
}

static void writesf_free(t_writesf *x)
{
        /* request QUIT and wait for acknowledge */
    void *threadrtn;
    pthread_mutex_lock(&x->x_mutex);
    x->x_requestcode = REQUEST_QUIT;
    /* post("stopping writesf thread..."); */
    sfread_cond_signal(&x->x_requestcondition);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
        /* post("signalling..."); */
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    pthread_mutex_unlock(&x->x_mutex);
    if (pthread_join(x->x_childthread, &threadrtn))
        error("writesf_free: join failed");
    /* post("... done."); */
    
    pthread_cond_destroy(&x->x_requestcondition);
    pthread_cond_destroy(&x->x_answercondition);
    pthread_mutex_destroy(&x->x_mutex);
    freebytes(x->x_buf, x->x_bufsize);
}

static void writesf_setup(void)
{
    writesf_class = class_new(gensym("writesf~"), (t_newmethod)writesf_new, 
        (t_method)writesf_free, sizeof(t_writesf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addmethod(writesf_class, (t_method)writesf_start, gensym("start"), 0);
    class_addmethod(writesf_class, (t_method)writesf_stop, gensym("stop"), 0);
    class_addmethod(writesf_class, (t_method)writesf_dsp, gensym("dsp"),
        A_CANT, 0);
    class_addmethod(writesf_class, (t_method)writesf_open, gensym("open"), 
        A_GIMME, 0);
    class_addmethod(writesf_class, (t_method)writesf_print, gensym("print"), 0);
    CLASS_MAINSIGNALIN(writesf_class, t_writesf, x_f);
}

/* ------------------------ global setup routine ------------------------- */

void d_soundfile_setup(void)
{
    soundfiler_setup();
    readsf_setup();
    writesf_setup();
}

