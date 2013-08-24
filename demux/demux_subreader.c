/*
 * Subtitle reader with format autodetection
 *
 * Copyright (c) 2001 laaz
 * Some code cleanup & realloc() by A'rpi/ESP-team
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>

#include <libavutil/common.h>
#include <libavutil/avstring.h>

#include "config.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/mp_common.h"
#include "mpvcore/options.h"
#include "stream/stream.h"
#include "demux/demux.h"

#define ERR ((void *) -1)

// subtitle formats
#define SUB_INVALID   -1
#define SUB_MICRODVD  0
#define SUB_SUBRIP    1
#define SUB_SUBVIEWER 2
#define SUB_SAMI      3
#define SUB_VPLAYER   4
#define SUB_RT        5
#define SUB_SSA       6
#define SUB_PJS       7
#define SUB_MPSUB     8
#define SUB_AQTITLE   9
#define SUB_SUBVIEWER2 10
#define SUB_SUBRIP09 11
#define SUB_JACOSUB  12
#define SUB_MPL2     13

#define SUB_MAX_TEXT 12
#define SUB_ALIGNMENT_BOTTOMLEFT       1
#define SUB_ALIGNMENT_BOTTOMCENTER     2
#define SUB_ALIGNMENT_BOTTOMRIGHT      3
#define SUB_ALIGNMENT_MIDDLELEFT       4
#define SUB_ALIGNMENT_MIDDLECENTER     5
#define SUB_ALIGNMENT_MIDDLERIGHT      6
#define SUB_ALIGNMENT_TOPLEFT          7
#define SUB_ALIGNMENT_TOPCENTER        8
#define SUB_ALIGNMENT_TOPRIGHT         9

typedef struct subtitle {

    int lines;

    unsigned long start;
    unsigned long end;

    char *text[SUB_MAX_TEXT];
    unsigned char alignment;
} subtitle;

typedef struct sub_data {
    const char *codec;
    subtitle *subtitles;
    int sub_uses_time;
    int sub_num;          // number of subtitle structs
    int sub_errs;
    double fallback_fps;
} sub_data;

// Parameter struct for the format-specific readline functions
struct readline_args {
    int utf16;
    struct MPOpts *opts;

    // subtitle reader state used by some formats

    float mpsub_multiplier;
    float mpsub_position;
    int sub_slacktime;

    int uses_time;

    /*
    Some subtitling formats, namely AQT and Subrip09, define the end of a
    subtitle as the beginning of the following. Since currently we read one
    subtitle at time, for these format we keep two global *subtitle,
    previous_aqt_sub and previous_subrip09_sub, pointing to previous subtitle,
    so we can change its end when we read current subtitle starting time.
    We use a single global unsigned long,
    previous_sub_end, for both (and even future) formats, to store the end of
    the previous sub: it is initialized to 0 in sub_read_file and eventually
    modified by sub_read_aqt_line or sub_read_subrip09_line.
    */
    unsigned long previous_sub_end;
};

/* Maximal length of line of a subtitle */
#define LINE_LEN 1000

static int eol(char p) {
	return p=='\r' || p=='\n' || p=='\0';
}

/* Remove leading and trailing space */
static void trail_space(char *s) {
	int i = 0;
	while (isspace(s[i])) ++i;
	if (i) strcpy(s, s + i);
	i = strlen(s) - 1;
	while (i > 0 && isspace(s[i])) s[i--] = '\0';
}

static char *stristr(const char *haystack, const char *needle) {
    int len = 0;
    const char *p = haystack;

    if (!(haystack && needle)) return NULL;

    len=strlen(needle);
    while (*p != '\0') {
	if (strncasecmp(p, needle, len) == 0) return (char*)p;
	p++;
    }

    return NULL;
}

static void sami_add_line(subtitle *current, char *buffer, char **pos) {
    char *p = *pos;
    *p = 0;
    trail_space(buffer);
    if (*buffer && current->lines < SUB_MAX_TEXT)
        current->text[current->lines++] = strdup(buffer);
    *pos = buffer;
}

static subtitle *sub_read_line_sami(stream_t* st, subtitle *current,
                                    struct readline_args *args)
{
    int utf16 = args->utf16;
    static char line[LINE_LEN+1];
    static char *s = NULL, *slacktime_s;
    char text[LINE_LEN+1], *p=NULL, *q;
    int state;

    current->lines = current->start = current->end = 0;
    current->alignment = SUB_ALIGNMENT_BOTTOMCENTER;
    state = 0;

    /* read the first line */
    if (!s)
	    if (!(s = stream_read_line(st, line, LINE_LEN, utf16))) return 0;

