/*
 * RTTY tone generator
 * (C) 2000-2024 Adam C Bernstein
 * All Rights Reserved
 *
 * Tone synthysis code originally written by Itai Nahshon.
 * dtmf-encode_bit 0.1
 * (C) 1998 Itai Nahshon (nahshon@actcom.co.il)
 *
 * Use and redistribution are subject to the GNU GENERAL PUBLIC LICENSE.
 */
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#ifdef ASOUNDLIB_H
#include <alsa/asoundlib.h>
#else
#include <alsa-utils.h>
#endif

static int resample = 1;                                /* enable alsa-lib resampling */
static snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
static unsigned int channels = 1;                       /* count of channels */
static unsigned int rate = 44100;                       /* stream rate */
static unsigned int buffer_time = 500000;               /* ring buffer length in us */
static snd_pcm_sframes_t buffer_size;
static unsigned int period_time = 100000;               /* period time in us */
static snd_pcm_sframes_t period_size;
static int period_event = 0;                            /* produce poll event after each period */



#define BAUD_DELAY_45 22    /* 22ms = 45 baud, 60WPM */
#define BAUD_DELAY_50 20    /* 20ms = 50 baud, 66WPM */
#define BAUD_DELAY_57 18    /* 18ms = 56.9 baud, 75WPM  */
#define BAUD_DELAY_74 13    /* 13ms = 74 baud, 100WPM */

#define COLUMN_MAX 76

#define COS_OFFSET 32767

#define FREQ_LO_HZ 950
#define FREQ_HI_HZ 1070

#define CHAR_A          0
#define CHAR_Z          25
#define CHAR_NULL       26
#define CHAR_LF         27
#define CHAR_SPACE      28
#define CHAR_CR         29
#define CHAR_SHIFT_UP   30
#define CHAR_SHIFT_DOWN 31
#define CHAR_OPEN       32
#define CHAR_CLOSED     33

#define CHAR_0          15
#define CHAR_1          16
#define CHAR_2          22
#define CHAR_3           4
#define CHAR_4          17
#define CHAR_5          19
#define CHAR_6          24
#define CHAR_7          20
#define CHAR_8           8
#define CHAR_9          14
 
#define CHAR_DASH        0
#define CHAR_QUESTION    1
#define CHAR_COLON       2
#define CHAR_DOLLAR      3
#define CHAR_BELL        6
#define CHAR_APOSTROPHE  9
#define CHAR_LPHAREN    10
#define CHAR_RPHAREN    11
#define CHAR_PERIOD     12
#define CHAR_COMMA      13
#define CHAR_SEMICOLON  21
#define CHAR_SOLIDUS    23
#define CHAR_QUOTE      25

#define BSIZE 4096
#define MINTABSIZE 2
#define MAXTABSIZE 65536

typedef struct rtty_conf
{
    char *output;
    char *filename;
    int bits;
    int speed;
    int volume;
    int format;
    int use_audio;
    int PCM_MAX;
    unsigned char *buf;
    int bufsize;
    snd_pcm_uframes_t frames;
    snd_pcm_uframes_t write_total;
    snd_pcm_hw_params_t *hwparams; /* Maybe not needed here */
    snd_pcm_sw_params_t *swparams;
    snd_pcm_t *pcm;
    signed short *costab;
    int tabsize;
    int bit_delay;
    int wpm;
    int fsk_shift;
    int shift;
    int freq_high;
    int freq_low;
    int column;
    int bufidx;
} rtty_conf;

void gen_costab(rtty_conf *);
void write_freq_to_alsa(rtty_conf *ctx, int f1, int msec);

void encode_bit(rtty_conf *ctx, int c);
void encode_to_baudot(rtty_conf *ctx, char c);
void encode_bits(rtty_conf *ctx, char *bits, int len);
int ascii_2_baudot(char c, char *baudot, int *shift);
void print_char(rtty_conf *ctx, char c);
void initialize_tty(rtty_conf *ctx);
void pause_print(rtty_conf *ctx, int count);
void print_line(rtty_conf *ctx, char *line);
void print_file(rtty_conf *ctx, char *name);
int set_raw(int fd, struct termios *old_mode);
void keyboard_io(rtty_conf *ctx);


void rtty_conf_init(rtty_conf *ctx)
{
    ctx->output = "plughw:0,0";
    ctx->bits = 16;
    ctx->speed = 44100;
    ctx->bit_delay = 22; /* 22ms = 45 baud */
    ctx->volume = 100;
    ctx->format = SND_PCM_FORMAT_S16_LE;
    ctx->use_audio = 0;
    ctx->PCM_MAX = 0;
    ctx->buf = NULL;
    ctx->bufsize = BSIZE;
    ctx->frames = 0;
    ctx->write_total = 0;
    ctx->costab = NULL;
    ctx->tabsize = 1024 * 8; 

    switch (ctx->wpm)
    {
      case 60:
        ctx->bit_delay = BAUD_DELAY_45;
        break;
      case 66:
        ctx->bit_delay = BAUD_DELAY_50;
        break;
      case 75:
        ctx->bit_delay = BAUD_DELAY_57;
        break;
      case 100:
        ctx->bit_delay = BAUD_DELAY_74;
        break;
      default:
        ctx->bit_delay = BAUD_DELAY_45;
        break;
    }

    if (ctx->freq_low == 0)
    {
        ctx->freq_low = FREQ_LO_HZ;
    }
    switch (ctx->fsk_shift)
    {
      case 170:
        ctx->freq_high = ctx->freq_low + 170;
        break;
      case 425:
        ctx->freq_high = ctx->freq_low + 425;
        break;
      case 850:
        ctx->freq_high = ctx->freq_low + 850;
        break;
        break;

      default:
        ctx->freq_high = ctx->freq_low + 170;
        break;
    }
}

void
Usage(void) {
    fprintf(stderr, "usage: rtty-alsa [options] number ...\n"
            " Valid options with their default values are:\n"
            "   Duration options:\n"
            "     --silent-time 50\n"
            "     --sleep-time  500\n"
            "   Audio output  options:\n"
            "     --output-dev audio_dev | - [stdout]\n"
            "     --use-audio   1\n"
            "     --speed       8000\n"
            "     --bits        8\n"
            "   Audio generation options:\n"
            "     --volume      100\n"
            "   RTTY options:\n"
            "     --input-file\n"
            "     --test-data\n"
            "     --keyboard\n"
            "     --wpm 60 | 66 | 75 | 100\n"
            "     --freq 500-3000\n"
            "     --shift 170 | 425 | 850\n"
            );
            
    exit(1);
}


void
getvalue(int *arg, int *index, int argc,
     char **argv, int min, int max) 
{
    if (*index >= argc-1)
        Usage();

    *arg = atoi(argv[1+*index]);

    if (*arg < min || *arg > max) {
        fprintf(stderr, "Value for %s should be in the range %d..%d\n", 
                argv[*index]+2, min, max);
        exit(1);
    }
    ++*index;
}

void test_generator(rtty_conf *ctx)
{
    int i = 0;
    char line[] =
        "the quick brown fox jumped over the lazy dog's back 1234567890\n"
        "ryryryryryryryryryryryryryryryryryryryryryryryryryryryryryryry\n"
        "sgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsgsg\n"
        "ryryryryryryryryryryryryryryryryryryryryryryryryryryryryryryry\n";

    initialize_tty(ctx);
    for (i=0; line[i]; i++) {
        print_char(ctx, line[i]);
    }
    initialize_tty(ctx);
    write_freq_to_alsa(ctx, ctx->freq_high, 2000);
}