    do {
	switch (state) {

	case 0: /* find "START=" or "Slacktime:" */
	    slacktime_s = stristr (s, "Slacktime:");
	    if (slacktime_s)
                args->sub_slacktime = strtol (slacktime_s+10, NULL, 0) / 10;

	    s = stristr (s, "Start=");
	    if (s) {
		current->start = strtol (s + 6, &s, 0) / 10;
                /* eat '>' */
                for (; *s != '>' && *s != '\0'; s++);
                s++;
		state = 1; continue;
	    }
	    break;

	case 1: /* find (optional) "<P", skip other TAGs */
	    for  (; *s == ' ' || *s == '\t'; s++); /* strip blanks, if any */
	    if (*s == '\0') break;
	    if (*s != '<') { state = 3; p = text; continue; } /* not a TAG */
	    s++;
	    if (*s == 'P' || *s == 'p') { s++; state = 2; continue; } /* found '<P' */
	    for (; *s != '>' && *s != '\0'; s++); /* skip remains of non-<P> TAG */
	    if (*s == '\0')
	      break;
	    s++;
	    continue;

	case 2: /* find ">" */
	    if ((s = strchr (s, '>'))) { s++; state = 3; p = text; continue; }
	    break;

	case 3: /* get all text until '<' appears */
	    if (p - text >= LINE_LEN)
	        sami_add_line(current, text, &p);
	    if (*s == '\0') break;
	    else if (!strncasecmp (s, "<br>", 4)) {
                sami_add_line(current, text, &p);
		s += 4;
	    }
	    else if (*s == '{') { state = 5; ++s; continue; }
	    else if (*s == '<') { state = 4; }
	    else if (!strncasecmp (s, "&nbsp;", 6)) { *p++ = ' '; s += 6; }
	    else if (*s == '\t') { *p++ = ' '; s++; }
	    else if (*s == '\r' || *s == '\n') { s++; }
	    else *p++ = *s++;

	    /* skip duplicated space */
	    if (p > text + 2) if (*(p-1) == ' ' && *(p-2) == ' ') p--;

	    continue;

	case 4: /* get current->end or skip <TAG> */
	    q = stristr (s, "Start=");
	    if (q) {
		current->end = strtol (q + 6, &q, 0) / 10 - 1;
		*p = '\0'; trail_space (text);
		if (text[0] != '\0')
		    current->text[current->lines++] = strdup (text);
		if (current->lines > 0) { state = 99; break; }
		state = 0; continue;
	    }
	    s = strchr (s, '>');
	    if (s) { s++; state = 3; continue; }
	    break;
       case 5: /* get rid of {...} text, but read the alignment code */
	    if ((*s == '\\') && (*(s + 1) == 'a')) {
               if (stristr(s, "\\a1") != NULL) {
                   current->alignment = SUB_ALIGNMENT_BOTTOMLEFT;
                   s = s + 3;
               }
               if (stristr(s, "\\a2") != NULL) {
                   current->alignment = SUB_ALIGNMENT_BOTTOMCENTER;
                   s = s + 3;
               } else if (stristr(s, "\\a3") != NULL) {
                   current->alignment = SUB_ALIGNMENT_BOTTOMRIGHT;
                   s = s + 3;
               } else if ((stristr(s, "\\a4") != NULL) || (stristr(s, "\\a5") != NULL) || (stristr(s, "\\a8") != NULL)) {
                   current->alignment = SUB_ALIGNMENT_TOPLEFT;
                   s = s + 3;
               } else if (stristr(s, "\\a6") != NULL) {
                   current->alignment = SUB_ALIGNMENT_TOPCENTER;
                   s = s + 3;
               } else if (stristr(s, "\\a7") != NULL) {
                   current->alignment = SUB_ALIGNMENT_TOPRIGHT;
                   s = s + 3;
               } else if (stristr(s, "\\a9") != NULL) {
                   current->alignment = SUB_ALIGNMENT_MIDDLELEFT;
                   s = s + 3;
               } else if (stristr(s, "\\a10") != NULL) {
                   current->alignment = SUB_ALIGNMENT_MIDDLECENTER;
                   s = s + 4;
               } else if (stristr(s, "\\a11") != NULL) {
                   current->alignment = SUB_ALIGNMENT_MIDDLERIGHT;
                   s = s + 4;
               }
	    }
	    if (*s == '}') state = 3;
	    ++s;
	    continue;
	}

	/* read next line */
	if (state != 99 && !(s = stream_read_line (st, line, LINE_LEN, utf16))) {
	    if (current->start > 0) {
		break; // if it is the last subtitle
	    } else {
		return 0;
	    }
	}

    } while (state != 99);

    // For the last subtitle
    if (current->end <= 0) {
        current->end = current->start + args->sub_slacktime;
        sami_add_line(current, text, &p);
    }

    return current;
}


static const char *sub_readtext(const char *source, char **dest) {
    int len=0;
    const char *p=source;

//    printf("src=%p  dest=%p  \n",source,dest);

    while ( !eol(*p) && *p!= '|' ) {
	p++,len++;
    }

    *dest= malloc (len+1);
    if (!*dest) {return ERR;}

    strncpy(*dest, source, len);
    (*dest)[len]=0;

    while (*p=='\r' || *p=='\n' || *p=='|') p++;

    if (*p) return p;  // not-last text field
    else return NULL;  // last text field
}

static subtitle *set_multiline_text(subtitle *current, const char *text, int start)
{
    int i = start;
    while ((text = sub_readtext(text, current->text + i))) {
        if (current->text[i] == ERR) return ERR;
        i++;
        if (i >= SUB_MAX_TEXT) {
            mp_msg(MSGT_SUBREADER, MSGL_WARN, "Too many lines in a subtitle\n");
            current->lines = i;
            return current;
        }
    }
    current->lines = i + 1;
    return current;
}

static subtitle *sub_read_line_microdvd(stream_t *st,subtitle *current,
                                        struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    char line2[LINE_LEN+1];

    do {
	if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
    } while ((sscanf (line,
		      "{%ld}{}%[^\r\n]",
		      &(current->start), line2) < 2) &&
	     (sscanf (line,
		      "{%ld}{%ld}%[^\r\n]",
		      &(current->start), &(current->end), line2) < 3));

    return set_multiline_text(current, line2, 0);
}

static subtitle *sub_read_line_mpl2(stream_t *st,subtitle *current,
                                    struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    char line2[LINE_LEN+1];

    do {
	if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
    } while ((sscanf (line,
		      "[%ld][%ld]%[^\r\n]",
		      &(current->start), &(current->end), line2) < 3));
    current->start *= 10;
    current->end *= 10;

    return set_multiline_text(current, line2, 0);
}

static subtitle *sub_read_line_subrip(stream_t* st, subtitle *current,
                                    struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    int a1,a2,a3,a4,b1,b2,b3,b4;
    char *p=NULL, *q=NULL;
    int len;

    while (1) {
	if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
	if (sscanf (line, "%d:%d:%d.%d,%d:%d:%d.%d",&a1,&a2,&a3,&a4,&b1,&b2,&b3,&b4) < 8) continue;
	current->start = a1*360000+a2*6000+a3*100+a4;
	current->end   = b1*360000+b2*6000+b3*100+b4;

	if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;

	p=q=line;
	for (current->lines=1; current->lines < SUB_MAX_TEXT; current->lines++) {
	    for (q=p,len=0; *p && *p!='\r' && *p!='\n' && *p!='|' && strncmp(p,"[br]",4); p++,len++);
	    current->text[current->lines-1]=malloc (len+1);
	    if (!current->text[current->lines-1]) return ERR;
	    strncpy (current->text[current->lines-1], q, len);
	    current->text[current->lines-1][len]='\0';
	    if (!*p || *p=='\r' || *p=='\n') break;
	    if (*p=='|') p++;
	    else while (*p++!=']');
	}
	break;
    }
    return current;
}

static subtitle *sub_read_line_subviewer(stream_t *st, subtitle *current,
                                         struct readline_args *args)
{
    int utf16 = args->utf16;
    int a1, a2, a3, a4, b1, b2, b3, b4, j = 0;

    while (!current->text[0]) {
        char line[LINE_LEN + 1], full_line[LINE_LEN + 1];
        int i;

        /* Parse SubRip header */
        if (!stream_read_line(st, line, LINE_LEN, utf16))
            return NULL;
        if (sscanf(line, "%d:%d:%d%*1[,.:]%d --> %d:%d:%d%*1[,.:]%d",
                     &a1, &a2, &a3, &a4, &b1, &b2, &b3, &b4) < 8)
            continue;

        current->start = a1 * 360000 + a2 * 6000 + a3 * 100 + a4 / 10;
        current->end   = b1 * 360000 + b2 * 6000 + b3 * 100 + b4 / 10;

        /* Concat lines */
        full_line[0] = 0;
        for (i = 0; i < SUB_MAX_TEXT; i++) {
            int blank = 1, len = 0;
            char *p;

            if (!stream_read_line(st, line, LINE_LEN, utf16))
                break;

            for (p = line; *p != '\n' && *p != '\r' && *p; p++, len++)
                if (*p != ' ' && *p != '\t')
                    blank = 0;

            if (blank)
                break;

            *p = 0;

            if (!(j + 1 + len < sizeof(full_line) - 1))
                break;

            if (j != 0)
                full_line[j++] = '\n';
            strcpy(&full_line[j], line);
            j += len;
        }

        if (full_line[0]) {
            current->text[0] = strdup(full_line);
            current->lines = 1;
        }
    }
    return current;
}

static subtitle *sub_read_line_subviewer2(stream_t *st,subtitle *current,
                                          struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    int a1,a2,a3,a4;
    char *p=NULL;
    int i,len;

    while (!current->text[0]) {
        if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
	if (line[0]!='{')
	    continue;
        if ((len=sscanf (line, "{T %d:%d:%d:%d",&a1,&a2,&a3,&a4)) < 4)
            continue;
        current->start = a1*360000+a2*6000+a3*100+a4/10;
        for (i=0; i<SUB_MAX_TEXT;) {
            if (!stream_read_line (st, line, LINE_LEN, utf16)) break;
            if (line[0]=='}') break;
            len=0;
            for (p=line; *p!='\n' && *p!='\r' && *p; ++p,++len);
            if (len) {
                current->text[i]=malloc (len+1);
                if (!current->text[i]) return ERR;
                strncpy (current->text[i], line, len); current->text[i][len]='\0';
                ++i;
            } else {
                break;
            }
        }
        current->lines=i;
    }
    return current;
}


static subtitle *sub_read_line_vplayer(stream_t *st,subtitle *current,
                                       struct readline_args *args)
{
        int utf16 = args->utf16;
	char line[LINE_LEN+1];
	int a1,a2,a3;
	char *p=NULL, separator;
	int len,plen;

	while (!current->text[0]) {
		if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
		if ((len=sscanf (line, "%d:%d:%d%c%n",&a1,&a2,&a3,&separator,&plen)) < 4)
			continue;

		if (!(current->start = a1*360000+a2*6000+a3*100))
			continue;
                /* removed by wodzu
		p=line;
 		// finds the body of the subtitle
 		for (i=0; i<3; i++){
		   p=strchr(p,':');
		   if (p==NULL) break;
		   ++p;
		}
		if (p==NULL) {
		    printf("SUB: Skipping incorrect subtitle line!\n");
		    continue;
		}
                */
                // by wodzu: hey! this time we know what length it has! what is
                // that magic for? it can't deal with space instead of third
                // colon! look, what simple it can be:
                p = &line[ plen ];

		if (*p!='|') {
			//
                        return set_multiline_text(current, p, 0);
		}
	}
	return current;
}

static subtitle *sub_read_line_rt(stream_t *st,subtitle *current,
                                    struct readline_args *args)
{
    int utf16 = args->utf16;

	//TODO: This format uses quite rich (sub/super)set of xhtml
	// I couldn't check it since DTD is not included.
	// WARNING: full XML parses can be required for proper parsing
    char line[LINE_LEN+1];
    int a1,a2,a3,a4,b1,b2,b3,b4;
    char *p=NULL,*next=NULL;
    int len,plen;