static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
        int err;
        /* get the current swparams */
        err = snd_pcm_sw_params_current(handle, swparams);
        if (err < 0) {
                printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* start the transfer when the buffer is almost full: */
        /* (buffer_size / avail_min) * avail_min */
        err = snd_pcm_sw_params_set_start_threshold(handle, swparams, (buffer_size / period_size) * period_size);
        if (err < 0) {
                printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* allow the transfer when at least period_size samples can be processed */
        /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
        err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_event ? buffer_size : period_size);
        if (err < 0) {
                printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* enable period events when requested */
        if (period_event) {
                err = snd_pcm_sw_params_set_period_event(handle, swparams, 1);
                if (err < 0) {
                        printf("Unable to set period event: %s\n", snd_strerror(err));
                        return err;
                }
        }
        /* write the parameters to the playback device */
        err = snd_pcm_sw_params(handle, swparams);
        if (err < 0) {
                printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
                return err;
        }
        return 0;
}
static int set_hwparams(snd_pcm_t *handle,
                        snd_pcm_hw_params_t *params,
                        snd_pcm_access_t access)
{
        unsigned int rrate;
        snd_pcm_uframes_t size;
        int err, dir;
        /* choose all parameters */
        err = snd_pcm_hw_params_any(handle, params);
        if (err < 0) {
                printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
                return err;
        }
        /* set hardware resampling */
        err = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
        if (err < 0) {
                printf("Resampling setup failed for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* set the interleaved read/write format */
        err = snd_pcm_hw_params_set_access(handle, params, access);
        if (err < 0) {
                printf("Access type not available for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* set the sample format */
        err = snd_pcm_hw_params_set_format(handle, params, format);
        if (err < 0) {
                printf("Sample format not available for playback: %s\n", snd_strerror(err));
                return err;
        }
        /* set the count of channels */
        err = snd_pcm_hw_params_set_channels(handle, params, channels);
        if (err < 0) {
                printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
                return err;
        }
        /* set the stream rate */
        rrate = rate;
        err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
        if (err < 0) {
                printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
                return err;
        }
        if (rrate != rate) {
                printf("Rate doesn't match (requested %iHz, get %iHz)\n", rate, err);
                return -EINVAL;
        }
        /* set the buffer time */
        err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
        if (err < 0) {
                printf("Unable to set buffer time %i for playback: %s\n", buffer_time, snd_strerror(err));
                return err;
        }
        err = snd_pcm_hw_params_get_buffer_size(params, &size);
        if (err < 0) {
                printf("Unable to get buffer size for playback: %s\n", snd_strerror(err));
                return err;
        }
        buffer_size = size;
        /* set the period time */
        err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
        if (err < 0) {
                printf("Unable to set period time %i for playback: %s\n", period_time, snd_strerror(err));
                return err;
        }
        err = snd_pcm_hw_params_get_period_size(params, &size, &dir);
        if (err < 0) {
                printf("Unable to get period size for playback: %s\n", snd_strerror(err));
                return err;
        }
        period_size = size;
        /* write the parameters to device */
        err = snd_pcm_hw_params(handle, params);
        if (err < 0) {
                printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
                return err;
        }
        return 0;
}

int main(int argc, char **argv)
{
    int i;
    int test_data = 0;
    int keyboard = 0;
    int sts = 0;
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hwparams = NULL;
    snd_pcm_sw_params_t *swparams = NULL;
    int channels = 1;
    int sample_size = sizeof(short);
    int fd = 0;
    rtty_conf ctx = {0};
    char *audio_device = "default";

    for(i = 1; i < argc; i++) {
        if (argv[i][0] != '-' ||
           argv[i][1] != '-')
            break;

        if (!strcmp(argv[i], "--keyboard")) {
            keyboard = 1;
        }
        else if (!strcmp(argv[i], "--test-data")) {
            test_data = 1;
        }
        else if (!strcmp(argv[i], "--volume")) {
            getvalue(&ctx.volume, &i, argc, argv,
                 0, 100);
        }
        else if (!strcmp(argv[i], "--speed")) {
            getvalue(&ctx.speed, &i, argc, argv,
                 5000, 48000);
        }
        else if (!strcmp(argv[i], "--wpm")) {
            getvalue(&ctx.wpm, &i, argc, argv,
                 10, 10000);
        }
        else if (!strcmp(argv[i], "--shift")) {
            getvalue(&ctx.fsk_shift, &i, argc, argv,
                 10, 1000);
        }
        else if (!strcmp(argv[i], "--freq")) {
            getvalue(&ctx.freq_low, &i, argc, argv,
                 500, 3000);
        }
        else if (!strcmp(argv[i], "--bits")) {
            getvalue(&ctx.bits, &i, argc, argv,
                 8, 16);
        }
        else if (!strcmp(argv[i], "--use-audio")) {
            getvalue(&ctx.use_audio, &i, argc, argv,
                 0, 1);
        }
        else if (!strcmp(argv[i], "--input-file")) {
            i++;
            if (i >= argc)
                Usage();
            ctx.filename = argv[i];
        }
        else if (!strcmp(argv[i], "--output-dev")) {
            i++;
            if (i >= argc)
                Usage();
            ctx.output = argv[i];
        }
        else {
            Usage();
        }
    }

    rtty_conf_init(&ctx);
    if (strcmp(ctx.output, "-") == 0)
        fd = 1;     /* stdout */

    fd = fd;


    switch(ctx.bits) {
        case 8:
            ctx.format = SND_PCM_FORMAT_U8;
            sample_size = sizeof(char);
            break;
        case 16:
            ctx.format = SND_PCM_FORMAT_S16_LE;
            sample_size = sizeof(short);
            break;
        default:
            fprintf(stderr, "Value for bits should be 8 or 16\n");
            return(1);
    }

    sts = snd_pcm_open(&pcm, audio_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (sts)
    {
        printf("alsa_init: failed %d\n", sts);
        return 1;
    }
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    ctx.pcm = pcm;
    ctx.hwparams = hwparams;
    ctx.swparams = swparams;

    if ((sts = set_hwparams(pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
            printf("Setting of hwparams failed: %s\n", snd_strerror(sts));
            exit(EXIT_FAILURE);
    }
    if ((sts = set_swparams(pcm, swparams)) < 0) {
            printf("Setting of swparams failed: %s\n", snd_strerror(sts));
            exit(EXIT_FAILURE);
    }

    ctx.frames = period_size;
    ctx.bufsize = ctx.frames * channels * sample_size;
    ctx.buf = (unsigned char *) malloc(ctx.bufsize);
    ctx.PCM_MAX = snd_pcm_avail_update(ctx.pcm);

    gen_costab(&ctx);
    ctx.buf = malloc(ctx.bufsize);
    if (ctx.buf == NULL) {
        perror("malloc buf");
        return(1);
    }

    /* Load data into ALSA sound buffer */
    write_freq_to_alsa(&ctx, ctx.freq_high, 500);

    if (test_data) {
        test_generator(&ctx);
    }
    else {
        pause_print(&ctx, 10);
        initialize_tty(&ctx);

        if (keyboard) 
        {
            keyboard_io(&ctx);
        }
        else if (ctx.filename) {
            print_file(&ctx, ctx.filename);
        }
        else {
            while (i < argc) {
                print_line(&ctx, argv[i]);
            }
        }

        initialize_tty(&ctx);
        pause_print(&ctx, 10);
    }
    snd_pcm_drain(ctx.pcm);
    snd_pcm_close(ctx.pcm);
    return 0;
}


void keyboard_io(rtty_conf *ctx)
{
    fd_set rmask;
    struct timeval tv;
    char c;
    int n;
    struct termios old;
    int pcm_bits = 0;

    set_raw(0, &old);
    do {
        FD_ZERO(&rmask);
        FD_SET(0, &rmask);
        tv.tv_sec = 0;
        tv.tv_usec = 176000; /* Time to send one character 8*22ms */
        tv.tv_usec = 100000; /* Timeout to send blank data every 100ms */
        n = select(1, &rmask, NULL, NULL, &tv);
        if (n > 0) {
            read(0, &c, 1);
            if (c == '\r' || c == '\n' ||
                (ctx->column && ((ctx->column % COLUMN_MAX) == 0)))
            {
                encode_to_baudot(ctx, CHAR_CR);
                encode_to_baudot(ctx, CHAR_LF);
                printf("\r\n");
                ctx->column = 0;
            }
            else {
                print_char(ctx, c);
            }
            fflush(stdout);
        }
        else {
            /* 
             * Feedback loop to prevent PCM underrun when no data is
             * entered from the keyboard. Send 150ms worth of blank
             * data. This is slightly more than the select() timeout,
             * so an underrun should never happen.
             */
            pcm_bits = snd_pcm_avail_update(ctx->pcm);
            if (pcm_bits > (ctx->PCM_MAX - ctx->speed/2))
            {
                write_freq_to_alsa(ctx, ctx->freq_high, 150);
            }
        }
    } while (c != 'Z');
    tcsetattr(0, TCSANOW, &old);
}

void print_file(rtty_conf *ctx, char *name)
{
    FILE *fp;
    char *line;

    fp = fopen(name, "r");
    if (!fp) {
        perror("fopen");
        return;
    }
    line = (char *) malloc(1024);
    if (!line) {
        perror("malloc");
        fclose(fp);
        return;
    }
    fgets(line, 1022, fp);
    while (!feof(fp)) {
        print_line(ctx, line);
        fgets(line, 1022, fp);
    }
    
    fclose(fp);
    free(line);
}


void print_line(rtty_conf *ctx, char *line)
{
    char *cp;
    if (!line) return;

    for (cp=line; *cp; cp++) {
        if (isspace(*cp) || isalnum(*cp) || ispunct(*cp)) {
            print_char(ctx, *cp);
        }
    }
}


void initialize_tty(rtty_conf *ctx)
{
    encode_to_baudot(ctx, CHAR_NULL);
    encode_to_baudot(ctx, CHAR_NULL);
    encode_to_baudot(ctx, CHAR_SHIFT_DOWN);
    encode_to_baudot(ctx, CHAR_CR);
    encode_to_baudot(ctx, CHAR_LF);
}


void pause_print(rtty_conf *ctx, int count)
{
    int i;
    for (i=0; i<count; i++) {
        encode_to_baudot(ctx, CHAR_CLOSED);
    }
}


void print_char(rtty_conf *ctx, char c)
{
    char baudot[16];
    char *bp;
    int cnt;
    struct timeval tv;
    int pcm_bits = 0;
    int sent = 0;

    while (!sent)
    {
        /* 
         * Feedback loop to prevent PCM underrun when no data is
         * entered from the keyboard. Send 150ms worth of blank
         * data. This is slightly more than the select() timeout,
         * so an underrun should never happen.
         */
        pcm_bits = snd_pcm_avail_update(ctx->pcm);
        if (1 || pcm_bits > (ctx->PCM_MAX - ctx->speed/4))
        {
            cnt = ascii_2_baudot(c, baudot, &ctx->shift);
            bp = baudot;
            while (cnt > 0) {
                encode_to_baudot(ctx, *bp);
                bp++;
                cnt--;
            }
            sent = 1;
            if (isspace(c) || isalnum(c) || ispunct(c))
            {
                ctx->column++;
                if (c == '\n' || c == '\r')
                {
                    ctx->column = 0;
                    printf("\r\n");
                }
                else
                {
                    putchar((char) toupper((int) c));
                }
                fflush(stdout);
            }

            if (ctx->column >= COLUMN_MAX) {
                encode_to_baudot(ctx, CHAR_CR);
                encode_to_baudot(ctx, CHAR_LF);
                encode_to_baudot(ctx, CHAR_CR);
                printf("\r\n");
                ctx->column = 0;
            }
        }
        else
        {
printf("timeout!!!!\n");
            tv.tv_sec = 0;
            tv.tv_usec = 100000; /* Timeout to send blank data every 100ms */
            select(0, NULL, NULL, NULL, &tv);
        }
    }
}


int
ascii_2_baudot(char c, char *baudot, int *shift)
{
    int i;
    char *p = baudot;

    /* Convert punction characters first */
    switch (c) {
      case '-':
        *p++ = CHAR_DASH;
        break;
      case '?':
        *p++ = CHAR_QUESTION;
        break;
      case ':':
        *p++ = CHAR_COLON;
        break;
      case '$':
        *p++ = CHAR_DOLLAR;
        break;
      case 7:
        *p++ = CHAR_BELL;
        break;
      case '\'':
      case '`':
        *p++ = CHAR_APOSTROPHE;
        break;
      case '(':
        *p++ = CHAR_LPHAREN;
        break;
      case ')':
        *p++ = CHAR_RPHAREN;
        break;
      case '.':
        *p++ = CHAR_PERIOD;
        break;
      case ',':
        *p++ = CHAR_COMMA;
        break;
      case ';':
        *p++ = CHAR_SEMICOLON;
        break;
      case '/':
        *p++ = CHAR_SOLIDUS;
        break;
      case '"':
        *p++ = CHAR_QUOTE;
        break;
    }

    /* p has advanced one byte if we have found a character to convert */

    if (p != baudot) {
        /*
         * Prefix the converted character with shift up if not 
         * already shifted
         */
        if (!*shift) {
            p[0] = p[-1];
            p[-1] = CHAR_SHIFT_UP;
            p++;
            *shift = 1;
        }
    }
    else {

        /* The current character has not been mapped */

        i = (int) c;
        if (c == ' ') {
            *p++ = CHAR_SPACE;
        }
        else if (c == '\n') {
            *p++ = CHAR_CR;
            *p++ = CHAR_LF;
        }
        else if (isdigit(c)) {
            if (!*shift) {
                *shift = 1;
                *p++ = CHAR_SHIFT_UP;
            }
            i = (int) (c - '0');
            switch(i) {
              case 0:
                *p++ = CHAR_0;
                break;
              case 1:
                *p++ = CHAR_1;
                break;
              case 2:
                *p++ = CHAR_2;
                break;
              case 3:
                *p++ = CHAR_3;
                break;
              case 4:
                *p++ = CHAR_4;
                break;
              case 5:
                *p++ = CHAR_5;
                break;
              case 6:
                *p++ = CHAR_6;
                break;
              case 7:
                *p++ = CHAR_7;
                break;
              case 8:
                *p++ = CHAR_8;
                break;
              case 9:
                *p++ = CHAR_9;
                break;
            }
        }
        else if (isalpha(c)) {
            if (*shift) {
                *shift = 0;
                *p++ = CHAR_SHIFT_DOWN;
            }
            *p++ = (char) (toupper(c) - 'A');
        }
        else if (i>=CHAR_NULL && i<=CHAR_CLOSED) {
           *p++ = i;
        }
        else {
            /*
             * There is no reasonable mapping for this character
             */
            *p++ = CHAR_NULL;
        }
    }
    return p - baudot;
}

void
encode_to_baudot(rtty_conf *ctx, char c)
{
    int i;

static char baudot_bits[][8] = {
    {0, 1, 1, 0, 0, 0, 1, 1}, /* A */
    {0, 1, 0, 0, 1, 1, 1, 1}, /* B */
    {0, 0, 1, 1, 1, 0, 1, 1}, /* C */
    {0, 1, 0, 0, 1, 0, 1, 1}, /* D */
    {0, 1, 0, 0, 0, 0, 1, 1}, /* E / 3 */
    {0, 1, 0, 1, 1, 0, 1, 1}, /* F */
    {0, 0, 1, 0, 1, 1, 1, 1}, /* G */
    {0, 0, 0, 1, 0, 1, 1, 1}, /* H */
    {0, 0, 1, 1, 0, 0, 1, 1}, /* I  / 8 */
    {0, 1, 1, 0, 1, 0, 1, 1}, /* J */
    {0, 1, 1, 1, 1, 0, 1, 1}, /* K */
    {0, 0, 1, 0, 0, 1, 1, 1}, /* L */
    {0, 0, 0, 1, 1, 1, 1, 1}, /* M / . */
    {0, 0, 0, 1, 1, 0, 1, 1}, /* N */
    {0, 0, 0, 0, 1, 1, 1, 1}, /* O / 9 */
    {0, 0, 1, 1, 0, 1, 1, 1}, /* P / 0 */
    {0, 1, 1, 1, 0, 1, 1, 1}, /* Q / 1 */
    {0, 0, 1, 0, 1, 0, 1, 1}, /* R / 4 */
    {0, 1, 0, 1, 0, 0, 1, 1}, /* S */
    {0, 0, 0, 0, 0, 1, 1, 1}, /* T / 5 */
    {0, 1, 1, 1, 0, 0, 1, 1}, /* U / 7 */
    {0, 0, 1, 1, 1, 1, 1, 1}, /* V */
    {0, 1, 1, 0, 0, 1, 1, 1}, /* W / 2 */
    {0, 1, 0, 1, 1, 1, 1, 1}, /* X / / */
    {0, 1, 0, 1, 0, 1, 1, 1}, /* Y / 6 */
    {0, 1, 0, 0, 0, 1, 1, 1}, /* Z */
    {0, 0, 0, 0, 0, 0, 1, 1}, /* NULL */
    {0, 0, 1, 0, 0, 0, 1, 1}, /* LF */
    {0, 0, 0, 1, 0, 0, 1, 1}, /* SPACE */
    {0, 0, 0, 0, 1, 0, 1, 1}, /* CR */
    {0, 1, 1, 0, 1, 1, 1, 1}, /* SHIFT_UP */
    {0, 1, 1, 1, 1, 1, 1, 1}, /* SHIFT_DOWN */
    {0, 0, 0, 0, 0, 0, 0, 0}, /* Open */
    {1, 1, 1, 1, 1, 1, 1, 1}  /* closed */
};


    i = (int) c;
    if (i>=0 && i<(sizeof(baudot_bits)/8)) {
       encode_bits(ctx, baudot_bits[i], 8);
    }
}


void encode_bits(rtty_conf *ctx, char *bits, int len)
{
    int i;

    for (i=0; i < len; i++) {
        encode_bit(ctx, bits[i]);
    }
}



void
encode_bit(rtty_conf *ctx, int c) 
{
    switch(c) {
    case 0:
        write_freq_to_alsa(ctx, ctx->freq_low, ctx->bit_delay);
        break;
    case 1:
        write_freq_to_alsa(ctx, ctx->freq_high, ctx->bit_delay);
        break;
    }
}


void
write_freq_to_alsa(rtty_conf *ctx, int f1, int msec)
{
    int d1, e1, g1;
    int time;
    int val;
    int sts = 0;
    int frame_size = 1;
    /*
     * This variable must be static, to insure that the two tone signals
     * will remain in phase when switching between frequencies.
     */
    static int i1;

    if (msec <= 0)
        return;

    f1 *= ctx->tabsize;
    d1 = f1 / ctx->speed;
    g1 = f1 - d1 * ctx->speed;
    e1 = ctx->speed/2;


    time = (msec * ctx->speed) / 1000;
    while(--time >= 0) {
        val = ctx->costab[i1];

        if (ctx->format == SND_PCM_FORMAT_U8) {
            ctx->buf[ctx->bufidx++] = 128 + (val >> 8); 
            frame_size = 1;
        }
        else if (ctx->format == SND_PCM_FORMAT_S16_LE) {
            ctx->buf[ctx->bufidx++] = val & 0xff;
            ctx->buf[ctx->bufidx++] = (val>>8) & 0xff;
            frame_size = 2;
        }

        i1 += d1;
        if (e1 < 0) {
            e1 += ctx->speed;
            i1 += 1;
        }
        if (i1 >= ctx->tabsize)
            i1 -= ctx->tabsize;

        if (ctx->bufidx >= ctx->frames) {
            sts = snd_pcm_writei(ctx->pcm, ctx->buf, ctx->frames/frame_size);
            if (sts < 0)
            {
                printf("snd_pcm_writei 1: error %d\n", sts);
                snd_pcm_prepare(ctx->pcm);
            }
            ctx->bufidx = 0;
        }
        e1 -= g1;
    }
}

void
gen_costab(rtty_conf *ctx)
{
    int i = 0;
    double d = 0.0;

    ctx->costab = (signed short *) malloc(ctx->tabsize * sizeof(signed short));
    if (ctx->costab == NULL) 
    {
        perror("malloc costab");
        exit(1);
    }

    for (i = 0; i < ctx->tabsize; i++) 
    {
        d = 2*M_PI*i;
        ctx->costab[i] = (int)((ctx->volume/100.0)*COS_OFFSET*cos(d/ctx->tabsize));
    }
}



/*
 *  Put terminal into raw mode.
 *  The "definitive" set raw code?  I don't know about that
 *  but it works.
 *
 *  Until proven otherwise this is the way to put a pty into raw mode.
 *  This has been tested by streaming binary data into and from
 *  a pty after being set into raw mode with this code, and it
 *  was completely unmodified by the pty.  Earlier versions of
 *  set_raw() could not claim this fact.
 */
int set_raw(int fd, struct termios *old_mode)
{
    struct termios mode;

    if (!isatty(fd)) {
        return (-1);
    }

    tcgetattr(fd, &mode);
    if (old_mode) {
        tcgetattr(fd, old_mode);
    }

    mode.c_iflag     = 0;
    mode.c_oflag    &= ~OPOST;
    mode.c_lflag    &= ~(IEXTEN | ISIG | ICANON | ECHO | XCASE);
    mode.c_cflag    &= ~(CSIZE | PARENB);
    mode.c_cflag    |= CS8;
    mode.c_cc[VMIN]  = 1;
    mode.c_cc[VTIME] = 1;

    tcsetattr(fd, TCSANOW, &mode);
    return 0;
}