    while (!current->text[0]) {
	if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
	//TODO: it seems that format of time is not easily determined, it may be 1:12, 1:12.0 or 0:1:12.0
	//to describe the same moment in time. Maybe there are even more formats in use.
	//if ((len=sscanf (line, "<Time Begin=\"%d:%d:%d.%d\" End=\"%d:%d:%d.%d\"",&a1,&a2,&a3,&a4,&b1,&b2,&b3,&b4)) < 8)
	plen=a1=a2=a3=a4=b1=b2=b3=b4=0;
	if (
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d.%d\" %*[Ee]nd=\"%d.%d\"%*[^<]<clear/>%n",&a3,&a4,&b3,&b4,&plen)) < 4) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d.%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",&a3,&a4,&b2,&b3,&b4,&plen)) < 5) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d\" %*[Ee]nd=\"%d:%d\"%*[^<]<clear/>%n",&a2,&a3,&b2,&b3,&plen)) < 4) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",&a2,&a3,&b2,&b3,&b4,&plen)) < 5) &&
//	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\" %*[Ee]nd=\"%d:%d\"%*[^<]<clear/>%n",&a2,&a3,&a4,&b2,&b3,&plen)) < 5) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\" %*[Ee]nd=\"%d:%d.%d\"%*[^<]<clear/>%n",&a2,&a3,&a4,&b2,&b3,&b4,&plen)) < 6) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d:%d.%d\" %*[Ee]nd=\"%d:%d:%d.%d\"%*[^<]<clear/>%n",&a1,&a2,&a3,&a4,&b1,&b2,&b3,&b4,&plen)) < 8) &&
	//now try it without end time
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d.%d\"%*[^<]<clear/>%n",&a3,&a4,&plen)) < 2) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d\"%*[^<]<clear/>%n",&a2,&a3,&plen)) < 2) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d.%d\"%*[^<]<clear/>%n",&a2,&a3,&a4,&plen)) < 3) &&
	((len=sscanf (line, "<%*[tT]ime %*[bB]egin=\"%d:%d:%d.%d\"%*[^<]<clear/>%n",&a1,&a2,&a3,&a4,&plen)) < 4)
	)
	    continue;
	current->start = a1*360000+a2*6000+a3*100+a4/10;
	current->end   = b1*360000+b2*6000+b3*100+b4/10;
	if (b1 == 0 && b2 == 0 && b3 == 0 && b4 == 0)
	  current->end = current->start+200;
	p=line;	p+=plen;
	// TODO: I don't know what kind of convention is here for marking multiline subs, maybe <br/> like in xml?
	next = strstr(line,"<clear/>");
	if(next && strlen(next)>8){
	  next+=8;
          return set_multiline_text(current, next, 0);
	}
    }
    return current;
}

static subtitle *sub_read_line_ssa(stream_t *st,subtitle *current,
                                    struct readline_args *args)
{
    /* Instead of hardcoding the expected fields and their order on
     * each dialogue line, this code should parse the "Format: " line
     * which lists the fields used in the script. As is, this may not
     * work correctly with all scripts.
     */

        int utf16 = args->utf16;
        int comma;

	int hour1, min1, sec1, hunsec1,
	    hour2, min2, sec2, hunsec2, nothing;
	int num;

	char line[LINE_LEN+1],
	     line3[LINE_LEN+1],
	     *line2;
	char *tmp;

	do {
		if (!stream_read_line (st, line, LINE_LEN, utf16)) return NULL;
	} while (sscanf (line, "Dialogue: Marked=%d,%d:%d:%d.%d,%d:%d:%d.%d"
			"%[^\n\r]", &nothing,
			&hour1, &min1, &sec1, &hunsec1,
			&hour2, &min2, &sec2, &hunsec2,
			line3) < 9
		 &&
		 sscanf (line, "Dialogue: %d,%d:%d:%d.%d,%d:%d:%d.%d"
			 "%[^\n\r]", &nothing,
			 &hour1, &min1, &sec1, &hunsec1,
			 &hour2, &min2, &sec2, &hunsec2,
			 line3) < 9	    );

        line2=strchr(line3, ',');
        if (!line2) return NULL;

        for (comma = 3; comma < 9; comma ++)
            if (!(line2 = strchr(++line2, ',')))
                return NULL;
        line2++;

	current->lines=0;num=0;
	current->start = 360000*hour1 + 6000*min1 + 100*sec1 + hunsec1;
	current->end   = 360000*hour2 + 6000*min2 + 100*sec2 + hunsec2;

        while (((tmp=strstr(line2, "\\n")) != NULL) || ((tmp=strstr(line2, "\\N")) != NULL) ){
		current->text[num]=malloc(tmp-line2+1);
		strncpy (current->text[num], line2, tmp-line2);
		current->text[num][tmp-line2]='\0';
		line2=tmp+2;
		num++;
		current->lines++;
		if (current->lines >=  SUB_MAX_TEXT) return current;
	}

	current->text[num]=strdup(line2);
	current->lines++;

	return current;
}

/*
 * PJS subtitles reader.
 * That's the "Phoenix Japanimation Society" format.
 * I found some of them in http://www.scriptsclub.org/ (used for anime).
 * The time is in tenths of second.
 *
 * by set, based on code by szabi (dunnowhat sub format ;-)
 */
static subtitle *sub_read_line_pjs(stream_t *st,subtitle *current,
                                   struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    char text[LINE_LEN+1], *s, *d;

    if (!stream_read_line (st, line, LINE_LEN, utf16))
	return NULL;
    /* skip spaces */
    for (s=line; *s && isspace(*s); s++);
    /* allow empty lines at the end of the file */
    if (*s==0)
	return NULL;
    /* get the time */
    if (sscanf (s, "%ld,%ld,", &(current->start),
		&(current->end)) <2) {
	return ERR;
    }
    /* the files I have are in tenths of second */
    current->start *= 10;
    current->end *= 10;
    /* walk to the beggining of the string */
    for (; *s; s++) if (*s==',') break;
    if (*s) {
	for (s++; *s; s++) if (*s==',') break;
	if (*s) s++;
    }
    if (*s!='"') {
	return ERR;
    }
    /* copy the string to the text buffer */
    for (s++, d=text; *s && *s!='"'; s++, d++)
	*d=*s;
    *d=0;
    current->text[0] = strdup(text);
    current->lines = 1;

    return current;
}

static subtitle *sub_read_line_mpsub(stream_t *st, subtitle *current,
                                     struct readline_args *args)
{
        int utf16 = args->utf16;
	char line[LINE_LEN+1];
	float a,b;
	int num=0;
	char *p, *q;

	do
	{
		if (!stream_read_line(st, line, LINE_LEN, utf16)) return NULL;
	} while (sscanf (line, "%f %f", &a, &b) !=2);

	args->mpsub_position += a*args->mpsub_multiplier;
	current->start=(int) args->mpsub_position;
	args->mpsub_position += b*args->mpsub_multiplier;
	current->end=(int) args->mpsub_position;

	while (num < SUB_MAX_TEXT) {
		if (!stream_read_line (st, line, LINE_LEN, utf16)) {
			if (num == 0) return NULL;
			else return current;
		}
		p=line;
		while (isspace(*p)) p++;
		if (eol(*p) && num > 0) return current;
		if (eol(*p)) return NULL;

		for (q=p; !eol(*q); q++);
		*q='\0';
		if (strlen(p)) {
			current->text[num]=strdup(p);
//			printf (">%s<\n",p);
			current->lines = ++num;
		} else {
			if (num) return current;
			else return NULL;
		}
	}
	return NULL; // we should have returned before if it's OK
}

static subtitle *sub_read_line_aqt(stream_t *st,subtitle *current,
                                   struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];

retry:
    while (1) {
    // try to locate next subtitle
        if (!stream_read_line (st, line, LINE_LEN, utf16))
		return NULL;
        if (!(sscanf (line, "-->> %ld", &(current->start)) <1))
		break;
    }

    if (!args->previous_sub_end)
    args->previous_sub_end = (current->start) ? current->start - 1 : 0;

    if (!stream_read_line (st, line, LINE_LEN, utf16))
	return NULL;

    sub_readtext((char *) &line,&current->text[0]);
    current->lines = 1;
    current->end = current->start; // will be corrected by next subtitle

    if (!stream_read_line (st, line, LINE_LEN, utf16))
	return current;

    if (set_multiline_text(current, line, 1) == ERR)
        return ERR;

    if (!strlen(current->text[0]) && !strlen(current->text[1]))
	goto retry;

    return current;
}

static subtitle *sub_read_line_subrip09(stream_t *st,subtitle *current,
                                    struct readline_args *args)
{
    int utf16 = args->utf16;
    char line[LINE_LEN+1];
    int a1,a2,a3;
    int len;

retry:
    while (1) {
    // try to locate next subtitle
        if (!stream_read_line (st, line, LINE_LEN, utf16))
		return NULL;
        if (!((len=sscanf (line, "[%d:%d:%d]",&a1,&a2,&a3)) < 3))
		break;
    }

    current->start = a1*360000+a2*6000+a3*100;

    if (!args->previous_sub_end)
        args->previous_sub_end = (current->start) ? current->start - 1 : 0;

    if (!stream_read_line (st, line, LINE_LEN, utf16))
	return NULL;

    current->text[0]=""; // just to be sure that string is clear

    if (set_multiline_text(current, line, 0) == ERR)
        return ERR;

    if (!strlen(current->text[0]) && current->lines <= 1)
	goto retry;

    return current;
}

static subtitle *sub_read_line_jacosub(stream_t* st, subtitle * current,
                                       struct readline_args *args)
{
    int utf16 = args->utf16;
    char line1[LINE_LEN], line2[LINE_LEN], directive[LINE_LEN], *p, *q;
    unsigned a1, a2, a3, a4, b1, b2, b3, b4, comment = 0;
    static unsigned jacoTimeres = 30;
    static int jacoShift = 0;

    memset(current, 0, sizeof(subtitle));
    memset(line1, 0, LINE_LEN);
    memset(line2, 0, LINE_LEN);
    memset(directive, 0, LINE_LEN);
    while (!current->text[0]) {
	if (!stream_read_line(st, line1, LINE_LEN, utf16)) {
	    return NULL;
	}
	if (sscanf
	    (line1, "%u:%u:%u.%u %u:%u:%u.%u %[^\n\r]", &a1, &a2, &a3, &a4,
	     &b1, &b2, &b3, &b4, line2) < 9) {
	    if (sscanf(line1, "@%u @%u %[^\n\r]", &a4, &b4, line2) < 3) {
		if (line1[0] == '#') {
		    int hours = 0, minutes = 0, seconds, delta, inverter =
			1;
		    unsigned units = jacoShift;
		    switch (toupper(line1[1])) {
		    case 'S':
			if (isalpha(line1[2])) {
			    delta = 6;
			} else {
			    delta = 2;
			}
			if (sscanf(&line1[delta], "%d", &hours)) {
			    if (hours < 0) {
				hours *= -1;
				inverter = -1;
			    }
			    if (sscanf(&line1[delta], "%*d:%d", &minutes)) {
				if (sscanf
				    (&line1[delta], "%*d:%*d:%d",
				     &seconds)) {
				    sscanf(&line1[delta], "%*d:%*d:%*d.%d",
					   &units);
				} else {
				    hours = 0;
				    sscanf(&line1[delta], "%d:%d.%d",
					   &minutes, &seconds, &units);
				    minutes *= inverter;
				}
			    } else {
				hours = minutes = 0;
				sscanf(&line1[delta], "%d.%d", &seconds,
				       &units);
				seconds *= inverter;
			    }
			    jacoShift =
				((hours * 3600 + minutes * 60 +
				  seconds) * jacoTimeres +
				 units) * inverter;
			}
			break;
		    case 'T':
			if (isalpha(line1[2])) {
			    delta = 8;
			} else {
			    delta = 2;
			}
			sscanf(&line1[delta], "%u", &jacoTimeres);
			break;
		    }
		}
		continue;
	    } else {
		current->start =
		    (unsigned long) ((a4 + jacoShift) * 100.0 /
				     jacoTimeres);
		current->end =
		    (unsigned long) ((b4 + jacoShift) * 100.0 /
				     jacoTimeres);
	    }
	} else {
	    current->start =
		(unsigned
		 long) (((a1 * 3600 + a2 * 60 + a3) * jacoTimeres + a4 +
			 jacoShift) * 100.0 / jacoTimeres);
	    current->end =
		(unsigned
		 long) (((b1 * 3600 + b2 * 60 + b3) * jacoTimeres + b4 +
			 jacoShift) * 100.0 / jacoTimeres);
	}
	current->lines = 0;
	p = line2;
	while ((*p == ' ') || (*p == '\t')) {
	    ++p;
	}
	if (isalpha(*p)||*p == '[') {
	    int cont, jLength;

	    if (sscanf(p, "%s %[^\n\r]", directive, line1) < 2)
		return (subtitle *) ERR;
	    jLength = strlen(directive);
	    for (cont = 0; cont < jLength; ++cont) {
		if (isalpha(*(directive + cont)))
		    *(directive + cont) = toupper(*(directive + cont));
	    }
	    if ((strstr(directive, "RDB") != NULL)
		|| (strstr(directive, "RDC") != NULL)
		|| (strstr(directive, "RLB") != NULL)
		|| (strstr(directive, "RLG") != NULL)) {
		continue;
	    }
	    if (strstr(directive, "JL") != NULL) {
		current->alignment = SUB_ALIGNMENT_BOTTOMLEFT;
	    } else if (strstr(directive, "JR") != NULL) {
		current->alignment = SUB_ALIGNMENT_BOTTOMRIGHT;
	    } else {
		current->alignment = SUB_ALIGNMENT_BOTTOMCENTER;
	    }
	    strcpy(line2, line1);
	    p = line2;
	}
	for (q = line1; (!eol(*p)) && (current->lines < SUB_MAX_TEXT); ++p) {
	    switch (*p) {
	    case '{':
		comment++;
		break;
	    case '}':
		if (comment) {
		    --comment;
		    //the next line to get rid of a blank after the comment
		    if ((*(p + 1)) == ' ')
			p++;
		}
		break;
	    case '~':
		if (!comment) {
		    *q = ' ';
		    ++q;
		}
		break;
	    case ' ':
	    case '\t':
		if ((*(p + 1) == ' ') || (*(p + 1) == '\t'))
		    break;
		if (!comment) {
		    *q = ' ';
		    ++q;
		}
		break;
	    case '\\':
		if (*(p + 1) == 'n') {
		    *q = '\0';
		    q = line1;
		    current->text[current->lines++] = strdup(line1);
		    ++p;
		    break;
		}
		if ((toupper(*(p + 1)) == 'C')
		    || (toupper(*(p + 1)) == 'F')) {
		    ++p,++p;
		    break;
		}
		if ((*(p + 1) == 'B') || (*(p + 1) == 'b') || (*(p + 1) == 'D') ||	//actually this means "insert current date here"
		    (*(p + 1) == 'I') || (*(p + 1) == 'i') || (*(p + 1) == 'N') || (*(p + 1) == 'T') ||	//actually this means "insert current time here"
		    (*(p + 1) == 'U') || (*(p + 1) == 'u')) {
		    ++p;
		    break;
		}
		if ((*(p + 1) == '\\') ||
		    (*(p + 1) == '~') || (*(p + 1) == '{')) {
		    ++p;
		} else if (eol(*(p + 1))) {
		    if (!stream_read_line(st, directive, LINE_LEN, utf16))
			return NULL;
		    trail_space(directive);
		    av_strlcat(line2, directive, LINE_LEN);
		    break;
		}
	    default:
		if (!comment) {
		    *q = *p;
		    ++q;
		}
	    }			//-- switch
	}			//-- for
	*q = '\0';
	if (current->lines < SUB_MAX_TEXT)
	    current->text[current->lines] = strdup(line1);
    }				//-- while
    if (current->lines < SUB_MAX_TEXT)
        current->lines++;
    return current;
}

static int sub_autodetect (stream_t* st, int *uses_time, int utf16) {
    char line[LINE_LEN+1];
    int i,j=0;

    while (j < 100) {
	j++;
	if (!stream_read_line (st, line, LINE_LEN, utf16))
	    return SUB_INVALID;

	if (sscanf (line, "{%d}{%d}", &i, &i)==2)
		{*uses_time=0;return SUB_MICRODVD;}
	if (sscanf (line, "{%d}{}", &i)==1)
		{*uses_time=0;return SUB_MICRODVD;}
	if (sscanf (line, "[%d][%d]", &i, &i)==2)
		{*uses_time=1;return SUB_MPL2;}
	if (sscanf (line, "%d:%d:%d.%d,%d:%d:%d.%d",     &i, &i, &i, &i, &i, &i, &i, &i)==8)
		{*uses_time=1;return SUB_SUBRIP;}
	if (sscanf (line, "%d:%d:%d%*1[,.:]%d --> %d:%d:%d%*1[,.:]%d", &i, &i, &i, &i, &i, &i, &i, &i) == 8)
		{*uses_time=1;return SUB_SUBVIEWER;}
	if (sscanf (line, "{T %d:%d:%d:%d",&i, &i, &i, &i)==4)
		{*uses_time=1;return SUB_SUBVIEWER2;}
	if (strstr (line, "<SAMI>"))
		{*uses_time=1; return SUB_SAMI;}
	if (sscanf(line, "%d:%d:%d.%d %d:%d:%d.%d", &i, &i, &i, &i, &i, &i, &i, &i) == 8)
		{*uses_time = 1; return SUB_JACOSUB;}
	if (sscanf(line, "@%d @%d", &i, &i) == 2)
		{*uses_time = 1; return SUB_JACOSUB;}
	if (sscanf (line, "%d:%d:%d:",     &i, &i, &i )==3)
		{*uses_time=1;return SUB_VPLAYER;}
	if (sscanf (line, "%d:%d:%d ",     &i, &i, &i )==3)
		{*uses_time=1;return SUB_VPLAYER;}
	if (!strncasecmp(line, "<window", 7))
		{*uses_time=1;return SUB_RT;}
	if (!memcmp(line, "Dialogue: Marked", 16))
		{*uses_time=1; return SUB_SSA;}
	if (!memcmp(line, "Dialogue: ", 10))
		{*uses_time=1; return SUB_SSA;}
	if (sscanf (line, "%d,%d,\"%c", &i, &i, (char *) &i) == 3)
		{*uses_time=1;return SUB_PJS;}
	if (sscanf (line, "FORMAT=%d", &i) == 1)
		{*uses_time=0; return SUB_MPSUB;}
	if (!memcmp(line, "FORMAT=TIME", 11))
		{*uses_time=1; return SUB_MPSUB;}
	if (strstr (line, "-->>"))
		{*uses_time=0; return SUB_AQTITLE;}
	if (sscanf (line, "[%d:%d:%d]", &i, &i, &i)==3)
		{*uses_time=1;return SUB_SUBRIP09;}
    }

    return SUB_INVALID;  // too many bad lines
}

static void adjust_subs_time(subtitle* sub, float subtime, float fps,
                             float sub_fps, int block,
                             int sub_num, int sub_uses_time) {
	int n,m;
	subtitle* nextsub;
	int i = sub_num;
	unsigned long subfms = (sub_uses_time ? 100 : fps) * subtime;

	n=m=0;
	if (i)	for (;;){
		if (sub->end <= sub->start){
			sub->end = sub->start + subfms;
			m++;
			n++;
		}
		if (!--i) break;
		nextsub = sub + 1;
	    if(block){
		if (sub->end >= nextsub->start){
			sub->end = nextsub->start - 1;
			if (sub->end - sub->start > subfms)
				sub->end = sub->start + subfms;
			if (!m)
				n++;
		}
	    }

		sub = nextsub;
		m = 0;
	}
	if (n) mp_msg(MSGT_SUBREADER,MSGL_V,"SUB: Adjusted %d subtitle(s).\n", n);
}

struct subreader {
    subtitle * (*read)(stream_t *st, subtitle *dest,
                       struct readline_args *args);
    void       (*post)(subtitle *dest);
    const char *name;
    const char *codec_name;
    struct readline_args args;
};

static bool subreader_autodetect(stream_t *fd, struct MPOpts *opts,
                                 struct subreader *out)
{
    static const struct subreader sr[]=
    {
	    { sub_read_line_microdvd, NULL, "microdvd", "microdvd" },
	    { sub_read_line_subrip, NULL, "subviewer" },
	    { sub_read_line_subviewer, NULL, "subrip", "subrip" },
	    { sub_read_line_sami, NULL, "sami" },
	    { sub_read_line_vplayer, NULL, "vplayer" },
	    { sub_read_line_rt, NULL, "rt" },
	    { sub_read_line_ssa, NULL, "ssa", "ass-text" },
	    { sub_read_line_pjs, NULL, "pjs" },
	    { sub_read_line_mpsub, NULL, "mpsub" },
	    { sub_read_line_aqt, NULL, "aqt" },
	    { sub_read_line_subviewer2, NULL, "subviewer 2.0" },
	    { sub_read_line_subrip09, NULL, "subrip 0.9" },
	    { sub_read_line_jacosub, NULL, "jacosub" },
	    { sub_read_line_mpl2, NULL, "mpl2" }
    };
    const struct subreader *srp;

    int sub_format = SUB_INVALID;
    int utf16;
    int uses_time = 0;
    for (utf16 = 0; sub_format == SUB_INVALID && utf16 < 3; utf16++) {
        sub_format=sub_autodetect (fd, &uses_time, utf16);
        stream_seek(fd,0);
    }
    utf16--;

    if (sub_format==SUB_INVALID) {
        mp_msg(MSGT_SUBREADER,MSGL_V,"SUB: Could not determine file format\n");
        return false;
    }
    srp=sr+sub_format;
    mp_msg(MSGT_SUBREADER, MSGL_V, "SUB: Detected subtitle file format: %s\n", srp->name);

    *out = *srp;
    out->args = (struct readline_args) {
        .utf16 = utf16,
        .opts = opts,
        .sub_slacktime = 20000, //20 sec
        .mpsub_multiplier = (uses_time ? 100.0 : 1.0),
        .uses_time = uses_time,
    };

    return true;
}

static sub_data* sub_read_file(stream_t *fd, struct subreader *srp)
{
    struct MPOpts *opts = fd->opts;
    float fps = 23.976;
    int n_max, i, j;
    subtitle *first, *sub, *return_sub, *alloced_sub = NULL;
    sub_data *subt_data;
    int sub_num = 0, sub_errs = 0;
    struct readline_args args = srp->args;

    sub_num=0;n_max=32;
    first=malloc(n_max*sizeof(subtitle));
    if (!first)
        abort();

    alloced_sub =
    sub = malloc(sizeof(subtitle));
    //This is to deal with those formats (AQT & Subrip) which define the end of a subtitle
    //as the beginning of the following
    args.previous_sub_end = 0;
    while(1){
        if(sub_num>=n_max){
            n_max+=16;
            first=realloc(first,n_max*sizeof(subtitle));
        }
	memset(sub, '\0', sizeof(subtitle));
        sub=srp->read(fd, sub, &args);
        if(!sub) break;   // EOF

	if ( sub == ERR )
	 {
	  free(first);
	  free(alloced_sub);
	  return NULL;
	 }
        // Apply any post processing that needs recoding first
        if ((sub!=ERR) && srp->post) srp->post(sub);
	if(!sub_num || (first[sub_num - 1].start <= sub->start)){
	    first[sub_num].start = sub->start;
  	    first[sub_num].end   = sub->end;
	    first[sub_num].lines = sub->lines;
	    first[sub_num].alignment = sub->alignment;
  	    for(i = 0; i < sub->lines; ++i){
		first[sub_num].text[i] = sub->text[i];
  	    }
	    if (args.previous_sub_end){
  		first[sub_num - 1].end = args.previous_sub_end;
    		args.previous_sub_end = 0;
	    }
	} else {
	    for(j = sub_num - 1; j >= 0; --j){
    		first[j + 1].start = first[j].start;
    		first[j + 1].end   = first[j].end;
		first[j + 1].lines = first[j].lines;
		first[j + 1].alignment = first[j].alignment;
    		for(i = 0; i < first[j].lines; ++i){
      		    first[j + 1].text[i] = first[j].text[i];
		}
		if(!j || (first[j - 1].start <= sub->start)){
	    	    first[j].start = sub->start;
	    	    first[j].end   = sub->end;
	    	    first[j].lines = sub->lines;
	    	    first[j].alignment = sub->alignment;
	    	    for(i = 0; i < SUB_MAX_TEXT; ++i){
			first[j].text[i] = sub->text[i];
		    }
		    if (args.previous_sub_end){
			first[j].end = first[j - 1].end;
			first[j - 1].end = args.previous_sub_end;
			args.previous_sub_end = 0;
		    }
		    break;
    		}
	    }
	}
        if(sub==ERR) ++sub_errs; else ++sub_num; // Error vs. Valid
    }

    free(alloced_sub);

//    printf ("SUB: Subtitle format %s time.\n", uses_time?"uses":"doesn't use");
    mp_msg(MSGT_SUBREADER, MSGL_V,"SUB: Read %i subtitles, %i bad line(s).\n",
           sub_num, sub_errs);

    if(sub_num<=0){
	free(first);
	return NULL;
    }

    adjust_subs_time(first, 6.0, fps, opts->sub_fps, 1, sub_num, args.uses_time);/*~6 secs AST*/
    return_sub = first;

    if (return_sub == NULL) return NULL;
    subt_data = talloc_zero(NULL, sub_data);
    subt_data->codec = srp->codec_name ? srp->codec_name : "text";
    subt_data->sub_uses_time = args.uses_time;
    subt_data->sub_num = sub_num;
    subt_data->sub_errs = sub_errs;
    subt_data->subtitles = return_sub;
    subt_data->fallback_fps = fps;
    return subt_data;
}

static void subdata_free(sub_data *subd)
{
    int i, j;
    for (i = 0; i < subd->sub_num; i++)
        for (j = 0; j < subd->subtitles[i].lines; j++)
            free( subd->subtitles[i].text[j] );
    free( subd->subtitles );
    talloc_free(subd);
}

struct priv {
    struct demux_packet **pkts;
    int num_pkts;
    int current;
    struct sh_stream *sh;
};

static void add_sub_data(struct demuxer *demuxer, struct sub_data *subdata)
{
    struct priv *priv = demuxer->priv;

    for (int i = 0; i < subdata->sub_num; i++) {
        subtitle *st = &subdata->subtitles[i];
        // subdata is in 10 ms ticks, pts is in seconds
        double t = subdata->sub_uses_time ? 0.01 : (1 / subdata->fallback_fps);

        int len = 0;
        for (int j = 0; j < st->lines; j++)
            len += st->text[j] ? strlen(st->text[j]) : 0;

        len += 2 * st->lines;   // '\N', including the one after the last line
        len += 6;               // {\anX}
        len += 1;               // '\0'

        char *data = talloc_array(NULL, char, len);

        char *p = data;
        char *end = p + len;

        if (st->alignment)
            p += snprintf(p, end - p, "{\\an%d}", st->alignment);

        for (int j = 0; j < st->lines; j++)
            p += snprintf(p, end - p, "%s\\N", st->text[j]);

        if (st->lines > 0)
            p -= 2;             // remove last "\N"
        *p = 0;

        struct demux_packet *pkt = talloc_ptrtype(priv, pkt);
        *pkt = (struct demux_packet) {
            .pts = st->start * t,
            .duration = (st->end - st->start) * t,
            .buffer = talloc_steal(pkt, data),
            .len = strlen(data),
        };

        MP_TARRAY_APPEND(priv, priv->pkts, priv->num_pkts, pkt);
    }
}

static struct stream *read_probe_stream(struct stream *s, int max)
{
    // Very roundabout, but only needed for initial probing.
    bstr probe = stream_peek(s, max);
    return open_memory_stream(probe.start, probe.len);
}

#define PROBE_SIZE FFMIN(32 * 1024, STREAM_MAX_BUFFER_SIZE)

static int d_open_file(struct demuxer *demuxer, enum demux_check check)
{
    if (check > DEMUX_CHECK_REQUEST)
        return -1;

    struct stream *ps = read_probe_stream(demuxer->stream, PROBE_SIZE);

    struct subreader sr;
    bool res = subreader_autodetect(ps, demuxer->opts, &sr);

    free_stream(ps);

    if (!res)
        return -1;

    demuxer->filetype = sr.name;

    sub_data *sd = sub_read_file(demuxer->stream, &sr);
    if (!sd)
        return -1;

    struct priv *p = talloc_zero(demuxer, struct priv);
    demuxer->priv = p;

    p->sh = new_sh_stream(demuxer, STREAM_SUB);
    p->sh->codec = sd->codec;
    p->sh->sub->frame_based = !sd->sub_uses_time;
    p->sh->sub->is_utf8 = sr.args.utf16 != 0; // converted from utf-16 -> utf-8

    add_sub_data(demuxer, sd);
    subdata_free(sd);

    return 0;
}

static int d_fill_buffer(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    struct demux_packet *dp = demux_packet_list_fill(p->pkts, p->num_pkts,
                                                     &p->current);
    return demuxer_add_packet(demuxer, p->sh, dp);
}

static void d_seek(struct demuxer *demuxer, float secs, float audio_delay,
                   int flags)
{
    struct priv *p = demuxer->priv;
    demux_packet_list_seek(p->pkts, p->num_pkts, &p->current, secs, flags);
}

static int d_control(struct demuxer *demuxer, int cmd, void *arg)
{
    struct priv *p = demuxer->priv;
    switch (cmd) {
    case DEMUXER_CTRL_GET_TIME_LENGTH:
        *((double *) arg) = demux_packet_list_duration(p->pkts, p->num_pkts);
        return DEMUXER_CTRL_OK;
    default:
        return DEMUXER_CTRL_NOTIMPL;
    }
}

const struct demuxer_desc demuxer_desc_subreader = {
    .name = "subreader",
    .desc = "Deprecated MPlayer subreader",
    .open = d_open_file,
    .fill_buffer = d_fill_buffer,
    .seek = d_seek,
    .control = d_control,
};
